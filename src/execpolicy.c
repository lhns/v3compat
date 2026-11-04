/* What happens at exec, and at the places that name the executable. DynamoRIO handles execve only,
 * and virtualizes only readlink of /proc/self/exe; the rest is rewritten here, on the way into the
 * kernel. Refusals and the shared judgement: exec_check.h. */
#include "dr_api.h"
#include "drmgr.h"
#include "exec_check.h"
#include "execpolicy.h"

enum { PATHBUF = 4096, FD_MIN = 1000 };
/* Spells execveat's private fd copy, so the child never closes an fd the app itself exec'd through. */
#define DUP_PREFIX "/proc/self/fd/./"

typedef struct {
    reg_t save[6]; /* rdi rsi rdx r10 r8 r9 as the app set them */
    bool rewritten;
    int dupfd;     /* execveat's private copy of the fd, closed if the exec fails */
    char path[PATHBUF];
    struct v3c_exec x;
} tls_t;

static int tls_idx;
/* This process's executable, held open so that it reads even if it has been deleted or is a memfd. The
 * app can see this descriptor in /proc/self/fd. */
static file_t held = INVALID_FILE;
/* The kernel itself runs an empty argv as one empty string (Linux 5.18); DR reads argv[0] unchecked. */
static char *empty_vec[2] = {"", 0};

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static bool eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static bool starts(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return false;
    return true;
}
static char *put_int(char *p, long v) {
    char t[24];
    int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static void fd_path(char *out, long fd) { *put_int(put_str(out, "/proc/self/fd/"), fd) = 0; }

/* The app's string at src, at most max-1 bytes; -1 if it faults, -2 if it does not fit. */
static long read_str(const char *src, char *dst, size_t max) {
    size_t n = 0;
    while (n + 1 < max) {
        size_t chunk = 4096 - ((ptr_uint_t)(src + n) & 4095), got = 0;
        if (chunk > max - 1 - n) chunk = max - 1 - n;
        if (!dr_safe_read(src + n, chunk, dst + n, &got) && got == 0) return -1;
        for (size_t i = 0; i < got; i++)
            if (dst[n + i] == 0) return (long)(n + i);
        n += got;
        if (got < chunk) return -1;
    }
    return -2;
}

/* "/proc/self/exe", "/proc/thread-self/exe" or "/proc/<n>/exe" with n a task of this process. */
static bool is_self_exe(const char *p) {
    long n = 0;
    size_t l = slen(p);
    char task[48], *q;
    struct v3c_stat st;
    if (eq(p, "/proc/self/exe") || eq(p, "/proc/thread-self/exe")) return true;
    if (l < 11 || l > 17 || !starts(p, "/proc/") || !eq(p + l - 4, "/exe")) return false;
    for (const char *d = p + 6; d < p + l - 4; d++) {
        if (*d < '0' || *d > '9') return false;
        n = n * 10 + (*d - '0');
    }
    q = put_str(task, "/proc/self/task/");
    *put_int(q, n) = 0;
    return V3C_SC(V3C_SYS_stat, task, &st, 0) == 0;
}

/* Where the app's own executable can be opened; false if it cannot. */
static bool exe_target(char *out) {
    if (held == INVALID_FILE) return false;
    fd_path(out, held);
    return true;
}

static void save_regs(void *dc, tls_t *t) {
    dr_mcontext_t mc = {sizeof(mc), DR_MC_INTEGER};
    dr_get_mcontext(dc, &mc);
    t->save[0] = mc.xdi; t->save[1] = mc.xsi; t->save[2] = mc.xdx;
    t->save[3] = mc.r10; t->save[4] = mc.r8; t->save[5] = mc.r9;
    t->rewritten = true;
}

/* The syscall is skipped and returns -err. */
static bool fail_with(void *dc, int err) {
    dr_syscall_set_result(dc, -err);
    return false;
}

static void refuse(void *dc, const struct v3c_exec *x) {
    dr_fprintf(STDERR, "v3compat: refusing to exec %s: %s\n", x->final, x->why);
    /* An app-issued exit is DR's normal path, also from a vfork child; dr_exit_process is not. */
    dr_syscall_set_sysnum(dc, V3C_SYS_exit_group);
    dr_syscall_set_param(dc, 0, V3COMPAT_REFUSED_EXEC);
}

/* 1: run the exec on; 0: the syscall is already decided (refused or failed). */
static int judge(void *dc, tls_t *t, const char *path) {
    v3c_judge(path, &t->x);
    if (t->x.verdict == V3C_REFUSE) { refuse(dc, &t->x); return 0; }
    if (t->x.verdict == V3C_FAIL) { fail_with(dc, t->x.err); return 0; }
    return 1;
}

static bool pre_execve(void *dc, tls_t *t) {
    const char *app_path = (const char *)dr_syscall_get_param(dc, 0);
    char self[PATHBUF];
    const char *judged = t->path;
    if (read_str(app_path, t->path, PATHBUF) < 0) return true; /* the kernel reports EFAULT or ENAMETOOLONG */
    if (is_self_exe(t->path) && exe_target(self)) judged = self;
    if (!judge(dc, t, judged)) return t->x.verdict == V3C_REFUSE;
    /* DR reads argv for scripts and does not check it. */
    if (dr_syscall_get_param(dc, 1) == 0) {
        save_regs(dc, t);
        dr_syscall_set_param(dc, 1, (reg_t)empty_vec);
    }
    return true;
}

static bool pre_execveat(void *dc, tls_t *t) {
    int dirfd = (int)dr_syscall_get_param(dc, 0);
    const char *app_path = (const char *)dr_syscall_get_param(dc, 1);
    reg_t argv = dr_syscall_get_param(dc, 2), envp = dr_syscall_get_param(dc, 3);
    long flags = (long)dr_syscall_get_param(dc, 4);
    long n = read_str(app_path, t->path, PATHBUF);
    t->dupfd = -1;
    if (n < 0) return fail_with(dc, n == -1 ? 14 /* EFAULT */ : 36 /* ENAMETOOLONG */);
    if (flags & ~(long)(V3C_AT_EMPTY_PATH | V3C_AT_SYMLINK_NOFOLLOW)) return fail_with(dc, V3C_EINVAL);
    if (n == 0) {
        struct v3c_stat a, b;
        char fdp[40], target[PATHBUF], hdr[2];
        long r;
        if (!(flags & V3C_AT_EMPTY_PATH)) return fail_with(dc, V3C_ENOENT);
        if (V3C_SC(V3C_SYS_fstat, dirfd, &a, 0) != 0) return fail_with(dc, V3C_EBADF);
        /* A script run from a close-on-exec fd cannot be opened by its interpreter: natively ENOENT. */
        if ((V3C_SC(V3C_SYS_fcntl, dirfd, 1 /* F_GETFD */, 0) & 1) &&
            v3c_sc(V3C_SYS_pread64, dirfd, (long)hdr, 2, 0, 0, 0) == 2 && hdr[0] == '#' && hdr[1] == '!')
            return fail_with(dc, V3C_ENOENT);
        fd_path(fdp, dirfd);
        r = V3C_SC(V3C_SYS_readlink, fdp, target, PATHBUF - 1);
        if (r > 0 && target[0] == '/') {
            target[r] = 0;
            if (V3C_SC(V3C_SYS_stat, target, &b, 0) == 0 && a.dev == b.dev && a.ino == b.ino)
                put_str(t->path, target)[0] = 0;
            else
                r = -1;
        } else {
            r = -1;
        }
        if (r < 0) {
            /* memfd, deleted or renamed: a copy of the fd, which the exec'd client closes again. */
            r = v3c_sc(V3C_SYS_fcntl, dirfd, 0 /* F_DUPFD */, FD_MIN, 0, 0, 0);
            if (r < 0) r = v3c_sc(V3C_SYS_fcntl, dirfd, 0 /* F_DUPFD */, 3, 0, 0, 0);
            if (r < 0) return fail_with(dc, (int)-r);
            t->dupfd = (int)r;
            *put_int(put_str(t->path, DUP_PREFIX), r) = 0;
        }
    } else if (t->path[0] != '/' && dirfd != V3C_AT_FDCWD) {
        char dp[40];
        size_t dl;
        fd_path(dp, dirfd);
        dl = slen(dp);
        if (dl + 1 + (size_t)n >= PATHBUF) return fail_with(dc, 36);
        for (long i = n; i >= 0; i--) t->path[dl + 1 + (size_t)i] = t->path[i];
        for (size_t i = 0; i < dl; i++) t->path[i] = dp[i];
        t->path[dl] = '/';
    }
    if ((flags & V3C_AT_SYMLINK_NOFOLLOW) && n > 0) {
        struct v3c_stat st;
        if (v3c_sc(V3C_SYS_newfstatat, V3C_AT_FDCWD, (long)t->path, (long)&st, V3C_AT_SYMLINK_NOFOLLOW, 0, 0) == 0 &&
            (st.mode & 0170000) == 0120000)
            return fail_with(dc, V3C_ELOOP);
    }
    if (!judge(dc, t, t->path)) {
        if (t->dupfd >= 0) V3C_SC(V3C_SYS_close, t->dupfd, 0, 0);
        t->dupfd = -1;
        return t->x.verdict == V3C_REFUSE;
    }
    save_regs(dc, t);
    dr_syscall_set_sysnum(dc, V3C_SYS_execve);
    dr_syscall_set_param(dc, 0, (reg_t)t->path);
    dr_syscall_set_param(dc, 1, argv ? argv : (reg_t)empty_vec);
    dr_syscall_set_param(dc, 2, envp);
    return true;
}

/* The calls that follow /proc/self/exe: the index of the path argument and of the flags (-1: none). */
static bool open_like(int sysnum, int *pi, int *fi) {
    *fi = -1;
    switch (sysnum) {
    case V3C_SYS_stat: case V3C_SYS_access: case V3C_SYS_open: case V3C_SYS_readlink: *pi = 0; return true;
    case V3C_SYS_openat: case V3C_SYS_openat2: case V3C_SYS_faccessat: case V3C_SYS_readlinkat: *pi = 1; return true;
    case V3C_SYS_newfstatat: case V3C_SYS_faccessat2: *pi = 1; *fi = 3; return true;
    case V3C_SYS_statx: *pi = 1; *fi = 2; return true;
    }
    return false;
}

static void pre_open_like(void *dc, tls_t *t, int sysnum, int pi, int fi) {
    const char *app_path = (const char *)dr_syscall_get_param(dc, pi);
    char head[6];
    size_t got = 0;
    bool rl = sysnum == V3C_SYS_readlink || sysnum == V3C_SYS_readlinkat;
    if (!rl && held == INVALID_FILE) return;
    if (fi >= 0 && ((long)dr_syscall_get_param(dc, fi) & V3C_AT_SYMLINK_NOFOLLOW)) return;
    if (!dr_safe_read(app_path, 6, head, &got) || got != 6 || !starts(head, "/proc/")) return;
    if (read_str(app_path, t->path, 64) < 0 || !is_self_exe(t->path)) return;
    if (rl) {
        /* DR answers readlink of /proc/self/exe itself, but not of the thread-self spelling. */
        if (!eq(t->path, "/proc/thread-self/exe")) return;
        put_str(t->path, "/proc/self/exe")[0] = 0;
    } else if (!exe_target(t->path)) {
        return;
    }
    save_regs(dc, t);
    dr_syscall_set_param(dc, pi, (reg_t)t->path);
}

static bool event_filter(void *dc, int sysnum) {
    int pi, fi;
    (void)dc;
    return sysnum == V3C_SYS_clone3 || sysnum == V3C_SYS_execve || sysnum == V3C_SYS_execveat ||
           open_like(sysnum, &pi, &fi);
}

static bool event_pre(void *dc, int sysnum) {
    tls_t *t = (tls_t *)drmgr_get_tls_field(dc, tls_idx);
    int pi, fi;
    t->rewritten = false;
    t->dupfd = -1;
    /* A static glibc posix_spawn child dies under DR's clone3. libcs fall back to clone, as under Docker's
     * default seccomp, which answers the same. */
    if (sysnum == V3C_SYS_clone3) return fail_with(dc, 38 /* ENOSYS */);
    if (sysnum == V3C_SYS_execve) return pre_execve(dc, t);
    if (sysnum == V3C_SYS_execveat) return pre_execveat(dc, t);
    if (open_like(sysnum, &pi, &fi)) pre_open_like(dc, t, sysnum, pi, fi);
    return true;
}

/* A syscall that comes back leaves the registers as the kernel would, whatever was rewritten. */
static void event_post(void *dc, int sysnum) {
    tls_t *t = (tls_t *)drmgr_get_tls_field(dc, tls_idx);
    dr_mcontext_t mc = {sizeof(mc), DR_MC_INTEGER};
    (void)sysnum;
    if (t->dupfd >= 0) V3C_SC(V3C_SYS_close, t->dupfd, 0, 0);
    t->dupfd = -1;
    if (!t->rewritten) return;
    t->rewritten = false;
    dr_get_mcontext(dc, &mc);
    mc.xdi = t->save[0]; mc.xsi = t->save[1]; mc.xdx = t->save[2];
    mc.r10 = t->save[3]; mc.r8 = t->save[4]; mc.r9 = t->save[5];
    dr_set_mcontext(dc, &mc);
}

static void event_thread_init(void *dc) {
    tls_t *t = (tls_t *)dr_thread_alloc(dc, sizeof *t);
    t->rewritten = false;
    t->dupfd = -1;
    drmgr_set_tls_field(dc, tls_idx, t);
}
static void event_thread_exit(void *dc) { dr_thread_free(dc, drmgr_get_tls_field(dc, tls_idx), sizeof(tls_t)); }

/* The value of DYNAMORIO_EXE_PATH in this process's initial environment, which the libc-less client reads
 * directly. An exec'd child's is what its parent passed to execve. */
static bool exe_path_from_environ(char *out, size_t max) {
    static const char key[] = "DYNAMORIO_EXE_PATH=";
    char buf[512];
    long fd = V3C_SC(V3C_SYS_openat, V3C_AT_FDCWD, "/proc/self/environ", 0);
    size_t pos = 0, olen = 0; /* offset in the current entry, bytes of value kept */
    bool match = true, found = false;
    if (fd < 0) return false;
    while (!found) {
        long n = V3C_SC(V3C_SYS_read, fd, buf, sizeof buf);
        if (n <= 0) break;
        for (long i = 0; i < n && !found; i++) {
            char c = buf[i];
            if (c == 0) {
                found = match && pos >= sizeof key - 1;
                if (!found) { pos = 0; olen = 0; match = true; }
                continue;
            }
            if (pos < sizeof key - 1) {
                if (c != key[pos]) match = false;
            } else if (match) {
                if (olen + 1 >= max) match = false;
                else { out[olen++] = c; out[olen] = 0; }
            }
            pos++;
        }
    }
    V3C_SC(V3C_SYS_close, fd, 0, 0);
    /* The last entry may lack its terminator. */
    if (!found && match && pos >= sizeof key - 1) found = true;
    return found && olen > 0;
}

void exec_policy_init(void) {
    char path[PATHBUF];
    tls_idx = drmgr_register_tls_field();
    if (tls_idx < 0 || !drmgr_register_thread_init_event(event_thread_init) ||
        !drmgr_register_thread_exit_event(event_thread_exit))
        return;

    if (exe_path_from_environ(path, sizeof path)) {
        held = dr_open_file(path, DR_FILE_READ);
        /* execveat's copy of the fd: hand it back so the app sees no extra fd. */
        if (starts(path, DUP_PREFIX)) {
            long n = 0;
            for (const char *d = path + sizeof DUP_PREFIX - 1; *d >= '0' && *d <= '9'; d++) n = n * 10 + (*d - '0');
            V3C_SC(V3C_SYS_close, n, 0, 0);
        }
    }
    if (held == INVALID_FILE) { /* the environment named nothing usable */
        module_data_t *m = dr_get_main_module();
        if (m != NULL) {
            held = dr_open_file(m->full_path, DR_FILE_READ);
            dr_free_module_data(m);
        }
    }
    dr_register_filter_syscall_event(event_filter);
    drmgr_register_pre_syscall_event(event_pre);
    drmgr_register_post_syscall_event(event_post);
}

/* What happens at exec, and at the places that name the executable. DynamoRIO handles execve only,
 * and virtualizes only readlink of /proc/self/exe; the rest, and that, is rewritten here, on the way into the
 * kernel. Refusals and the shared judgement: exec_check.h. */
#include "dr_api.h"
#include "drmgr.h"
#include "exec_check.h"
#include "execpolicy.h"
#include "str.h"

enum { PATHBUF = 4096 };

typedef struct {
    reg_t save[6]; /* rdi rsi rdx r10 r8 r9 as the app set them */
    bool rewritten;
    int pinned;    /* the file DynamoRIO will load, closed if the exec fails */
    char **argv;   /* a script's kernel-built argv, argv_n slots, freed if the exec fails */
    size_t argv_n;
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
static char *put_int(char *p, long v) {
    char t[24];
    int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
#define fd_path v3c_fd_path

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

/* The argv the kernel gives a script: for each shebang level the interpreter, its argument and the name it was
 * reached by, then the app's arguments after argv[0]. Null if the app's argv faults. */
static char **script_argv(void *dc, tls_t *t, char **app_argv) {
    size_t n = 0, cap;
    char **v, *one;
    size_t got;
    while (app_argv != NULL) {
        if (!dr_safe_read(app_argv + n, sizeof one, &one, &got) || got != sizeof one) return NULL;
        if (one == NULL) break;
        n++;
    }
    cap = n + 3 * V3C_MAXSHEBANG + 2;
    t->argv = v = (char **)dr_thread_alloc(dc, cap * sizeof *v);
    t->argv_n = cap;
    v += 3 * V3C_MAXSHEBANG;
    for (size_t i = 0; i < n; i++) dr_safe_read(app_argv + i, sizeof one, &v[i], &got);
    if (n == 0) n = 1; /* the kernel's empty argv becomes one slot, replaced below */
    v[n] = NULL;
    for (int k = 0; k < t->x.n; k++) {
        v[0] = k == 0 ? t->path : (char *)t->x.interp[k - 1];
        if (t->x.arg[k]) *--v = (char *)t->x.arg[k];
        *--v = (char *)t->x.interp[k];
    }
    return v;
}

/* Every exec opens the file once, judges that descriptor and has DynamoRIO load from it (ADR 0009). t->path is only
 * the exec's name. True: go on with it, as execve. False: it is decided; no post event comes, so nothing is rewritten
 * before this point. */
static bool run_judged(void *dc, tls_t *t, int dirfd, const char *path, int oflags, reg_t argv, reg_t envp,
                       bool cloexec_dir) {
    long fd;
    v3c_judge(dirfd, path, oflags, &t->x);
    if (t->x.verdict == V3C_REFUSE)
        dr_fprintf(STDERR, "v3compat: refusing to exec %s: %s\n", t->x.final, t->x.why);
    if (t->x.verdict != V3C_OK) return fail_with(dc, t->x.err);
    /* A script run through a close-on-exec fd cannot be opened by its interpreter: natively ENOENT. */
    if (t->x.n > 0 && cloexec_dir) { V3C_SC(V3C_SYS_close, t->x.fd, 0, 0); return fail_with(dc, V3C_ENOENT); }
    fd = v3c_pin(t->x.fd);
    if (fd < 0) return fail_with(dc, (int)-fd);
    if (t->x.n > 0) {
        argv = (reg_t)script_argv(dc, t, (char **)argv);
        if (argv == 0) { V3C_SC(V3C_SYS_close, fd, 0, 0); return fail_with(dc, 14 /* EFAULT */); }
    }
    t->pinned = (int)fd;
    save_regs(dc, t);
    dr_syscall_set_sysnum(dc, V3C_SYS_execve);
    dr_syscall_set_param(dc, 0, (reg_t)t->path);
    /* DR reads argv and does not check it. */
    dr_syscall_set_param(dc, 1, argv ? argv : (reg_t)empty_vec);
    dr_syscall_set_param(dc, 2, envp);
    dr_syscall_set_exec_file(dc, (file_t)fd);
    return true;
}

static bool pre_execve(void *dc, tls_t *t) {
    const char *app_path = (const char *)dr_syscall_get_param(dc, 0);
    reg_t argv = dr_syscall_get_param(dc, 1), envp = dr_syscall_get_param(dc, 2);
    char self[PATHBUF];
    long n = read_str(app_path, t->path, PATHBUF);
    if (n < 0) return fail_with(dc, n == -1 ? 14 /* EFAULT */ : 36 /* ENAMETOOLONG */);
    if (n == 0) return fail_with(dc, V3C_ENOENT);
    if (is_self_exe(t->path) && exe_target(self)) return run_judged(dc, t, V3C_AT_FDCWD, self, 0, argv, envp, false);
    return run_judged(dc, t, V3C_AT_FDCWD, t->path, 0, argv, envp, false);
}

static bool pre_execveat(void *dc, tls_t *t) {
    int dirfd = (int)dr_syscall_get_param(dc, 0);
    const char *app_path = (const char *)dr_syscall_get_param(dc, 1);
    reg_t argv = dr_syscall_get_param(dc, 2), envp = dr_syscall_get_param(dc, 3);
    long flags = (long)dr_syscall_get_param(dc, 4);
    char path[PATHBUF], self[40];
    long n = read_str(app_path, path, PATHBUF);
    bool by_fd, cloexec;
    if (n < 0) return fail_with(dc, n == -1 ? 14 /* EFAULT */ : 36 /* ENAMETOOLONG */);
    if (flags & ~(long)(V3C_AT_EMPTY_PATH | V3C_AT_SYMLINK_NOFOLLOW)) return fail_with(dc, V3C_EINVAL);
    if (n == 0 && !(flags & V3C_AT_EMPTY_PATH)) return fail_with(dc, V3C_ENOENT);
    by_fd = n == 0 || (path[0] != '/' && dirfd != V3C_AT_FDCWD);
    cloexec = false;
    if (by_fd) {
        long fl = V3C_SC(V3C_SYS_fcntl, dirfd, 1 /* F_GETFD */, 0);
        if (fl < 0) return fail_with(dc, (int)-fl);
        cloexec = fl & 1;
    }
    /* The kernel's name for it, which AT_EXECFN and a script's interpreter see. */
    if (!by_fd) {
        put_str(t->path, path)[0] = 0;
    } else {
        char *q = put_int(put_str(t->path, "/dev/fd/"), dirfd);
        if ((size_t)n + (size_t)(q - t->path) + 2 > PATHBUF) return fail_with(dc, 36);
        if (n > 0) q = put_str(put_str(q, "/"), path);
        *q = 0;
    }
    if (n == 0) {
        fd_path(self, dirfd);
        return run_judged(dc, t, V3C_AT_FDCWD, self, 0, argv, envp, cloexec);
    }
    if (!by_fd && is_self_exe(path) && exe_target(self)) return run_judged(dc, t, V3C_AT_FDCWD, self, 0, argv, envp, false);
    return run_judged(dc, t, dirfd, path, (flags & V3C_AT_SYMLINK_NOFOLLOW) ? V3C_O_NOFOLLOW : 0, argv, envp, cloexec);
}

/* The calls that follow /proc/self/exe, sent to the held descriptor so the kernel answers. That includes readlink:
 * DynamoRIO's own answer fails on a short buffer and skips a path it does not recognise. The index of the path
 * argument and of the flags (-1: none). */
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

static void pre_open_like(void *dc, tls_t *t, int pi, int fi) {
    const char *app_path = (const char *)dr_syscall_get_param(dc, pi);
    char head[6];
    size_t got = 0;
    if (held == INVALID_FILE) return;
    if (fi >= 0 && ((long)dr_syscall_get_param(dc, fi) & V3C_AT_SYMLINK_NOFOLLOW)) return;
    if (!dr_safe_read(app_path, 6, head, &got) || got != 6 || !starts(head, "/proc/")) return;
    if (read_str(app_path, t->path, 64) < 0 || !is_self_exe(t->path) || !exe_target(t->path)) return;
    save_regs(dc, t);
    dr_syscall_set_param(dc, pi, (reg_t)t->path);
}

static bool event_filter(void *dc, int sysnum) {
    int pi, fi;
    (void)dc;
    return sysnum == V3C_SYS_execve || sysnum == V3C_SYS_execveat || open_like(sysnum, &pi, &fi);
}

static bool event_pre(void *dc, int sysnum) {
    tls_t *t = (tls_t *)drmgr_get_tls_field(dc, tls_idx);
    int pi, fi;
    t->rewritten = false;
    t->pinned = -1;
    t->argv = NULL;
    if (sysnum == V3C_SYS_execve) return pre_execve(dc, t);
    if (sysnum == V3C_SYS_execveat) return pre_execveat(dc, t);
    if (open_like(sysnum, &pi, &fi)) pre_open_like(dc, t, pi, fi);
    return true;
}

/* A syscall that comes back leaves the registers as the kernel would, whatever was rewritten. */
static void event_post(void *dc, int sysnum) {
    tls_t *t = (tls_t *)drmgr_get_tls_field(dc, tls_idx);
    dr_mcontext_t mc = {sizeof(mc), DR_MC_INTEGER};
    (void)sysnum;
    if (t->pinned >= 0) V3C_SC(V3C_SYS_close, t->pinned, 0, 0);
    t->pinned = -1;
    if (t->argv != NULL) dr_thread_free(dc, t->argv, t->argv_n * sizeof *t->argv);
    t->argv = NULL;
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
    t->pinned = -1;
    t->argv = NULL;
    drmgr_set_tls_field(dc, tls_idx, t);
}
static void event_thread_exit(void *dc) { dr_thread_free(dc, drmgr_get_tls_field(dc, tls_idx), sizeof(tls_t)); }

/* A variable (key with its '=') of this process's initial environment, which the libc-less client reads
 * directly. */
static bool initial_env(const char *key, size_t klen, char *out, size_t max) {
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
                found = match && pos >= klen;
                if (!found) { pos = 0; olen = 0; match = true; }
                continue;
            }
            if (pos < klen) {
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
    if (!found && match && pos >= klen) found = true;
    return found && olen > 0;
}

void exec_policy_init(void) {
    char path[PATHBUF];
    tls_idx = drmgr_register_tls_field();
    if (tls_idx < 0 || !drmgr_register_thread_init_event(event_thread_init) ||
        !drmgr_register_thread_exit_event(event_thread_exit))
        return;

    /* The descriptor DynamoRIO loaded from: hold it privately and hand the number back, so the app sees no extra
     * fd and the file reads even if it has been deleted or is a memfd. */
    if (initial_env("DYNAMORIO_EXE_FD=", 17, path, sizeof path) && path[0] >= '0' && path[0] <= '9') {
        long n = 0;
        char fdp[40];
        for (const char *d = path; *d >= '0' && *d <= '9'; d++) n = n * 10 + (*d - '0');
        fd_path(fdp, n);
        held = dr_open_file(fdp, DR_FILE_READ);
        V3C_SC(V3C_SYS_close, n, 0, 0);
    }
    if (held == INVALID_FILE) { /* the environment named nothing usable */
        module_data_t *m = dr_get_main_module();
        if (m != NULL) {
            held = dr_open_file(m->full_path, DR_FILE_READ);
            dr_free_module_data(m);
        }
    }
    drmgr_register_filter_syscall_event(event_filter);
    drmgr_register_pre_syscall_event(event_pre);
    drmgr_register_post_syscall_event(event_post);
}

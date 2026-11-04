/* What DynamoRIO cannot do for a program about to be exec'd, decided the way the kernel would.
 * Freestanding (raw syscalls, no libc), so the static launcher and the libc-less client share it. */
#ifndef V3COMPAT_EXEC_CHECK_H
#define V3COMPAT_EXEC_CHECK_H

/* The launcher's status for a refusal; under the client the exec itself fails, with x->err (ADR 0009). */
#ifndef V3COMPAT_REFUSED_EXEC
#define V3COMPAT_REFUSED_EXEC 87 /* also in expand.h */
#endif

enum { V3C_MAXSHEBANG = 4, V3C_HDR = 256 };
/* PASS: not our business, the real exec reports it (err says how, for the launcher).
 * FAIL: the kernel would fail it but DynamoRIO would not notice; err says how.
 * REFUSE: the kernel would run it and DynamoRIO cannot; err is what the exec fails with. */
enum { V3C_OK, V3C_PASS, V3C_FAIL, V3C_REFUSE };

struct v3c_exec {
    int verdict, err;
    int level;        /* shebang level where it ended, 0 = the target itself */
    const char *why;  /* static reason for FAIL and REFUSE */
    int n;            /* shebang levels */
    const char *interp[V3C_MAXSHEBANG], *arg[V3C_MAXSHEBANG]; /* arg may be 0 */
    const char *final; /* the file that is really run */
    char hdr[V3C_MAXSHEBANG + 1][V3C_HDR + 4];
};

static inline long v3c_sc(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d, r8 __asm__("r8") = e, r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
#define V3C_SC(n, a, b, c) v3c_sc((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)

enum {
    V3C_SYS_read = 0, V3C_SYS_write = 1, V3C_SYS_open = 2, V3C_SYS_close = 3, V3C_SYS_stat = 4, V3C_SYS_fstat = 5,
    V3C_SYS_mmap = 9, V3C_SYS_pread64 = 17, V3C_SYS_access = 21, V3C_SYS_fcntl = 72,
    V3C_SYS_readlink = 89, V3C_SYS_readlinkat = 267, V3C_SYS_getuid = 102, V3C_SYS_geteuid = 107,
    V3C_SYS_getegid = 108, V3C_SYS_fstatfs = 138, V3C_SYS_prctl = 157, V3C_SYS_fgetxattr = 193,
    V3C_SYS_execve = 59, V3C_SYS_exit_group = 231, V3C_SYS_openat = 257,
    V3C_SYS_newfstatat = 262, V3C_SYS_faccessat = 269, V3C_SYS_execveat = 322, V3C_SYS_statx = 332,
    V3C_SYS_clone3 = 435, V3C_SYS_openat2 = 437, V3C_SYS_faccessat2 = 439
};
enum {
    V3C_AT_FDCWD = -100, V3C_AT_SYMLINK_NOFOLLOW = 0x100, V3C_AT_EACCESS = 0x200, V3C_AT_EMPTY_PATH = 0x1000,
    V3C_O_CLOEXEC = 02000000, V3C_X_OK = 1, V3C_R_OK = 4,
    V3C_ENOENT = 2, V3C_EACCES = 13, V3C_ENOEXEC = 8, V3C_ELOOP = 40, V3C_ENOSYS = 38, V3C_EPERM = 1,
    V3C_EBADF = 9, V3C_EINVAL = 22
};

struct v3c_stat { /* x86-64 kernel struct stat */
    unsigned long dev, ino, nlink;
    unsigned mode, uid, gid, pad0;
    unsigned long rdev;
    long size, blksize, blocks, t[6], unused[3];
};

static inline long v3c_openat(const char *p) { return V3C_SC(V3C_SYS_openat, V3C_AT_FDCWD, p, V3C_O_CLOEXEC); }
static inline long v3c_faccess(const char *p, int mode) {
    long r = v3c_sc(V3C_SYS_faccessat2, V3C_AT_FDCWD, (long)p, mode, V3C_AT_EACCESS, 0, 0);
    /* An old kernel or seccomp profile may not know faccessat2. */
    if (r == -V3C_ENOSYS || r == -V3C_EPERM) r = V3C_SC(V3C_SYS_faccessat, V3C_AT_FDCWD, p, mode);
    return r;
}

static inline int v3c_isspace(char c) { return c == ' ' || c == '\t'; }

/* Kernel binfmt_script: "#!" ws* interp [ws+ arg-to-end-of-line]. Edits hdr in place; 0 if no interpreter. */
static inline int v3c_shebang(char *hdr, long n, const char **interp, const char **arg) {
    long e = 2, s;
    while (e < n && hdr[e] != '\n') e++;
    hdr[e] = 0;
    while (e > 2 && v3c_isspace(hdr[e - 1])) hdr[--e] = 0;
    s = 2;
    while (hdr[s] && v3c_isspace(hdr[s])) s++;
    if (!hdr[s]) return 0;
    *interp = hdr + s;
    while (hdr[s] && !v3c_isspace(hdr[s])) s++;
    if (hdr[s]) hdr[s++] = 0;
    while (hdr[s] && v3c_isspace(hdr[s])) s++;
    *arg = hdr[s] ? hdr + s : 0;
    return 1;
}

static inline int v3c_nosuid_or_nnp(int fd) {
    unsigned long f[15]; /* struct statfs: f_flags is the 11th word */
    if (V3C_SC(V3C_SYS_prctl, 39 /* PR_GET_NO_NEW_PRIVS */, 0, 0) == 1) return 1;
    return V3C_SC(V3C_SYS_fstatfs, fd, f, 0) == 0 && (f[10] & 2 /* ST_NOSUID */);
}

/* Privileges an exec would change: the kernel's own rule, so root running a setuid-root file and any
 * setuid bit under no_new_privs or nosuid are not among them. */
static inline const char *v3c_priv_change(int fd, const struct v3c_stat *st) {
    long euid = V3C_SC(V3C_SYS_geteuid, 0, 0, 0), egid = V3C_SC(V3C_SYS_getegid, 0, 0, 0);
    if (v3c_nosuid_or_nnp(fd)) return 0;
    if ((st->mode & 04000) && st->uid != (unsigned)euid) return "setuid: it would change the user";
    if ((st->mode & 02000) && (st->mode & 010) && st->gid != (unsigned)egid)
        return "setgid: it would change the group";
    if (v3c_sc(V3C_SYS_fgetxattr, fd, (long)"security.capability", 0, 0, 0, 0) > 0 &&
        !(euid == 0 && V3C_SC(V3C_SYS_getuid, 0, 0, 0) == 0))
        return "it carries file capabilities";
    return 0;
}

static inline void v3c_set(struct v3c_exec *x, int verdict, int err, const char *why) {
    x->verdict = verdict; x->err = err; x->why = why;
}

/* Judges the program at path, following shebangs. Always sets x->final. */
static inline void v3c_judge(const char *path, struct v3c_exec *x) {
    const char *cur = path;
    x->n = 0; x->level = 0; x->final = path;
    v3c_set(x, V3C_OK, 0, 0);
    for (int lvl = 0;; lvl++) {
        struct v3c_stat st;
        unsigned char *h = (unsigned char *)x->hdr[lvl];
        long fd, n;
        x->level = lvl; x->final = cur;
        fd = v3c_openat(cur);
        if (fd < 0) {
            /* DR would fail to open it too, and then let the kernel run it untranslated. */
            if (v3c_faccess(cur, V3C_X_OK) == 0)
                /* EPERM, not EACCES: execvp would take EACCES as a reason to run the next file in PATH. */
                v3c_set(x, V3C_REFUSE, V3C_EPERM, "it is executable but not readable, so DynamoRIO cannot load it");
            else
                v3c_set(x, V3C_PASS, (int)-fd, 0);
            return;
        }
        if (V3C_SC(V3C_SYS_fstat, fd, &st, 0) != 0 || (st.mode & 0170000) != 0100000) {
            v3c_set(x, V3C_PASS, V3C_EACCES, 0);
            goto done;
        }
        if (v3c_faccess(cur, V3C_X_OK) != 0) { v3c_set(x, V3C_PASS, V3C_EACCES, 0); goto done; }
        n = v3c_sc(V3C_SYS_pread64, fd, (long)h, V3C_HDR, 0, 0, 0);
        if (n < 0) n = 0;
        h[n] = 0;
        if (n >= 2 && h[0] == '#' && h[1] == '!') {
            /* The kernel ignores setuid on a script and honours it on the interpreter. */
            if (x->n == V3C_MAXSHEBANG) { v3c_set(x, V3C_FAIL, V3C_ELOOP, "shebang chain too deep"); goto done; }
            if (!v3c_shebang((char *)h, n, &x->interp[x->n], &x->arg[x->n])) {
                v3c_set(x, V3C_PASS, V3C_ENOEXEC, 0);
                goto done;
            }
            cur = x->interp[x->n++];
            V3C_SC(V3C_SYS_close, fd, 0, 0);
            continue;
        }
        if (n >= 20 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
            unsigned long phoff = 0;
            unsigned phnum, phsz;
            int machine = h[18] | (h[19] << 8), type = h[16] | (h[17] << 8);
            if (h[4] == 1) { v3c_set(x, V3C_REFUSE, V3C_ENOEXEC, "it is a 32-bit program"); goto done; }
            if (n < 64 || h[4] != 2 || h[5] != 1 || machine != 62 || (type != 2 && type != 3)) {
                v3c_set(x, V3C_FAIL, V3C_ENOEXEC, "not an x86-64 executable");
                goto done;
            }
            for (int i = 0; i < 8; i++) phoff |= (unsigned long)h[32 + i] << (8 * i);
            phsz = h[54] | (h[55] << 8); phnum = h[56] | (h[57] << 8);
            /* What DR's loader insists on and would otherwise abort over. */
            if (phsz != 56 || phnum == 0 || phnum > 1024) { v3c_set(x, V3C_FAIL, V3C_ENOEXEC, "malformed ELF headers"); goto done; }
            {
                for (unsigned i = 0; i < phnum; i++) {
                    unsigned char ph[56];
                    char ip[V3C_HDR];
                    unsigned long off = 0, sz = 0;
                    if (v3c_sc(V3C_SYS_pread64, fd, (long)ph, 56, (long)(phoff + 56ul * i), 0, 0) != 56) {
                        v3c_set(x, V3C_FAIL, V3C_ENOEXEC, "truncated ELF");
                        goto done;
                    }
                    if (ph[0] != 3 /* PT_INTERP */) continue;
                    for (int k = 0; k < 8; k++) {
                        off |= (unsigned long)ph[8 + k] << (8 * k);
                        sz |= (unsigned long)ph[32 + k] << (8 * k);
                    }
                    if (sz == 0 || sz > sizeof ip - 1) { v3c_set(x, V3C_FAIL, V3C_ENOEXEC, "bad ELF interpreter"); goto done; }
                    if (v3c_sc(V3C_SYS_pread64, fd, (long)ip, (long)sz, (long)off, 0, 0) != (long)sz) {
                        v3c_set(x, V3C_FAIL, V3C_ENOEXEC, "truncated ELF");
                        goto done;
                    }
                    ip[sz] = 0;
                    {
                        long ifd = v3c_openat(ip);
                        if (ifd < 0) {
                            v3c_set(x, V3C_FAIL, V3C_ENOENT, "its ELF interpreter cannot be opened");
                            goto done;
                        }
                        V3C_SC(V3C_SYS_close, ifd, 0, 0);
                    }
                    break;
                }
            }
            x->why = v3c_priv_change((int)fd, &st);
            if (x->why) v3c_set(x, V3C_REFUSE, V3C_EPERM, x->why);
            goto done;
        }
        v3c_set(x, V3C_PASS, V3C_ENOEXEC, 0);
    done:
        V3C_SC(V3C_SYS_close, fd, 0, 0);
        return;
    }
}

#endif

/* v3compat-run [--] prog [args...]: runs prog under DynamoRIO with the v3compat client.
 * Static and freestanding: a container image may have no shell, no libc and no writable directory.
 * It execs libdynamorio.so with the target in DYNAMORIO_EXE_PATH, so the exit status and signals are the
 * program's own. Exit 125 launcher failure, 127 not found, 126 not executable, 87 refused (exec_check.h).
 *   V3COMPAT_NATIVE=1  skip DynamoRIO and every check
 *   V3COMPAT_DR_OPTS   extra DynamoRIO options
 *   V3COMPAT_STATS=1   client prints executed per-class and per-opcode totals at exit
 *   V3COMPAT_HELPERS_ONLY=1   no inline expansions, to compare them with the helpers
 *   V3COMPAT_AUDIT=1   print forms off the audited list at translation; run the native ones anyway
 *   V3COMPAT_CACHE=DIR translation cache, for tests and labs only (docs/translation-cache.md); with
 *                      V3COMPAT_CACHE_WRITE=1 a training run, V3COMPAT_CACHE_VERIFY=1|2, V3COMPAT_CACHE_STATS=1|2 */
#include "../src/exec_check.h"

#define MAX_CONFIG_VALUE 2048 /* DynamoRIO's limit for the option string */
#define PATH_MAX_ 4096

__asm__(".globl _start\n_start:\n xor %ebp,%ebp\n mov %rsp,%rdi\n and $-16,%rsp\n call run\n");

void *memcpy(void *d, const void *s, unsigned long n) {
    char *dd = d; const char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memmove(void *d, const void *s, unsigned long n) {
    char *dd = d; const char *ss = s;
    if (dd < ss) while (n--) *dd++ = *ss++;
    else while (n--) dd[n] = ss[n];
    return d;
}
void *memset(void *d, int c, unsigned long n) {
    char *dd = d;
    while (n--) *dd++ = (char)c;
    return d;
}

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static int has_prefix(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}
static int has_char(const char *s, const char *set) {
    for (; *s; s++) for (const char *c = set; *c; c++) if (*s == *c) return 1;
    return 0;
}
static char *scpy(char *d, const char *s) { while (*s) *d++ = *s++; return d; }

static const char *envget(char **envp, const char *name) { /* name is "NAME=" */
    for (; *envp; envp++) if (has_prefix(*envp, name)) return *envp + slen(name);
    return 0;
}

/* One line on stderr: the parts, then a newline. Never stdout, which belongs to the program. */
static void say(const char *first, ...) {
    char buf[PATH_MAX_ + 512], *p = buf;
    __builtin_va_list ap;
    __builtin_va_start(ap, first);
    p = scpy(p, "v3compat-run: ");
    for (const char *s = first; s; s = __builtin_va_arg(ap, const char *)) {
        unsigned long l = slen(s), room = (unsigned long)(buf + sizeof buf - 2 - p);
        if (l > room) l = room;
        memcpy(p, s, l); p += l;
    }
    __builtin_va_end(ap);
    *p++ = 10;
    V3C_SC(V3C_SYS_write, 2, buf, p - buf);
}
__attribute__((noreturn)) static void quit(int code) {
    V3C_SC(V3C_SYS_exit_group, code, 0, 0);
    for (;;) {}
}
__attribute__((noreturn)) static void die(int code, const char *a, const char *b, const char *c) {
    say(a, b, c, (const char *)0);
    quit(code);
}
static const char *errname(int e) {
    switch (e) {
    case 1: return "operation not permitted"; case 2: return "no such file or directory";
    case 8: return "exec format error"; case 12: return "out of memory"; case 13: return "permission denied";
    case 20: return "not a directory"; case 26: return "text file busy"; case 36: return "file name too long";
    case 40: return "too many levels of symbolic links"; default: return "error";
    }
}

static void *grab(unsigned long n) {
    long r = v3c_sc(V3C_SYS_mmap, 0, (long)((n + 4095) & ~4095ul), 3 /* RW */, 0x22 /* PRIVATE|ANON */, -1, 0);
    if (r < 0 && r > -4096) die(125, "out of memory", "", "");
    return (void *)r;
}

/* execvp's search: the first candidate that is an executable regular file; *eacces if one existed but was not. */
static const char *lookup(const char *prog, char **envp, int *eacces) {
    const char *path = envget(envp, "PATH=");
    char *buf;
    *eacces = 0;
    if (slen(prog) >= PATH_MAX_) return prog;
    for (const char *p = prog; *p; p++) if (*p == '/') return prog;
    if (!path) path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    buf = grab(2 * PATH_MAX_);
    for (;;) {
        unsigned long l = 0;
        char *q = buf;
        struct v3c_stat st;
        while (path[l] && path[l] != ':') l++;
        if (l + slen(prog) + 2 < PATH_MAX_) {
            if (l == 0) q = scpy(q, "."); else { memcpy(q, path, l); q += l; }
            *q++ = '/';
            q = scpy(q, prog); *q = 0;
            if (V3C_SC(V3C_SYS_stat, buf, &st, 0) == 0 && (st.mode & 0170000) == 0100000) {
                if (v3c_faccess(buf, V3C_X_OK) == 0) return buf;
                *eacces = 1;
            }
        }
        if (!path[l]) return 0;
        path += l + 1;
    }
}

static void native(char **argv, char **envp) {
    int eacces = 0;
    const char *t = lookup(argv[0], envp, &eacces);
    if (t) {
        long r = V3C_SC(V3C_SYS_execve, t, argv, envp);
        say(argv[0], ": ", errname((int)-r), (const char *)0);
        quit(r == -V3C_ENOENT ? 127 : 126);
    }
    die(eacces ? 126 : 127, argv[0], eacces ? ": permission denied" : ": command not found", "");
}

#define LIB "/../libexec/v3compat/libv3compat.so"
#define DRLIB "/../libexec/v3compat/dynamorio/lib64/release/libdynamorio.so"

__attribute__((used)) void run(long *sp) {
    long argc = sp[0];
    char **argv = (char **)(sp + 1), **envp = argv + argc + 1, **av;
    char *self, *client, *dr, *opts, *optsval, *envstr, *fdstr;
    const char *prog;
    long n;
    int eacces, ne = 0, i, k;
    struct v3c_exec *x;
    char **nenv, **nargv;

    argv++; argc--;
    if (argc > 0 && argv[0][0] == '-' && argv[0][1] == '-' && !argv[0][2]) { argv++; argc--; }
    if (argc <= 0) die(125, "usage: v3compat-run [--] prog [args...]", "", "");
    {
        const char *nat = envget(envp, "V3COMPAT_NATIVE=");
        if (nat && nat[0] == '1' && !nat[1]) native(argv, envp);
    }

    self = grab(4 * PATH_MAX_);
    client = self + PATH_MAX_; dr = client + PATH_MAX_ + 128;
    n = V3C_SC(V3C_SYS_readlink, "/proc/self/exe", self, PATH_MAX_ - 1);
    if (n <= 0) die(125, "cannot read /proc/self/exe (is /proc mounted?)", "", "");
    while (n > 0 && self[n - 1] != '/') n--;
    if (n <= 0) die(125, "cannot find its own directory", "", "");
    self[n - 1] = 0; /* the directory */
    if ((unsigned long)n + sizeof DRLIB + 8 >= PATH_MAX_) die(125, "install path too long", "", "");
    scpy(scpy(client, self), LIB); scpy(scpy(dr, self), DRLIB);
    if (has_char(self, " ;\"'\t\n")) die(125, "the install path may not contain a space, ';' or a quote: ", self, "");
    if (v3c_faccess(client, V3C_R_OK) != 0) die(125, "cannot read ", client, "");
    if (v3c_faccess(dr, V3C_R_OK) != 0) die(125, "cannot read ", dr, "");

    prog = lookup(argv[0], envp, &eacces);
    if (!prog) die(eacces ? 126 : 127, argv[0], eacces ? ": permission denied" : ": command not found", "");
    x = grab(sizeof *x);
    v3c_judge(V3C_AT_FDCWD, prog, 0, x);
    switch (x->verdict) {
    case V3C_REFUSE:
        say("refusing to exec ", x->final, ": ", x->why, (const char *)0);
        quit(V3COMPAT_REFUSED_EXEC);
    case V3C_FAIL:
        say(x->final, ": ", x->why ? x->why : errname(x->err), (const char *)0);
        quit((x->err == V3C_ENOENT && x->level == 0 && !x->why) ? 127 : 126);
    }
    /* What DynamoRIO loads; it survives the exec, and the client closes it. */
    x->fd = v3c_pin(x->fd);
    if (x->fd < 0) die(125, "cannot keep ", x->final, " open");

    /* DYNAMORIO_OPTIONS: -code_api, no cache reset (it corrupts multithreaded programs: ADR 0010), the user's options,
     * then the client with its options. */
    opts = grab(8192);
    {
        const char *user = envget(envp, "V3COMPAT_DR_OPTS="), *s;
        char *p = opts;
        if (user && slen(user) >= MAX_CONFIG_VALUE) die(125, "V3COMPAT_DR_OPTS is too long for DynamoRIO", "", ""); /* before it is copied */
        p = scpy(p, "DYNAMORIO_OPTIONS=-code_api -no_enable_reset -no_ignore_syscalls ");
        if (user && *user) { p = scpy(p, user); *p++ = ' '; }
        /* Not a user option yet (Phase 3): a directory, written by a training run. */
        s = envget(envp, "V3COMPAT_CACHE=");
        if (s && *s) {
            const char *w = envget(envp, "V3COMPAT_CACHE_WRITE="), *v = envget(envp, "V3COMPAT_CACHE_VERIFY="),
                       *st = envget(envp, "V3COMPAT_CACHE_STATS=");
            if (slen(s) >= 1024) die(125, "V3COMPAT_CACHE is too long", "", "");
            p = scpy(p, "-pcache_dir \""); p = scpy(p, s); p = scpy(p, "\" -pcache_mode ");
            p = scpy(p, w && w[0] == '1' && !w[1] ? "2 " : "1 ");
            /* the same rip-relative choices in every run, whatever vmcode's random offset */
            p = scpy(p, "-stable_reach ");
            if (v && (v[0] == '1' || v[0] == '2') && !v[1]) { p = scpy(p, "-pcache_verify "); *p++ = v[0]; *p++ = ' '; }
            if (st && (st[0] == '1' || st[0] == '2') && !st[1]) { p = scpy(p, "-pcache_stats "); *p++ = st[0]; *p++ = ' '; }
        }
        p = scpy(p, "-client_lib \""); p = scpy(p, client); p = scpy(p, ";0;");
        s = envget(envp, "V3COMPAT_STATS=");  if (s && s[0] == '1' && !s[1]) p = scpy(p, "-stats ");
        s = envget(envp, "V3COMPAT_HELPERS_ONLY="); if (s && s[0] == '1' && !s[1]) p = scpy(p, "-helpers-only ");
        s = envget(envp, "V3COMPAT_AUDIT=");  if (s && s[0] == '1' && !s[1]) p = scpy(p, "-audit ");
        p = scpy(p, "\"");
        *p = 0;
        optsval = opts + slen("DYNAMORIO_OPTIONS=");
        if (slen(optsval) >= MAX_CONFIG_VALUE) die(125, "V3COMPAT_DR_OPTS is too long for DynamoRIO", "", "");
    }
    /* The name is what was asked for, as for the kernel's AT_EXECFN, also if that is a script. */
    envstr = grab(slen(prog) + 64);
    scpy(scpy(envstr, "DYNAMORIO_EXE_PATH="), prog);
    fdstr = grab(64);
    {
        char *q = scpy(fdstr, "DYNAMORIO_EXE_FD=");
        long v = x->fd, div = 1;
        while (v / div >= 10) div *= 10;
        for (; div; div /= 10) *q++ = (char)('0' + v / div % 10);
        *q = 0;
    }

    for (nenv = envp; *nenv; nenv++) ne++;
    nenv = grab(sizeof(char *) * (unsigned long)(ne + 5));
    for (k = 0, i = 0; i < ne; i++) {
        const char *v = envp[i];
        if (has_prefix(v, "DYNAMORIO_EXE_PATH=") || has_prefix(v, "DYNAMORIO_EXE_FD=") ||
            has_prefix(v, "DYNAMORIO_RUNUNDER=") || has_prefix(v, "DYNAMORIO_OPTIONS="))
            continue;
        nenv[k++] = envp[i];
    }
    nenv[k++] = envstr; nenv[k++] = fdstr; nenv[k++] = "DYNAMORIO_RUNUNDER=3"; nenv[k++] = opts; nenv[k] = 0;

    /* The kernel's argv for a script: interpreter, its argument, the script, then the old arguments. */
    nargv = grab(sizeof(char *) * (unsigned long)(argc + 3 * V3C_MAXSHEBANG + 2));
    av = nargv + 3 * V3C_MAXSHEBANG;
    for (i = 0; i < argc; i++) av[i] = argv[i];
    av[argc] = 0;
    for (k = 0; k < x->n; k++) {
        const char *path = k == 0 ? prog : x->interp[k - 1];
        av[0] = (char *)path; /* replaces the old argv[0] */
        if (x->arg[k]) { av -= 1; av[0] = (char *)x->arg[k]; }
        av -= 1; av[0] = (char *)x->interp[k];
    }
    {
        long r = V3C_SC(V3C_SYS_execve, dr, av, nenv);
        die(125, "cannot execute DynamoRIO (", errname((int)-r), ")");
    }
}

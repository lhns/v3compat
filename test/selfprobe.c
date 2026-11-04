/* selfprobe PATH [report]: how this program sees its own executable. One line per probe, "ok NAME" or
 * "DIFF NAME detail"; "PASS" at the end if none differed. PATH is this binary.
 *   report   print what the client does not try to change (maps order, auxv, environ) instead */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

extern const ElfW(Ehdr) __ehdr_start;
static const char *real;
static int bad;

static void res(int ok, const char *name, const char *fmt, ...) {
    if (ok) { printf("ok %s\n", name); return; }
    char d[300];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(d, sizeof d, fmt, ap);
    __builtin_va_end(ap);
    printf("DIFF %s %s\n", name, d);
    bad = 1;
}

static size_t slurp(const char *path, char *buf, size_t max) {
    int fd = open(path, O_RDONLY);
    size_t len = 0;
    ssize_t n;
    if (fd < 0) return (size_t)-1;
    while (len < max && (n = read(fd, buf + len, max - len)) > 0) len += (size_t)n;
    close(fd);
    return len;
}

static char *file_a, *file_b;
static void check_path(const char *label, const char *p) {
    char name[160], tgt[4096];
    struct stat a, b, c;
    ssize_t n;
    size_t la, lb;
    snprintf(name, sizeof name, "readlink %s", label);
    n = readlink(p, tgt, sizeof tgt - 1);
    if (n >= 0) tgt[n] = 0;
    res(n >= 0 && !strcmp(tgt, real), name, "got '%s'", n >= 0 ? tgt : "error");
    snprintf(name, sizeof name, "open %s", label);
    la = slurp(p, file_a, 1 << 24);
    lb = slurp(real, file_b, 1 << 24);
    res(la == lb && la != (size_t)-1 && !memcmp(file_a, file_b, la), name, "%zd bytes against %zd", (ssize_t)la, (ssize_t)lb);
    snprintf(name, sizeof name, "open+fstat %s", label);
    int fd = open(p, O_RDONLY);
    int ok = fd >= 0 && fstat(fd, &a) == 0 && stat(real, &b) == 0 && a.st_ino == b.st_ino && a.st_dev == b.st_dev;
    res(ok, name, "ino/dev differ");
    if (fd >= 0) close(fd);
    snprintf(name, sizeof name, "stat %s", label);
    res(stat(p, &c) == 0 && c.st_ino == b.st_ino && c.st_dev == b.st_dev, name, "ino/dev differ");
    snprintf(name, sizeof name, "access %s", label);
    res(access(p, R_OK | X_OK) == 0, name, "not accessible");
}

/* What the kernel does with the buffer: it truncates, adds no NUL and touches nothing past what it returns. */
static void check_readlink_buffer(const char *label, const char *p) {
    enum { GUARD = 0x55 };
    char name[160], b[4096 + 2];
    const size_t len = strlen(real);
    static const size_t sizes[] = {1, 4, 0 /* the path's length */, (size_t)-1 /* one longer */};
    for (int at = 0; at < 2; at++) {
        for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
            size_t sz = sizes[i] == 0 ? len : sizes[i] == (size_t)-1 ? len + 1 : sizes[i];
            size_t want = sz < len ? sz : len;
            memset(b, GUARD, sizeof b);
            ssize_t n = at ? readlinkat(AT_FDCWD, p, b, sz) : readlink(p, b, sz);
            snprintf(name, sizeof name, "%s size %zu %s", at ? "readlinkat" : "readlink", sz, label);
            res(n == (ssize_t)want && !memcmp(b, real, want) && (unsigned char)b[want] == GUARD, name, "returned %zd", n);
        }
        /* Raw: musl's wrappers turn size 0 into a call with a dummy buffer. */
        errno = 0;
        long n = at ? syscall(SYS_readlinkat, AT_FDCWD, p, b, 0L) : syscall(SYS_readlink, p, b, 0L);
        snprintf(name, sizeof name, "%s size 0 %s", at ? "readlinkat" : "readlink", label);
        res(n == -1 && errno == EINVAL, name, "returned %ld errno %d", n, errno);
    }
    errno = 0;
    long r = syscall(SYS_readlink, p, (char *)8, 4096L);
    snprintf(name, sizeof name, "readlink bad buffer %s", label);
    res(r == -1 && errno == EFAULT, name, "returned %ld errno %d", r, errno);
}

static void *thread_probe(void *arg) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%ld/exe", (long)syscall(SYS_gettid));
    check_path("tid from a thread", p);
    check_readlink_buffer("tid from a thread", p);
    return arg;
}

int main(int argc, char **argv) {
    char p[64], buf[1 << 16];
    int report = argc > 2 && !strcmp(argv[2], "report");
    file_a = malloc(1 << 24); file_b = malloc(1 << 24);
    if (argc < 2 || !(real = realpath(argv[1], 0))) return 2;
    check_path("self", "/proc/self/exe");
    check_readlink_buffer("self", "/proc/self/exe");
    snprintf(p, sizeof p, "/proc/%d/exe", (int)getpid());
    check_path("pid", p);
    check_readlink_buffer("pid", p);
    check_path("thread-self", "/proc/thread-self/exe");
    check_readlink_buffer("thread-self", "/proc/thread-self/exe");
    { pthread_t t; pthread_create(&t, 0, thread_probe, 0); pthread_join(t, 0); }

    {   /* Look-alikes are not the executable. */
        static const char *const other[] = {"/proc/selfish", "/proc/self/exe_", "/proc/self/executable", "/proc/0/exe"};
        char name[160];
        for (size_t i = 0; i < sizeof other / sizeof *other; i++) {
            snprintf(name, sizeof name, "not rewritten %s", other[i]);
            res(open(other[i], O_RDONLY) < 0, name, "opened");
        }
    }

    size_t n = slurp("/proc/self/cmdline", buf, sizeof buf - 1);
    res(n != (size_t)-1 && !strcmp(buf, argv[0]), "cmdline argv[0]", "'%s' against '%s'", buf, argv[0]);
    const char *ex = (const char *)getauxval(AT_EXECFN);
    res(ex != NULL && (!strcmp(ex, argv[0]) || !strcmp(ex, real)), "AT_EXECFN", "'%s'", ex ? ex : "null");
    const ElfW(Phdr) *ph = (const ElfW(Phdr) *)getauxval(AT_PHDR);
    const ElfW(Phdr) *own = (const ElfW(Phdr) *)((const char *)&__ehdr_start + __ehdr_start.e_phoff);
    res(ph == own, "AT_PHDR", "%p against the program headers at %p", (const void *)ph, (const void *)own);
    res(getauxval(AT_SECURE) == 0, "AT_SECURE", "%lu", getauxval(AT_SECURE));
    res(getauxval(AT_PHNUM) == __ehdr_start.e_phnum, "AT_PHNUM", "%lu", getauxval(AT_PHNUM));
    {
        FILE *f = fopen("/proc/self/maps", "r");
        char line[4096];
        int found = 0, first_named = 0;
        unsigned long lo, hi, main_addr = (unsigned long)&main;
        while (f && fgets(line, sizeof line, f)) {
            char perms[8], path[2048] = "";
            if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %2047s", &lo, &hi, perms, path) < 3 || !path[0]) continue;
            if (!first_named) { first_named = 1; if (report) printf("first named mapping: %s\n", path); }
            if (!strcmp(path, real) && perms[2] == 'x' && lo <= main_addr && main_addr < hi) found = 1;
        }
        if (f) fclose(f);
        res(found, "maps: executable mapping of the app at its real address", "none");
    }
    if (report) {
        n = slurp("/proc/self/environ", buf, sizeof buf - 1);
        buf[n] = 0;
        for (char *e = buf; e < buf + n; e += strlen(e) + 1)
            if (!strncmp(e, "DYNAMORIO_", 10)) printf("environ: %.60s\n", e);
        n = slurp("/proc/self/auxv", buf, sizeof buf);
        printf("auxv bytes: %zu\n", n);
        for (size_t i = 0; i + 16 <= n; i += 16) {
            unsigned long k, v;
            memcpy(&k, buf + i, 8); memcpy(&v, buf + i + 8, 8);
            if (k == AT_BASE || k == AT_ENTRY || k == AT_PHDR || k == AT_SYSINFO_EHDR) printf("auxv %lu = %#lx\n", k, v);
        }
        printf("getauxval ENTRY=%#lx BASE=%#lx; main=%p\n", getauxval(AT_ENTRY), getauxval(AT_BASE), (void *)&main);
        return 0;
    }
    puts(bad ? "FAIL" : "PASS");
    return bad;
}

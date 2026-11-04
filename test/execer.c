/* execer MODE CHILD [ARGS...]: start CHILD (default argument "translated") the way MODE says, and report
 * how it ended; the child's own output is the verdict.
 *   plain thread execveat-path execveat-dirfd fexecve-cloexec fexecve-memfd vfork spawn
 *   emptyenv nullenv nullargv-plain nullargv-script closedfds shebang reexec
 *   childcheck [FD]   (not a mode to start from) PASS on FD if libdynamorio is mapped, using fd 0 for its open
 *   failexec CHILD    execveat and fexecve of a non-executable file must fail and the program continues
 *   nnp CHILD         plain, after PR_SET_NO_NEW_PRIVS
 *   regs CHILD        a failing execve, execveat (path and fd) and an openat of /proc/self/exe leave the
 *                     argument registers as the kernel would */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

static char *dflt[] = {"translated", 0};
static char **child_args; /* argv[1..] for the child */

static char **mkargv(const char *argv0) {
    int n = 0;
    while (child_args[n]) n++;
    char **v = calloc((size_t)n + 2, sizeof *v);
    v[0] = (char *)argv0;
    for (int i = 0; i < n; i++) v[i + 1] = child_args[i];
    return v;
}

static long execveat_(int fd, const char *path, char **argv, char **envp, int flags) {
    return syscall(SYS_execveat, fd, path, argv, envp, flags);
}

static void *thread_exec(void *p) {
    const char *child = p;
    execve(child, mkargv(child), environ);
    perror("execve in thread");
    return (void *)1;
}

static int wait_status(pid_t c) {
    int st;
    if (waitpid(c, &st, 0) < 0) return 2;
    if (WIFSIGNALED(st)) { printf("child killed by signal %d\n", WTERMSIG(st)); return 128 + WTERMSIG(st); }
    printf("child exited %d%c", WEXITSTATUS(st), 10);
    return WEXITSTATUS(st);
}

/* One raw syscall; the six argument registers are read back after it. */
static long raw6(long nr, long a[6], int *same) {
    register long r10 __asm__("r10") = a[3], r8 __asm__("r8") = a[4], r9 __asm__("r9") = a[5];
    long ret, rdi = a[0], rsi = a[1], rdx = a[2];
    __asm__ volatile("syscall" : "=a"(ret), "+D"(rdi), "+S"(rsi), "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9)
                     : "0"(nr) : "rcx", "r11", "memory");
    *same = rdi == a[0] && rsi == a[1] && rdx == a[2] && r10 == a[3] && r8 == a[4] && r9 == a[5];
    return ret;
}

static int regs_test(const char *nonexec) {
    char *av[] = {(char *)nonexec, 0};
    int fd = open(nonexec, O_RDONLY), same, bad = 0;
    long a1[6] = {(long)nonexec, (long)av, (long)environ, 11, 12, 13};
    long a2[6] = {AT_FDCWD, (long)nonexec, (long)av, (long)environ, 0, 14};
    long a3[6] = {fd, (long)"", (long)av, (long)environ, AT_EMPTY_PATH, 15};
    long a4[6] = {AT_FDCWD, (long)"/proc/self/exe", O_RDONLY, 0, 16, 17};
    long r;
    r = raw6(SYS_execve, a1, &same);
    if (r >= 0 || !same) { printf("FAIL execve r=%ld same=%d\n", r, same); bad = 1; }
    r = raw6(SYS_execveat, a2, &same);
    if (r >= 0 || !same) { printf("FAIL execveat path r=%ld same=%d\n", r, same); bad = 1; }
    r = raw6(SYS_execveat, a3, &same);
    if (r >= 0 || !same) { printf("FAIL execveat fd r=%ld same=%d\n", r, same); bad = 1; }
    r = raw6(SYS_openat, a4, &same);
    if (r < 0 || !same) { printf("FAIL openat self-exe r=%ld same=%d\n", r, same); bad = 1; }
    if (!bad) puts("PASS");
    return bad;
}

static int maps_has_dr(int fd) {
    char buf[1 << 16];
    ssize_t n, len = 0;
    while ((n = read(fd, buf + len, sizeof buf - 1 - (size_t)len)) > 0) len += n;
    buf[len] = 0;
    return strstr(buf, "libdynamorio") != NULL;
}

static int childcheck(int argc, char **argv) {
    int out = argc > 2 ? atoi(argv[2]) : 1;
    int fd = open("/proc/self/maps", O_RDONLY);
    char msg[160];
    /* After "closedfds" the first free descriptor is 0, as it is natively. */
    if (argc > 2 && fd != 0) { snprintf(msg, sizeof msg, "FAIL first open got fd %d, not 0\n", fd); goto bad; }
    if (!maps_has_dr(fd)) { snprintf(msg, sizeof msg, "FAIL not translated\n"); goto bad; }
    if (write(out, "PASS\n", 5) != 5) return 2;
    return 0;
bad:
    if (write(out, msg, strlen(msg)) < 0) return 2;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *mode = argv[1];
    if (!strcmp(mode, "childcheck")) return childcheck(argc, argv);
    if (argc < 3) return 2;
    const char *child = argv[2];
    child_args = argc > 3 ? argv + 3 : dflt;

    if (!strcmp(mode, "plain")) {
        execve(child, mkargv(child), environ);
    } else if (!strcmp(mode, "thread")) {
        pthread_t th;
        void *r;
        pthread_create(&th, 0, thread_exec, (void *)child);
        pthread_join(th, &r);
    } else if (!strcmp(mode, "execveat-path")) {
        execveat_(AT_FDCWD, child, mkargv(child), environ, 0);
    } else if (!strcmp(mode, "execveat-dirfd")) {
        char *dir = strdup(child), *slash = strrchr(dir, '/');
        *slash = 0;
        int dfd = open(dir, O_RDONLY | O_DIRECTORY);
        execveat_(dfd, slash + 1, mkargv(slash + 1), environ, 0);
    } else if (!strcmp(mode, "fexecve-cloexec")) {
        int fd = open(child, O_RDONLY | O_CLOEXEC);
        execveat_(fd, "", mkargv(child), environ, AT_EMPTY_PATH);
    } else if (!strcmp(mode, "fexecve-memfd")) {
        int src = open(child, O_RDONLY), m = memfd_create("child", 0);
        char buf[65536];
        ssize_t n;
        while ((n = read(src, buf, sizeof buf)) > 0) if (write(m, buf, (size_t)n) != n) return 2;
        close(src);
        execveat_(m, "", mkargv(child), environ, AT_EMPTY_PATH);
    } else if (!strcmp(mode, "vfork") || !strcmp(mode, "spawn")) {
        pid_t c;
        if (mode[0] == 'v') {
            c = vfork();
            if (c == 0) { execve(child, mkargv(child), environ); _exit(127); }
        } else if (posix_spawn(&c, child, 0, 0, mkargv(child), environ)) {
            return 2;
        }
        fflush(stdout);
        return wait_status(c);
    } else if (!strcmp(mode, "emptyenv")) {
        char *env[] = {0};
        execve(child, mkargv(child), env);
    } else if (!strcmp(mode, "nullenv")) {
        execve(child, mkargv(child), NULL);
    } else if (!strcmp(mode, "nullargv-plain")) {
        char *const *volatile none = 0;
        execve(child, none, environ);
    } else if (!strcmp(mode, "nullargv-script")) { /* child is the script */
        { char *const *volatile none = 0; execve(child, none, environ); }
    } else if (!strcmp(mode, "closedfds")) { /* child is this program, in childcheck mode */
        char *v[] = {(char *)child, "childcheck", "3", 0};
        dup2(1, 3);
        close(0); close(1); close(2);
        execve(child, v, environ);
    } else if (!strcmp(mode, "shebang")) { /* child is the script */
        execve(child, mkargv(child), environ);
    } else if (!strcmp(mode, "reexec")) {
        char *v[] = {argv[0], "childcheck", 0};
        execve("/proc/self/exe", v, environ);
    } else if (!strcmp(mode, "nnp")) {
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        execve(child, mkargv(child), environ);
    } else if (!strcmp(mode, "regs")) {
        return regs_test(child);
    } else if (!strcmp(mode, "failexec")) { /* child: a file without the execute bit */
        int fd = open(child, O_RDONLY | O_CLOEXEC);
        long a = execveat_(AT_FDCWD, child, mkargv(child), environ, 0);
        int ea = errno;
        long b = execveat_(fd, "", mkargv(child), environ, AT_EMPTY_PATH);
        int eb = errno;
        if (a == -1 && ea == EACCES && b == -1 && eb == EACCES) { puts("PASS"); return 0; }
        printf("FAIL execveat errno %d, fexecve errno %d\n", ea, eb);
        return 1;
    } else {
        return 2;
    }
    printf("FAIL exec %s: %s\n", mode, strerror(errno));
    return 1;
}

/* vfork_fail [runs]: vfork-like syscalls that fail, each followed by a thread and a vfork that execs. DynamoRIO takes a
 * lock for a vfork child before the syscall, which the parent must give back when there is no child. Fails three
 * ways: clone and clone3 with bad arguments, and, when not root, vfork() at RLIMIT_NPROC 0. "PASS" as the last line,
 * natively too. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct args { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls; };

static void *nothing(void *a) { return a; }

/* After a failed vfork: a thread and a working vfork. */
static int after(const char *what, int i) {
    pthread_t t;
    int st;
    if (pthread_create(&t, 0, nothing, 0) || pthread_join(t, 0)) { printf("FAIL %s %d: thread\n", what, i); return 1; }
    pid_t c = vfork();
    if (c == 0) { execl("/bin/true", "true", (char *)0); _exit(127); }
    if (c < 0 || waitpid(c, &st, 0) != c || st != 0) { printf("FAIL %s %d: vfork after\n", what, i); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    int runs = argc > 1 ? atoi(argv[1]) : 100, nproc = geteuid() != 0;
    for (int i = 0; i < runs; i++) {
        /* CLONE_THREAD without CLONE_SIGHAND is EINVAL. */
        long r = syscall(SYS_clone, CLONE_VM | CLONE_VFORK | CLONE_THREAD | SIGCHLD, 0, 0, 0, 0);
        if (r != -1 || errno != EINVAL) { printf("FAIL clone %d: %ld %s\n", i, r, strerror(errno)); return 1; }
        if (after("clone", i)) return 1;

        /* An exit signal past CSIGNAL is EINVAL. */
        struct args ca;
        memset(&ca, 0, sizeof ca);
        ca.flags = CLONE_VM | CLONE_VFORK;
        ca.exit_signal = 0x1000;
        r = syscall(SYS_clone3, &ca, sizeof ca);
        if (r != -1 || errno != EINVAL) { printf("FAIL clone3 %d: %ld %s\n", i, r, strerror(errno)); return 1; }
        if (after("clone3", i)) return 1;

        if (nproc) {
            struct rlimit old, zero = {0, 0};
            getrlimit(RLIMIT_NPROC, &old);
            zero.rlim_max = old.rlim_max;
            setrlimit(RLIMIT_NPROC, &zero);
            pid_t c = vfork();
            if (c == 0) _exit(0);
            int e = errno;
            setrlimit(RLIMIT_NPROC, &old);
            if (c > 0) { waitpid(c, 0, 0); printf("FAIL nproc %d: vfork did not fail\n", i); return 1; }
            if (e != EAGAIN) { printf("FAIL nproc %d: %s\n", i, strerror(e)); return 1; }
            if (after("nproc", i)) return 1;
        }
    }
    printf("ok %d runs%s\nPASS\n", runs, nproc ? "" : " (root: no RLIMIT_NPROC case)");
    return 0;
}

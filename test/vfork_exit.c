/* vfork_exit [runs] [nodump|noready]: a process ends while one of its threads waits in vfork for a child that has
 * not exited. Natively the process ends at once and the child runs on. The child here waits on a pipe that is closed
 * only once the process is gone, so a process that waits for its child shows as a timeout; then the child must still
 * finish. In the race mode the child exits or execs on its own as the process ends, which can let the waiting thread
 * go first. In the last mode every vfork fails, which must not leave the translator waiting for a child at exit.
 * nodump makes the process non-dumpable; noready ends it without waiting for the child to start (lab only:
 * the child may not run at all). Same output natively and translated: "PASS" as the last line. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define F_VM 0x100ull
#define F_VFORK 0x4000ull

struct args { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls; };

enum { EXIT, SIGNAL, MAIN_WAITS, CLONE3, RACE, FAILED, NMODES };
static const char *names[] = {"another thread exits", "SIGTERM", "main thread waits, another exits",
                              "clone3 CLONE_VFORK", "the child exits or execs as the process ends",
                              "vfork fails at a process limit, then the process exits"};
static int mode, hold[2], ready[2], alive[2], nodump, noready;
static volatile long spin;
static char *const targv[] = {"true", 0};
static char *const tenv[] = {0};

static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

/* The child: raw syscalls only, as it shares the parent's memory. Says it runs, waits for the pipe to close, then
 * says it finished. Once the process is gone, only the child holds alive[1]. */
static __attribute__((noreturn)) void child(void) {
    char b;
    sc3(SYS_write, ready[1], (long)"x", 1);
    if (mode == RACE) {
        for (long i = spin; i > 0; i--) __builtin_ia32_pause();
        if (spin & 1) sc3(SYS_execve, (long)"/bin/true", (long)targv, (long)tenv);
        sc3(SYS_exit_group, 0, 0, 0);
    }
    sc3(SYS_read, hold[0], (long)&b, 1);
    sc3(SYS_write, alive[1], (long)"d", 1);
    sc3(SYS_exit_group, 0, 0, 0);
    __builtin_unreachable();
}

static void clone3_vfork(void) {
    static char stack[65536] __attribute__((aligned(16)));
    struct args ca;
    long r;
    register long r8 __asm__("r8") = (long)child;
    memset(&ca, 0, sizeof ca);
    ca.flags = F_VM | F_VFORK;
    ca.exit_signal = SIGCHLD;
    ca.stack = (uint64_t)stack;
    ca.stack_size = sizeof stack;
    __asm__ volatile("syscall\n test %%rax,%%rax\n jnz 1f\n call *%%r8\n 1:\n"
                     : "=a"(r) : "0"(SYS_clone3), "D"(&ca), "S"(sizeof ca), "r"(r8) : "rcx", "r11", "rdx", "memory");
}

static void *waiter(void *a) {
    (void)a;
    if (mode == CLONE3) clone3_vfork();
    else if (vfork() == 0) child();
    for (;;) pause();
}

/* Ends the process once the child runs, so the waiting thread is in vfork. */
static void *ender(void *a) {
    char b;
    (void)a;
    if (!noready && read(ready[0], &b, 1) != 1) _exit(3);
    if (mode == SIGNAL) kill(getpid(), SIGTERM);
    _exit(7);
}

/* Every vfork fails with EAGAIN (RLIMIT_NPROC 0, which root ignores: then it exits 9), then the process exits. */
static void failed_vforks(void) {
    struct rlimit none;
    int failed = 0;
    if (getrlimit(RLIMIT_NPROC, &none)) _exit(2);
    none.rlim_cur = 0;
    if (setrlimit(RLIMIT_NPROC, &none)) _exit(2);
    for (int i = 0; i < 10; i++) {
        pid_t c = vfork();
        if (c == 0) _exit(0);
        if (c < 0) failed++;
        else waitpid(c, 0, 0);
    }
    _exit(failed == 10 ? 7 : 9);
}

static void scenario(void) {
    pthread_t t;
    close(hold[1]);
    if (mode == FAILED) failed_vforks();
    if (nodump) prctl(PR_SET_DUMPABLE, 0);
    if (pipe(ready)) _exit(2);
    if (pthread_create(&t, 0, mode == MAIN_WAITS ? ender : waiter, 0)) _exit(2);
    if (mode == MAIN_WAITS) waiter(0);
    ender(0);
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Waits up to secs for p; returns its status or -1. */
static int wait_for(pid_t p, double secs) {
    struct timespec tick = {0, 10000000};
    double end = now() + secs;
    int st;
    while (now() < end) {
        pid_t r = waitpid(p, &st, WNOHANG);
        if (r == p) return st;
        if (r < 0 && errno != EINTR) return -1;
        nanosleep(&tick, 0);
    }
    return -1;
}

/* The child's "d", then end of file: it ran to its exit. */
static int child_finished(void) {
    struct pollfd pf = {alive[0], POLLIN, 0};
    double end = now() + 10;
    char b = 0;
    int n = 0;
    while (now() < end && poll(&pf, 1, 100) >= 0) {
        if (!(pf.revents & (POLLIN | POLLHUP))) continue;
        if (read(alive[0], &b, 1) <= 0) return n == 1 && b == 'd';
        n++;
    }
    return 0;
}

int main(int argc, char **argv) {
    int runs = argc > 1 ? atoi(argv[1]) : 1, bad = 0, unfinished = 0;
    nodump = argc > 2 && !strcmp(argv[2], "nodump");
    noready = argc > 2 && !strcmp(argv[2], "noready");
    srand(1);
    for (int i = 0; i < runs; i++)
        for (mode = 0; mode < NMODES; mode++) {
            spin = rand() % 20000;
            if (pipe(hold) || pipe(alive)) return 2;
            fflush(stdout);
            pid_t p = fork();
            if (p == 0) scenario();
            close(hold[0]);
            close(alive[1]);
            double t0 = now();
            int st = wait_for(p, 10);
            double took = now() - t0;
            close(hold[1]); /* the child finishes */
            int finished = mode == RACE || mode == FAILED || child_finished();
            if (noready && !finished) { unfinished++; finished = 1; }
            close(alive[0]);
            if (st == -1) {
                st = wait_for(p, 10);
                if (st == -1) { kill(p, SIGKILL); waitpid(p, &st, 0); }
                printf("FAIL %s: the process waited for its vfork child (status %#x once it exited)\n", names[mode], st);
                bad = 1;
                continue;
            }
            int ok = mode == SIGNAL ? WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM : WIFEXITED(st) && WEXITSTATUS(st) == 7;
            if (runs == 1 || !ok || !finished)
                printf("%s %s: status %#x in %.2f s, the child %s\n", ok && finished ? "ok" : "FAIL", names[mode], st,
                       took, finished ? "finished" : "did not finish");
            if (!ok || !finished) bad = 1;
        }
    if (noready) printf("children that never finished: %d\n", unfinished);
    printf("%s\n", bad ? "FAILED" : "PASS");
    return bad;
}

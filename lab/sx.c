/* sx N [vfork]: lab copy of sigchld_exit that classifies a missing SIGCHLD. Threads exit all the time while N children
 * exit one at a time; a SIGCHLD handler reaps each and the next child starts once the previous one is reaped. After 30 s
 * without the reap it prints @@STUCK (waitpid's return and errno, the child's /proc stat, the handler's counters), waits
 * 20 s for the lab to attach gdb, and exits 1. Prints the slowest reap. "PASS" as the last line. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t reaped, calls, inhandler, overlap, werr;
static volatile int stop;

static void on_chld(int s) {
    int r;
    (void)s;
    if (__atomic_fetch_add(&inhandler, 1, __ATOMIC_SEQ_CST)) overlap++;
    calls++;
    while ((r = waitpid(-1, 0, WNOHANG)) > 0) __atomic_fetch_add(&reaped, 1, __ATOMIC_SEQ_CST);
    if (r < 0 && errno != ECHILD) werr = errno;
    __atomic_fetch_sub(&inhandler, 1, __ATOMIC_SEQ_CST);
}

static void *nothing(void *a) { return a; }

static void *churn(void *a) {
    while (!stop) {
        pthread_t t;
        if (pthread_create(&t, 0, nothing, 0) == 0) pthread_join(t, 0);
    }
    return a;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void stat_of(pid_t p, char *buf, int n) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", p);
    int fd = open(path, O_RDONLY), k = fd < 0 ? -1 : (int)read(fd, buf, n - 1);
    if (fd >= 0) close(fd);
    buf[k < 0 ? 0 : k] = 0;
    for (char *c = buf; *c; c++) if (*c == '\n') *c = ' ';
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1000, use_vfork = argc > 2 && !strcmp(argv[2], "vfork");
    struct sigaction sa;
    pthread_t c[4];
    double worst = 0;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_chld;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, 0);
    for (int i = 0; i < 4; i++) pthread_create(&c[i], 0, churn, 0);
    for (int k = 1; k <= n; k++) {
        double t0 = now();
        pid_t p = use_vfork ? vfork() : fork();
        if (p == 0) _exit(0);
        if (p < 0) { printf("FAIL fork errno %d\n", errno); return 1; }
        struct timespec tick = {0, 1000000};
        while (reaped < k && now() - t0 < 30) nanosleep(&tick, 0);
        if (now() - t0 > worst) worst = now() - t0;
        if (reaped < k) {
            char st[512];
            int s = 0;
            errno = 0;
            pid_t z = waitpid(p, &s, WNOHANG);
            int e = errno;
            stat_of(p, st, sizeof st);
            printf("@@STUCK parent=%d child=%d k=%d reaped=%d calls=%d overlap=%d werr=%d waitpid=%d errno=%d status=%#x "
                   "stat=[%s]\n", getpid(), p, k, (int)reaped, (int)calls, (int)overlap, (int)werr, z, e, s, st);
            fflush(stdout);
            double t1 = now();
            while (now() - t1 < 20) nanosleep(&tick, 0);
            stat_of(p, st, sizeof st);
            printf("after 20 s: reaped=%d stat=[%s]\n", (int)reaped, st);
            return 1;
        }
    }
    stop = 1;
    for (int i = 0; i < 4; i++) pthread_join(c[i], 0);
    printf("slowest reap %.3f s, handler calls %d, overlaps %d\nPASS\n", worst, (int)calls, (int)overlap);
    return 0;
}

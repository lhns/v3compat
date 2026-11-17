/* sigchld_exit N [vfork]: threads exit all the time while N children exit one at a time; a SIGCHLD handler reaps
 * each, and the next child starts only once the previous one is reaped, so a lost SIGCHLD shows as a timeout.
 * "PASS" as the last line. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t reaped;
static volatile int stop;

static void on_chld(int s) {
    (void)s;
    while (waitpid(-1, 0, WNOHANG) > 0) reaped++;
}

static void *nothing(void *a) { return a; }

static void *churn(void *a) {
    while (!stop) {
        pthread_t t;
        if (pthread_create(&t, 0, nothing, 0) == 0) pthread_join(t, 0);
    }
    return a;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1000, use_vfork = argc > 2 && !strcmp(argv[2], "vfork");
    struct sigaction sa;
    pthread_t c[4];
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_chld;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, 0);
    for (int i = 0; i < 4; i++) pthread_create(&c[i], 0, churn, 0);
    for (int k = 1; k <= n; k++) {
        pid_t p = use_vfork ? vfork() : fork();
        if (p == 0) _exit(0);
        if (p < 0) { printf("FAIL fork\n"); return 1; }
        struct timespec tick = {0, 1000000};
        int ms = 0;
        while (reaped < k && ms < 5000) { nanosleep(&tick, 0); ms++; }
        if (reaped < k) {
            int st;
            pid_t z = waitpid(p, &st, WNOHANG);
            printf("FAIL child %d of %d: no SIGCHLD in 5 s (%s)\n", k, n, z == p ? "it was a zombie" : "not reapable");
            return 1;
        }
    }
    stop = 1;
    for (int i = 0; i < 4; i++) pthread_join(c[i], 0);
    printf("PASS\n");
    return 0;
}

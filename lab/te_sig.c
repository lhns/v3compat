/* te_sig cap ms pre|post KIND: lab copy of test/thread_exit.c where one app thread dies of KIND while threads start
 * and end: pre at ms/2, post right after main printed (main then _exits with 3). KIND: segv (read of address 16),
 * abrt (abort()), bus (read past the end of a truncated memfd mapping), ill (ud2), fpe (integer divide by zero),
 * ksegv (raise(SIGSEGV), a sent SIGSEGV). A 6th argument vfork keeps a vfork child alive meanwhile. Natively pre ends with that signal's status (139, 134, 135, 132, 136, 139)
 * and post (1 ms after main's go) with 3. Whose thread dumped the core tells whose fault ended the process. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

static int cap;
static volatile int live, created, failed;
static pthread_attr_t detached;
static void *worker(void *arg);

static int start(void *arg) {
    pthread_t t;
    int ok = 0;
    if (__atomic_add_fetch(&live, 1, __ATOMIC_RELAXED) <= cap) {
        ok = pthread_create(&t, &detached, worker, arg) == 0;
        __atomic_add_fetch(ok ? &created : &failed, 1, __ATOMIC_RELAXED);
    }
    if (!ok) __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
    return ok;
}

static void *worker(void *arg) {
    volatile int sink = 0;
    for (int i = 0; i < 200; i++) sink += i;
    if (arg) start(0);
    __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
    return 0;
}

static volatile int go, zero;
static const char *kind = "segv";
static volatile char *busmap;
static long ms;
static int pre;

static void *faulter(void *arg) {
    (void)arg;
    if (pre) {
        struct timespec h = {ms / 2000, (ms % 2000) * 500000L};
        nanosleep(&h, 0);
    } else {
        /* 1 ms after main's go: natively exit_group has ended us by then. */
        while (!go) sched_yield();
        struct timespec h = {0, 1000000L};
        nanosleep(&h, 0);
    }
    if (!strcmp(kind, "abrt")) abort();
    if (!strcmp(kind, "bus")) return (void *)(long)busmap[0];
    if (!strcmp(kind, "ill")) __builtin_trap();
    if (!strcmp(kind, "fpe")) {
        int q, z = zero; /* idiv itself: gcc turns 1 / x into a compare */
        __asm__ volatile("cltd; idivl %2" : "=a"(q) : "a"(7), "r"(z) : "edx");
        return (void *)(long)q;
    }
    if (!strcmp(kind, "ksegv")) raise(SIGSEGV);
    return (void *)(long)*(volatile int *)16;
}

/* Keeps a vfork child alive most of the time: each spins 20 ms in the child. */
static void *vforker(void *arg) {
    (void)arg;
    for (;;) {
        pid_t c = vfork();
        if (c == 0) {
            struct timespec t0, t;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            do clock_gettime(CLOCK_MONOTONIC, &t);
            while ((t.tv_sec - t0.tv_sec) * 1000000000L + t.tv_nsec - t0.tv_nsec < 20000000L);
            _exit(0);
        }
        if (c > 0) waitpid(c, 0, 0);
    }
}

static void *spawner(void *arg) {
    (void)arg;
    for (;;)
        if (!start((void *)1)) sched_yield();
}

int main(int argc, char **argv) {
    pthread_t t;
    cap = argc > 1 ? atoi(argv[1]) : 64;
    ms = argc > 2 ? atoi(argv[2]) : 300;
    pre = argc > 3 && !strcmp(argv[3], "pre");
    if (argc > 4) kind = argv[4];
    int with_vfork = argc > 5 && !strcmp(argv[5], "vfork");
    struct timespec d = {ms / 1000, (ms % 1000) * 1000000L};
    int fd = memfd_create("te_sig", 0);
    if (fd < 0 || ftruncate(fd, 4096)) return 2;
    busmap = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0);
    if (busmap == MAP_FAILED || ftruncate(fd, 0)) return 2;
    if (pthread_attr_init(&detached) || pthread_attr_setdetachstate(&detached, PTHREAD_CREATE_DETACHED)) return 2;
    if (with_vfork && pthread_create(&t, &detached, vforker, 0)) return 2;
    for (int i = 0; i < 4; i++)
        if (pthread_create(&t, 0, spawner, 0)) return 2;
    if (pthread_create(&t, &detached, faulter, 0)) return 2;
    nanosleep(&d, 0);
    printf("created %d, failed %d\n", created, failed);
    fflush(stdout);
    go = 1;
    _exit(3);
}

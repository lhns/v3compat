/* Lab copy of test/thread_exit.c (lab-te-exit): te_fault cap ms pre|post makes one app thread read address 16, so the
 * process dies of an app SIGSEGV while threads start: pre at ms/2, post right after main printed. The crashing thread
 * of the core tells whose fault killed it.
 * thread_exit [cap] [ms] [nproc|vfork]: threads keep creating threads, at most cap alive, and the main thread ends the
 * process after ms, while threads are starting and ending. nproc makes every creation fail (RLIMIT_NPROC 0), as at a
 * thread limit, and tries vfork there too; vfork does only the vfork. The process must end with status 3, promptly.
 * Same natively and translated. */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int cap;
static volatile int live, created, failed;
static pthread_attr_t detached;
static void *worker(void *arg);

/* Takes a slot for a new thread and starts it; arg says whether it may start one more. */
static int start(void *arg) {
    pthread_t t;
    int ok = 0;
    if (__atomic_add_fetch(&live, 1, __ATOMIC_RELAXED) <= cap) {
        /* Created detached: glibc's pthread_detach() reads the thread's descriptor after marking it detached,
         * and a thread exiting meanwhile frees it (and its stack) itself, so the read can fault, natively too. */
        ok = pthread_create(&t, &detached, worker, arg) == 0;
        __atomic_add_fetch(ok ? &created : &failed, 1, __ATOMIC_RELAXED);
    }
    if (!ok) __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
    return ok;
}

/* Each worker may start one more before it ends, so threads also start from threads that are ending. */
static void *worker(void *arg) {
    volatile int sink = 0;
    for (int i = 0; i < 200; i++) sink += i;
    if (arg) start(0);
    __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
    return 0;
}

static volatile int go;
static void *faulter(void *arg) {
    if (arg) {
        long ms = (long)arg;
        struct timespec h = {ms / 2000, (ms % 2000) * 500000L};
        nanosleep(&h, 0);
    } else
        while (!go) sched_yield();
    return (void *)(long)*(volatile int *)16;
}

static void *spawner(void *arg) {
    (void)arg;
    for (;;)
        if (!start((void *)1)) sched_yield();
}

int main(int argc, char **argv) {
    pthread_t t;
    cap = argc > 1 ? atoi(argv[1]) : 64;
    int ms = argc > 2 ? atoi(argv[2]) : 300;
    struct timespec d = {ms / 1000, (ms % 1000) * 1000000L};
    int vfork_only = argc > 3 && !strcmp(argv[3], "vfork"), nproc = argc > 3 && !strcmp(argv[3], "nproc");
    if (pthread_attr_init(&detached) || pthread_attr_setdetachstate(&detached, PTHREAD_CREATE_DETACHED)) return 2;
    for (int i = 0; i < 4 && !vfork_only; i++)
        if (pthread_create(&t, 0, spawner, 0)) return 2;
    int pre = argc > 3 && !strcmp(argv[3], "pre"), post = argc > 3 && !strcmp(argv[3], "post");
    if ((pre || post) && pthread_create(&t, &detached, faulter, pre ? (void *)(long)ms : 0)) return 2;
    if (nproc || vfork_only) {
        struct rlimit none = {0, RLIM_INFINITY};
        if (getrlimit(RLIMIT_NPROC, &none)) return 2;
        none.rlim_cur = 0;
        if (setrlimit(RLIMIT_NPROC, &none)) return 2;
        /* A vfork that fails the same way. */
        for (int i = 0; i < 10; i++) {
            pid_t c = vfork();
            if (c == 0) _exit(0);
            if (c > 0) waitpid(c, 0, 0);
        }
    }
    nanosleep(&d, 0);
    char line[256], rss[64] = "?";
    FILE *st = fopen("/proc/self/status", "r");
    while (st && fgets(line, sizeof line, st))
        if (!strncmp(line, "VmRSS:", 6)) sscanf(line + 6, "%63s", rss);
    printf("created %d, failed %d, rss %s kB\n", created, failed, rss);
    fflush(stdout);
    go = 1;
    /* RLIMIT_NPROC does not bind root: nothing failed, so nothing was tested. */
    _exit(nproc && failed == 0 ? 9 : 3);
}

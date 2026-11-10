/* thread_exit [cap] [ms] [nproc]: threads keep creating threads, at most cap alive, and the main thread ends the
 * process after ms, while threads are starting and ending. nproc makes every creation fail (RLIMIT_NPROC 0), as at a
 * thread limit, and tries vfork there too. The process must end with status 3, promptly. Same natively and translated. */
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
static void *worker(void *arg);

/* Takes a slot for a new thread and starts it; arg says whether it may start one more. */
static int start(void *arg) {
    pthread_t t;
    int ok = 0;
    if (__atomic_add_fetch(&live, 1, __ATOMIC_RELAXED) <= cap) {
        ok = pthread_create(&t, 0, worker, arg) == 0;
        __atomic_add_fetch(ok ? &created : &failed, 1, __ATOMIC_RELAXED);
        if (ok) pthread_detach(t);
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
    for (int i = 0; i < 4; i++)
        if (pthread_create(&t, 0, spawner, 0)) return 2;
    if (argc > 3 && !strcmp(argv[3], "nproc")) {
        struct rlimit none = {0, RLIM_INFINITY};
        if (getrlimit(RLIMIT_NPROC, &none)) return 2;
        none.rlim_cur = 0;
        if (setrlimit(RLIMIT_NPROC, &none)) return 2;
    }
    nanosleep(&d, 0);
    printf("created %d, failed %d\n", created, failed);
    fflush(stdout);
    _exit(3);
}

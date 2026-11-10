/* thread_exit [cap] [ms]: threads keep creating threads, at most cap alive, and the main thread ends the process
 * after ms, while threads are starting and ending. The process must end with status 3. Same natively and
 * translated. */
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int cap;
static volatile int live, created;
static void *worker(void *arg);

/* Takes a slot for a new thread and starts it; arg says whether it may start one more. */
static int start(void *arg) {
    pthread_t t;
    if (__atomic_add_fetch(&live, 1, __ATOMIC_RELAXED) <= cap && pthread_create(&t, 0, worker, arg) == 0) {
        pthread_detach(t);
        __atomic_add_fetch(&created, 1, __ATOMIC_RELAXED);
        return 1;
    }
    __atomic_sub_fetch(&live, 1, __ATOMIC_RELAXED);
    return 0;
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
    nanosleep(&d, 0);
    printf("created %d\n", created);
    fflush(stdout);
    _exit(3);
}

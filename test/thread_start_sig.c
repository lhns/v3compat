/* thread_start_sig ROUNDS: each round creates 8 threads and sends each a SIGXCPU, which the program handles, right
 * after pthread_create, then 50 more. A signal that reaches a thread DynamoRIO has not finished initializing must still
 * go to the handler. Prints "ok" and exits 0 when every thread ran and was joined. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int stop;
static volatile unsigned long got;
static void onsig(int s) { (void)s; __atomic_add_fetch(&got, 1, __ATOMIC_RELAXED); }
static void *spin(void *a) { (void)a; while (!stop) __asm__ volatile("pause"); return NULL; }

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 20;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onsig;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGXCPU, &sa, 0);
    for (int r = 0; r < rounds; r++) {
        pthread_t t[8];
        stop = 0;
        for (int i = 0; i < 8; i++) {
            if (pthread_create(&t[i], 0, spin, 0) != 0) return 2;
            pthread_kill(t[i], SIGXCPU);
        }
        for (int k = 0; k < 50; k++)
            for (int i = 0; i < 8; i++) pthread_kill(t[i], SIGXCPU);
        stop = 1;
        for (int i = 0; i < 8; i++) pthread_join(t[i], 0);
    }
    printf("ok %lu\n", got);
    return 0;
}

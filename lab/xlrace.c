/* xlrace: SIGXCPU (handler installed) to each new thread right after pthread_create, with no cache involved.
 * Lab only: does DynamoRIO report "default action in native thread" for a thread still in its init? */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
static volatile int stop;
static volatile unsigned long got;
static void onsig(int s) { (void)s; __atomic_add_fetch(&got, 1, __ATOMIC_RELAXED); }
static void *spin(void *a) { (void)a; while (!stop) __asm__ volatile("pause"); return NULL; }
int main(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = onsig; sa.sa_flags = SA_RESTART;
    sigaction(SIGXCPU, &sa, 0);
    pthread_t t[8];
    for (int i = 0; i < 8; i++) { pthread_create(&t[i], 0, spin, 0); pthread_kill(t[i], SIGXCPU); }
    for (int k = 0; k < 50; k++) for (int i = 0; i < 8; i++) pthread_kill(t[i], SIGXCPU);
    stop = 1;
    for (int i = 0; i < 8; i++) pthread_join(t[i], 0);
    printf("ok %lu\n", got);
    return 0;
}

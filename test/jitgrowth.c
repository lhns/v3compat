/* jitgrowth REGIONS: a worker generates, runs and unmaps hot code while main waits in pthread_join, as a JIT server's
 * other threads wait. Exits 1 if the second half of the run grew more than 2 KB per region; "PASS" otherwise. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int regions;
static long grew_kb;

static long rss_kb(void) {
    long v = 0;
    char l[256];
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) _exit(2);
    while (fgets(l, sizeof l, f))
        if (sscanf(l, "VmRSS: %ld", &v) == 1) break;
    fclose(f);
    return v;
}

static void *work(void *arg) {
    long mid = 0;
    (void)arg;
    for (uint32_t i = 0; i < (uint32_t)regions; i++) {
        unsigned char *p = mmap(0, 8192, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) _exit(2);
        for (int k = 0; k < 40; k++) { /* unique code, each hot enough to become a trace */
            unsigned char *q = p + k * 16;
            q[0] = 0xb8;
            memcpy(q + 1, &i, 4);
            q[5] = 0xc3;
            uint32_t (*f)(void) = (uint32_t (*)(void))q;
            for (int r = 0; r < 60; r++)
                if (f() != i) _exit(3);
        }
        munmap(p, 8192);
        if (i + 1 == (uint32_t)regions / 2) mid = rss_kb();
    }
    grew_kb = rss_kb() - mid;
    return NULL;
}

int main(int argc, char **argv) {
    pthread_t t;
    regions = argc > 1 ? atoi(argv[1]) : 1000;
    if (pthread_create(&t, 0, work, 0)) return 2;
    pthread_join(t, 0);
    double per = (double)grew_kb / (regions - regions / 2);
    printf("%.1f KB per region\n", per);
    if (per > 2.0) return 1;
    puts("PASS");
    return 0;
}

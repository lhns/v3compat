/* trace_flush SECONDS: threads keep traces hot in one code page while a writer dirties a byte of that page that is
 * never executed, so DynamoRIO flushes the page while traces through it are being built, and another thread keeps
 * generating fresh code so trace emission waits on the block-building lock. A flush that lands after a trace is
 * emitted but before its per-block area lists are gone used to free them under the builder (fork fix
 * fix-trace-blk-vmlists). Prints "ok" and exits 0 when every result was right. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

enum { COPIES = 6, STRIDE = 64, DATA = 2048 };

/* f(n): eax = sum over i = n..1 of (i odd ? 4 : 1); two blocks in the loop, so traces span several. */
static const unsigned char code[] = {
    0x31, 0xc0,                         /* xor eax, eax */
    0x89, 0xf9,                         /* mov ecx, edi */
    0xf7, 0xc1, 0x01, 0, 0, 0,          /* loop: test ecx, 1 */
    0x74, 0x03,                         /* jz skip */
    0x83, 0xc0, 0x03,                   /* add eax, 3 */
    0x83, 0xc0, 0x01,                   /* skip: add eax, 1 */
    0xff, 0xc9,                         /* dec ecx */
    0x75, 0xee,                         /* jnz loop */
    0xc3,                               /* ret */
};

static unsigned char *page;
static atomic_int stop, bad;
static atomic_ulong calls, writes, fresh;

static int expect(int n) { return n + 3 * ((n + 1) / 2); }

static void *runner(void *arg) {
    int (*f)(int) = (int (*)(int))(page + (uintptr_t)arg * STRIDE);
    while (!atomic_load(&stop)) {
        int n = 200 + (int)(atomic_load(&calls) % 50);
        if (f(n) != expect(n)) atomic_fetch_add(&bad, 1);
        atomic_fetch_add(&calls, 1);
    }
    return 0;
}

static void *writer(void *arg) {
    (void)arg;
    while (!atomic_load(&stop)) {
        ((volatile unsigned char *)page)[DATA]++;
        atomic_fetch_add(&writes, 1);
        struct timespec t = {0, 20000};
        nanosleep(&t, 0);
    }
    return 0;
}

/* Fresh code in its own page, run a few times and unmapped: keeps blocks being built. */
static void *generator(void *arg) {
    (void)arg;
    while (!atomic_load(&stop)) {
        unsigned char *p = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { atomic_fetch_add(&bad, 1); return 0; }
        memcpy(p, code, sizeof code);
        int (*f)(int) = (int (*)(int))p;
        for (int k = 0; k < 4; k++)
            if (f(9) != expect(9)) atomic_fetch_add(&bad, 1);
        munmap(p, 4096);
        atomic_fetch_add(&fresh, 1);
    }
    return 0;
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    page = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return 2;
    for (int i = 0; i < COPIES; i++) memcpy(page + i * STRIDE, code, sizeof code);
    pthread_t t[COPIES + 2];
    for (int i = 0; i < COPIES; i++) pthread_create(&t[i], 0, runner, (void *)(uintptr_t)i);
    pthread_create(&t[COPIES], 0, writer, 0);
    pthread_create(&t[COPIES + 1], 0, generator, 0);
    struct timespec d = {secs, 0};
    nanosleep(&d, 0);
    atomic_store(&stop, 1);
    for (int i = 0; i < COPIES + 2; i++) pthread_join(t[i], 0);
    printf("%s calls=%lu writes=%lu fresh=%lu bad=%d\n", atomic_load(&bad) ? "FAIL" : "ok", atomic_load(&calls),
           atomic_load(&writes), atomic_load(&fresh), atomic_load(&bad));
    return atomic_load(&bad) != 0;
}

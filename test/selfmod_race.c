/* selfmod_race SECONDS [fixed]: one thread calls a function on a writable code page while another flips its first
 * instruction between "mov eax, 1" and a jump to a far function that returns 2, as a JIT patches a method's entry.
 * Each flip makes a sandboxed block rebuild. A rebuild that decoded the old bytes but emitted the new ones ran the
 * app's jump displacement from the code cache, landing far off in DynamoRIO's reservation (crash 2). Every call must
 * return 1 or 2. Prints "ok" and exits 0 when they did and both kinds were seen. With "fixed" the function stays the
 * jump and another byte of its page is written: the same blocks with no race, which pass on base too. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The jump target's distance: far enough that the misplaced displacement leaves the cache's live memory. */
#define FAR 0x30000000L

static unsigned char *fn;
static uint64_t ver_a, ver_b;
static atomic_int stop, fixed;
static atomic_ulong ones, twos, flips;

static void *runner(void *arg) {
    (void)arg;
    int (*f)(void) = (int (*)(void))fn;
    while (!atomic_load(&stop)) {
        int r = f();
        if (r == 1)
            atomic_fetch_add(&ones, 1);
        else if (r == 2)
            atomic_fetch_add(&twos, 1);
        else {
            printf("FAIL: call returned %d\n", r);
            fflush(stdout);
            _exit(1);
        }
    }
    return 0;
}

static void *writer(void *arg) {
    (void)arg;
    volatile uint64_t *w = (volatile uint64_t *)fn;
    while (!atomic_load(&stop) && atomic_load(&fixed)) {
        /* The function stays the jump; another byte of its page is written. */
        ((volatile unsigned char *)fn)[2048]++;
        atomic_fetch_add(&flips, 1);
        struct timespec t = {0, 20000};
        nanosleep(&t, 0);
    }
    while (!atomic_load(&stop)) {
        __atomic_store_n(w, ver_b, __ATOMIC_SEQ_CST);
        for (volatile int i = 0; i < 200; i++) {
        }
        __atomic_store_n(w, ver_a, __ATOMIC_SEQ_CST);
        for (volatile int i = 0; i < 200; i++) {
        }
        atomic_fetch_add(&flips, 1);
    }
    return 0;
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    atomic_store(&fixed, argc > 2 && !strcmp(argv[2], "fixed"));
    fn = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fn == MAP_FAILED) return 2;
    unsigned char *far = mmap(fn + FAR, 4096, PROT_READ | PROT_EXEC | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (far == MAP_FAILED) far = mmap(fn - FAR, 4096, PROT_READ | PROT_EXEC | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (far == MAP_FAILED) return 2;
    memcpy(far, "\xb8\x02\x00\x00\x00\xc3", 6); /* mov eax, 2; ret */
    unsigned char a[8] = {0xb8, 0x01, 0, 0, 0, 0xc3, 0x90, 0x90}; /* mov eax, 1; ret */
    unsigned char b[8] = {0xe9, 0, 0, 0, 0, 0xc3, 0x90, 0x90};    /* jmp far; ret */
    int32_t rel = (int32_t)(far - (fn + 5));
    memcpy(b + 1, &rel, 4);
    memcpy(&ver_a, a, 8);
    memcpy(&ver_b, b, 8);
    memcpy(fn, atomic_load(&fixed) ? b : a, 8);
    pthread_t r, w;
    pthread_create(&r, 0, runner, 0);
    pthread_create(&w, 0, writer, 0);
    struct timespec t = {secs, 0};
    nanosleep(&t, 0);
    atomic_store(&stop, 1);
    pthread_join(r, 0);
    pthread_join(w, 0);
    if ((!atomic_load(&ones) && !atomic_load(&fixed)) || !atomic_load(&twos)) {
        printf("FAIL: nothing tested, ones=%lu twos=%lu flips=%lu\n", atomic_load(&ones), atomic_load(&twos),
               atomic_load(&flips));
        return 1;
    }
    printf("ok ones=%lu twos=%lu flips=%lu\n", atomic_load(&ones), atomic_load(&twos), atomic_load(&flips));
    return 0;
}

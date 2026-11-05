// seg_threads: each thread's own fs base must be used by the rewritten operands. Threads hold different
// thread-local values and run loads and stores of expanded forms against them at the same time, the
// absolute (local-exec) form and the base-register form. Native checks the model.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define NT 6
#define ITERS 3000

__thread uint64_t tv[4] __attribute__((aligned(32)));
__thread uint64_t tw[4] __attribute__((aligned(32)));
static pthread_barrier_t bar;
static int bad[NT];

static void *run(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < 4; i++) tv[i] = 0x0123456789abcdefull * (uint64_t)(id + 1) + (uint64_t)i * 0x1111;
    uint64_t fsb;
    __asm__("mov %%fs:0, %0" : "=r"(fsb));
    uint64_t otv = (uint64_t)tv - fsb, otw = (uint64_t)tw - fsb;
    pthread_barrier_wait(&bar);
    for (int it = 0; it < ITERS; it++) {
        uint64_t a = 0x00ff00ff12345678ull ^ (uint64_t)it, r, r2, out[4] __attribute__((aligned(32))), in[4] __attribute__((aligned(32))) = {1, 2, 3, (uint64_t)it};
        __asm__("andn %%fs:tv@tpoff, %1, %0" : "=r"(r) : "r"(a));
        __asm__("andn %%fs:(%2), %1, %0" : "=r"(r2) : "r"(a), "r"(otv));
        if (r != (~a & tv[0]) || r2 != r) bad[id]++;
        __asm__("movbe %%fs:tv@tpoff+8, %0" : "=r"(r));
        if (r != __builtin_bswap64(tv[1])) bad[id]++;
        __asm__ volatile("movbe %0, %%fs:tw@tpoff+16" : : "r"(a) : "memory");
        __asm__ volatile("movbe %0, %%fs:(%1)" : : "r"(a), "r"(otw) : "memory");
        if (tw[2] != __builtin_bswap64(a) || tw[0] != __builtin_bswap64(a)) bad[id]++;
        // A positive absolute displacement: the thread control block, as the stack protector reads it.
        uint64_t can, t2[4] __attribute__((aligned(32)));
        __asm__("mov %%fs:0x28, %0" : "=r"(can));
        __asm__("andn %%fs:0x28, %1, %0" : "=r"(r) : "r"(a));
        if (r != (~a & can)) bad[id]++;
        __asm__("movbe %%fs:0x28, %0" : "=r"(r));
        if (r != __builtin_bswap64(can)) bad[id]++;
        __asm__ volatile("vmovdqu (%0), %%ymm1\n vpaddq %%fs:0x28, %%ymm1, %%ymm2\n vmovdqu %%ymm2, (%1)" : : "r"(in), "r"(t2) : "xmm1", "xmm2", "memory");
        for (int k = 0; k < 4; k++) {
            uint64_t w;
            __builtin_memcpy(&w, (char *)fsb + 0x28 + 8 * k, 8);
            if (t2[k] != in[k] + w) bad[id]++;
        }
        __asm__ volatile("vmovdqu (%0), %%ymm1\n vpaddq %%fs:tv@tpoff, %%ymm1, %%ymm2\n vmovdqu %%ymm2, (%1)" : : "r"(in), "r"(out) : "xmm1", "xmm2", "memory");
        for (int k = 0; k < 4; k++) if (out[k] != in[k] + tv[k]) bad[id]++;
        __asm__ volatile("vmovdqu (%0), %%ymm1\n vpmulld %%fs:(%2), %%ymm1, %%ymm2\n vmovdqu %%ymm2, (%1)" : : "r"(in), "r"(out), "r"(otv) : "xmm1", "xmm2", "memory");
        for (int k = 0; k < 4; k++) {
            uint32_t x0 = (uint32_t)in[k], x1 = (uint32_t)(in[k] >> 32), y0 = (uint32_t)tv[k], y1 = (uint32_t)(tv[k] >> 32);
            if (out[k] != ((uint64_t)(x0 * y0) | (uint64_t)(x1 * y1) << 32)) bad[id]++;
        }
    }
    return 0;
}

int main(void) {
    pthread_t t[NT];
    pthread_barrier_init(&bar, 0, NT);
    for (long i = 0; i < NT; i++) pthread_create(&t[i], 0, run, (void *)i);
    int total = 0;
    for (int i = 0; i < NT; i++) { pthread_join(t[i], 0); total += bad[i]; }
    printf("%s\n", total ? "FAIL" : "PASS");
    return total != 0;
}

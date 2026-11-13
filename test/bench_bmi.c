// bench_bmi: time per instruction of the scalar BMI forms, movbe and a few vector forms, for CI to run natively,
// under the client, and under the client with V3COMPAT_HELPERS_ONLY=1. Not a ctest: the numbers are
// for reading. Each loop body is 64 independent copies of the instruction, so what is timed is the
// instruction (or its expansion), not a dependency chain.
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define X8(s) s s s s s s s s
#define X64(s) X8(X8(s))

// A loop of n iterations over 64 copies; registers in use are rcx rdx rsi rdi r8 r9 r10 r11 and a buffer.
#define LOOP(name, body)                                                                 \
    static void name(uint64_t n, void *buf) {                                            \
        __asm__ volatile("mov $0x123456789abcdef, %%rdx\n mov $0xfedcba9876543210, %%rcx\n" \
                         "mov $0x1111, %%rsi\n"                                          \
                         "1:\n" X64(body "\n") "dec %0\n jnz 1b\n"                      \
                         : "+r"(n)                                                       \
                         : "D"(buf)                                                      \
                         : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "cc", "memory"); \
    }

LOOP(b_mov, "mov %%rcx, %%r8\n")
LOOP(b_rorx, "rorx $13, %%rcx, %%r8\n")
LOOP(b_andn, "andn %%rcx, %%rdx, %%r8\n")
LOOP(b_mulx, "mulx %%rcx, %%r8, %%r9\n")
LOOP(b_movbe_ld, "movbe (%%rdi), %%r8\n")
LOOP(b_movbe_st, "movbe %%rcx, (%%rdi)\n")
LOOP(b_shlx, "shlx %%rsi, %%rcx, %%r8\n")
LOOP(b_shrx, "shrx %%rsi, %%rcx, %%r8\n")
LOOP(b_sarx, "sarx %%rsi, %%rcx, %%r8\n")
LOOP(b_bzhi, "bzhi %%rsi, %%rcx, %%r8\n")
LOOP(b_tzcnt, "tzcnt %%rcx, %%r8\n")
LOOP(b_lzcnt, "lzcnt %%rcx, %%r8\n")

// Vector forms: ymm registers are clobbered, nothing else.
#define LOOPV(name, body)                                                                \
    static void name(uint64_t n, void *buf) {                                            \
        __asm__ volatile("1:\n" X64(body "\n") "dec %0\n jnz 1b\n"                      \
                         : "+r"(n)                                                       \
                         : "D"(buf)                                                      \
                         : "xmm0", "xmm1", "xmm2", "cc", "memory");                      \
    }
LOOPV(b_vpand, "vpand %%ymm1, %%ymm2, %%ymm0\n")
LOOPV(b_vpcmpeqd, "vpcmpeqd %%ymm1, %%ymm2, %%ymm0\n")
LOOPV(b_vpaddq, "vpaddq %%ymm1, %%ymm2, %%ymm0\n")
LOOPV(b_vpmovzxbq, "vpmovzxbq %%xmm1, %%ymm0\n")
LOOPV(b_vpbroadcastb, "vpbroadcastb %%xmm1, %%ymm0\n")
LOOPV(b_vpabsd, "vpabsd %%ymm1, %%ymm0\n")
LOOPV(b_vpermd, "vpermd %%ymm1, %%ymm2, %%ymm0\n")
LOOPV(b_vpbroadcastb_x, "vpbroadcastb %%xmm1, %%xmm0\n")
LOOPV(b_vpshufd, "vpshufd $0x1b, %%ymm1, %%ymm0\n")
LOOPV(b_vpermq, "vpermq $0x4e, %%ymm1, %%ymm0\n")
LOOPV(b_vpsllvq, "vpsllvq %%ymm1, %%ymm2, %%ymm0\n")   // stays a frame helper: the call's cost
LOOP(b_vpmovmskb, "vpmovmskb %%ymm1, %%r8d\n")

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

// Best of 5 repetitions, with the iteration count grown until one repetition takes over 50 ms.
static double ns_per_op(void (*f)(uint64_t, void *), void *buf) {
    uint64_t n = 16;
    while (1) {
        double t0 = now();
        f(n, buf);
        if (now() - t0 > 0.05) break;
        n *= 2;
    }
    double best = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now();
        f(n, buf);
        double dt = now() - t0;
        if (dt < best) best = dt;
    }
    return best * 1e9 / ((double)n * 64);
}

int main(void) {
    static uint8_t buf[64] __attribute__((aligned(64)));
    static const struct {
        const char *name;
        void (*f)(uint64_t, void *);
    } rows[] = {{"mov", b_mov}, {"rorx", b_rorx}, {"andn", b_andn},
                {"mulx", b_mulx}, {"movbe_load", b_movbe_ld}, {"movbe_store", b_movbe_st},
                {"shlx", b_shlx}, {"shrx", b_shrx}, {"sarx", b_sarx}, {"bzhi", b_bzhi}, {"tzcnt", b_tzcnt},
                {"lzcnt", b_lzcnt}, {"vpand", b_vpand}, {"vpcmpeqd", b_vpcmpeqd}, {"vpaddq", b_vpaddq},
                {"vpmovzxbq", b_vpmovzxbq}, {"vpbroadcastb", b_vpbroadcastb}, {"vpabsd", b_vpabsd},
                {"vpermd", b_vpermd}, {"vpbroadcastb_x", b_vpbroadcastb_x}, {"vpshufd", b_vpshufd},
                {"vpermq", b_vpermq}, {"vpmovmskb", b_vpmovmskb}, {"vpsllvq", b_vpsllvq}};
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
        printf("bench %-14s %10.2f ns/op\n", rows[i].name, ns_per_op(rows[i].f, buf));
    return 0;
}

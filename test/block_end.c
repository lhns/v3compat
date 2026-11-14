// block_end: an emulated instruction that ends a block runs once. DynamoRIO ends a block without a branch after
// 256 instructions, before the sixth store of a sandboxed block (JIT code) and at the end of a code region, and it
// takes the block's fall-through from the last application instruction the client left in it.
// Each run is UNITS units of (add, emulated, emulated) in a straight line: 256 is 1 mod 3, so successive blocks end
// on each of the three. The emulated forms accumulate, so one run twice shows in the result. Runs the same under
// V3COMPAT_HELPERS_ONLY=1; natively it checks the model.
#include <stdint.h>
#include <stdio.h>

#define UNITS 1500
#define STR_(x) #x
#define STR(x) STR_(x)
#define RUN(name, pre, emu, post)                                                                  \
    __asm__(".text\n.globl " #name "\n.type " #name ",@function\n" #name ":\n" pre                 \
            ".rept " STR(UNITS) "\nadd $1, %eax\n" emu "\n" emu "\n.endr\n" post "ret\n")

// ymm0 += in, per 32-bit lane; returns the adds counted in eax.
RUN(run_vec, "xor %eax, %eax\nvmovdqu (%rdi), %ymm1\nvpxor %xmm0, %xmm0, %xmm0\n", "vpaddd %ymm1, %ymm0, %ymm0",
    "vmovdqu %ymm0, (%rsi)\nvzeroupper\n");
// rcx rotates right by one.
RUN(run_bmi, "xor %eax, %eax\nmov %rdi, %rcx\n", "rorx $1, %rcx, %rcx", "mov %rcx, (%rsi)\n");
// xmm0 += a * b.
RUN(run_fma, "xor %eax, %eax\nvmovapd %xmm0, %xmm2\nvxorpd %xmm0, %xmm0, %xmm0\n", "vfmadd231sd %xmm2, %xmm1, %xmm0",
    "");
int run_vec(const uint32_t *in, uint32_t *out);
int run_bmi(uint64_t x, uint64_t *out);
double run_fma(double a, double b);

int main(void) {
    int bad = 0;
    uint32_t in[8], out[8];
    for (int i = 0; i < 8; i++) in[i] = 0x9E3779B1u * (uint32_t)(i + 1);
    int n = run_vec(in, out);
    if (n != UNITS) { printf("FAIL vec: %d adds, expected %d\n", n, UNITS); bad = 1; }
    for (int i = 0; i < 8; i++)
        if (out[i] != 2u * UNITS * in[i]) {
            printf("FAIL vec lane %d: %08x, expected %08x\n", i, out[i], 2u * UNITS * in[i]);
            bad = 1;
            break;
        }
    uint64_t x = 0x0123456789ABCDEFull, r, sh = 2 * UNITS % 64;
    n = run_bmi(x, &r);
    if (n != UNITS || r != (x >> sh | x << (64 - sh))) {
        printf("FAIL bmi: %d adds, %016llx, expected %016llx\n", n, (unsigned long long)r,
               (unsigned long long)(x >> sh | x << (64 - sh)));
        bad = 1;
    }
    double d = run_fma(1.5, 2.0);
    if (d != 3.0 * 2 * UNITS) { printf("FAIL fma: %.1f, expected %.1f\n", d, 3.0 * 2 * UNITS); bad = 1; }
    if (!bad) printf("PASS\n");
    return bad;
}

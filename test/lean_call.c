// lean_call: a frame helper changes nothing but its destination. Every GPR, every ymm at full width, the flags
// (DF set or clear), MXCSR, rsp and the red zone below it must be as before; the helper runs on its own stack.
// Run under the client with V3COMPAT_HELPERS_ONLY=1 too, which sends the inline forms the same way.
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#define ARITH 0x8D5ul
#define DF 0x400ul

struct st {
    uint64_t g[16];     // in DR order: rax rcx rdx rbx rsp(unused) rbp rsi rdi r8-r15
    uint64_t fl, mx, rsp;
    uint64_t gout[16];
    uint64_t flout, mxout, rspout;
    uint64_t rz[16], rzout[16];
    uint8_t y[16][32] __attribute__((aligned(32)));
    uint8_t yout[16][32] __attribute__((aligned(32)));
} S __attribute__((aligned(64)));

#define LOADY(i) "vmovdqu S+%c[y]+32*" #i "(%%rip), %%ymm" #i "\n"
#define SAVEY(i) "vmovdqu %%ymm" #i ", S+%c[yo]+32*" #i "(%%rip)\n"
#define RZ(i) "mov S+%c[rz]+8*" #i "(%%rip), %%rax\n mov %%rax, -128+8*" #i "(%%rsp)\n"
#define RZO(i) "mov -128+8*" #i "(%%rsp), %%rax\n mov %%rax, S+%c[rzo]+8*" #i "(%%rip)\n"
#define X4(m, a, b, c, d) m(a) m(b) m(c) m(d)
#define X16(m) X4(m, 0, 1, 2, 3) X4(m, 4, 5, 6, 7) X4(m, 8, 9, 10, 11) X4(m, 12, 13, 14, 15)

// One case: everything loaded from S, the instruction, everything stored back. rsp is checked directly.
#define CASE(name, insn)                                                                              \
    static void name(void) {                                                                          \
        __asm__ volatile(                                                                             \
            "push %%rbx\n push %%rbp\n push %%r12\n push %%r13\n push %%r14\n push %%r15\n"          \
            "mov %%rsp, S+%c[rsp](%%rip)\n ldmxcsr S+%c[mx](%%rip)\n" X16(LOADY)                         \
            /* the push is in the red zone, so the flags go first */                                  \
            "push S+%c[fl](%%rip)\n popfq\n" X16(RZ)                                                    \
            "mov S+0(%%rip), %%rax\n mov S+8(%%rip), %%rcx\n mov S+16(%%rip), %%rdx\n mov S+24(%%rip), %%rbx\n" \
            "mov S+40(%%rip), %%rbp\n mov S+48(%%rip), %%rsi\n mov S+56(%%rip), %%rdi\n"                 \
            "mov S+64(%%rip), %%r8\n mov S+72(%%rip), %%r9\n mov S+80(%%rip), %%r10\n mov S+88(%%rip), %%r11\n" \
            "mov S+96(%%rip), %%r12\n mov S+104(%%rip), %%r13\n mov S+112(%%rip), %%r14\n mov S+120(%%rip), %%r15\n" \
            insn "\n"                                                                                 \
            "mov %%rax, S+%c[go](%%rip)\n mov %%rcx, S+%c[go]+8(%%rip)\n mov %%rdx, S+%c[go]+16(%%rip)\n" \
            "mov %%rbx, S+%c[go]+24(%%rip)\n mov %%rsp, S+%c[rspo](%%rip)\n mov %%rbp, S+%c[go]+40(%%rip)\n" \
            "mov %%rsi, S+%c[go]+48(%%rip)\n mov %%rdi, S+%c[go]+56(%%rip)\n mov %%r8, S+%c[go]+64(%%rip)\n" \
            "mov %%r9, S+%c[go]+72(%%rip)\n mov %%r10, S+%c[go]+80(%%rip)\n mov %%r11, S+%c[go]+88(%%rip)\n" \
            "mov %%r12, S+%c[go]+96(%%rip)\n mov %%r13, S+%c[go]+104(%%rip)\n mov %%r14, S+%c[go]+112(%%rip)\n" \
            "mov %%r15, S+%c[go]+120(%%rip)\n" X16(SAVEY) X16(RZO)                                       \
            "pushfq\n pop S+%c[flo](%%rip)\n cld\n stmxcsr S+%c[mxo](%%rip)\n"                          \
            "pop %%r15\n pop %%r14\n pop %%r13\n pop %%r12\n pop %%rbp\n pop %%rbx\n"                  \
            :                                                                                         \
            : [rsp] "i"(offsetof(struct st, rsp)), [mx] "i"(offsetof(struct st, mx)),                  \
              [fl] "i"(offsetof(struct st, fl)), [go] "i"(offsetof(struct st, gout)),                 \
              [rspo] "i"(offsetof(struct st, rspout)), [flo] "i"(offsetof(struct st, flout)),          \
              [mxo] "i"(offsetof(struct st, mxout)), [y] "i"(offsetof(struct st, y)),                 \
              [yo] "i"(offsetof(struct st, yout)), [rz] "i"(offsetof(struct st, rz)),                 \
              [rzo] "i"(offsetof(struct st, rzout))                                                   \
            : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory", "xmm0",   \
              "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", \
              "xmm12", "xmm13", "xmm14", "xmm15");                                                    \
    }

// rdi points at buf; each case names the one register it writes (gpr index in S.g, or ymm number).
CASE(c_vpermd, "vpermd %%ymm1, %%ymm0, %%ymm2")
CASE(c_vpshufd_x, "vpshufd $0x1b, %%xmm3, %%xmm4")
CASE(c_vpermq_mem, "vpermq $0x4e, (%%rdi), %%ymm5")
CASE(c_vpmovmskb, "vpmovmskb %%ymm6, %%eax")
CASE(c_fma, "vfmadd231ps %%ymm7, %%ymm8, %%ymm9")
CASE(c_fma_mem, "vfmadd213sd (%%rdi), %%xmm10, %%xmm11")
CASE(c_vpaddq, "vpaddq %%ymm12, %%ymm13, %%ymm14")

static const struct {
    const char *name;
    void (*f)(void);
    int gdst, ydst;
    int fp;
} cases[] = {
    {"vpermd", c_vpermd, -1, 2, 0},       {"vpshufd xmm", c_vpshufd_x, -1, 4, 0},
    {"vpermq mem", c_vpermq_mem, -1, 5, 0}, {"vpmovmskb", c_vpmovmskb, 0, -1, 0},
    {"vfmadd231ps", c_fma, -1, 9, 1},     {"vfmadd213sd mem", c_fma_mem, -1, 11, 1},
    {"vpaddq", c_vpaddq, -1, 14, 0},
};

int main(void) {
    static double buf[4] __attribute__((aligned(32))) = {1.5, -2.25, 3.0, 0.5};
    int bad = 0;
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++)
        for (int k = 0; k < 4; k++) {
            memset(&S, 0, sizeof S);
            for (int i = 0; i < 16; i++) S.g[i] = 0x0101010101010101ull * (uint64_t)(i + 1) ^ (uint64_t)k << 60;
            S.g[7] = (uint64_t)buf;
            S.fl = 2 | (k & 1 ? ARITH : 0) | (k & 2 ? DF : 0);
            S.mx = 0x1F80 | 0x4000;   // round up: a helper running with a changed MXCSR would show
            for (int i = 0; i < 16; i++) {
                S.rz[i] = 0x5a5a5a5a00000000ull | (uint64_t)(i * 7 + k);
                for (int b = 0; b < 32; b++) S.y[i][b] = (uint8_t)(i * 37 + b * 11 + k + 1);
            }
            for (int i = 0; i < 16; i++)   // small floats where FMA reads, so nothing raises
                if (i >= 7 && i <= 11)
                    for (int l = 0; l < 8; l++) { float f = 1.0f + (float)l; memcpy(&S.y[i][4 * l], &f, 4); }
            cases[c].f();
            __builtin_ia32_ldmxcsr(0x1F80);
            const char *n = cases[c].name;
            for (int i = 0; i < 16; i++)
                if (i != 4 && i != cases[c].gdst && S.gout[i] != S.g[i]) {
                    printf("FAIL %s k=%d: gpr %d %llx -> %llx\n", n, k, i, (unsigned long long)S.g[i],
                           (unsigned long long)S.gout[i]);
                    bad++;
                }
            if (S.rspout != S.rsp) { printf("FAIL %s k=%d: rsp\n", n, k); bad++; }
            if ((S.flout & (ARITH | DF)) != (S.fl & (ARITH | DF))) {
                printf("FAIL %s k=%d: flags %llx -> %llx\n", n, k, (unsigned long long)S.fl, (unsigned long long)S.flout);
                bad++;
            }
            if (cases[c].fp ? (S.mxout & ~0x3Full) != S.mx : S.mxout != S.mx) {
                printf("FAIL %s k=%d: mxcsr %llx -> %llx\n", n, k, (unsigned long long)S.mx, (unsigned long long)S.mxout);
                bad++;
            }
            for (int i = 0; i < 16; i++)
                if (i != cases[c].ydst && memcmp(S.y[i], S.yout[i], 32)) { printf("FAIL %s k=%d: ymm%d\n", n, k, i); bad++; }
            if (memcmp(S.rz, S.rzout, sizeof S.rz)) { printf("FAIL %s k=%d: red zone\n", n, k); bad++; }
        }
    if (bad) return 1;
    printf("PASS\n");
    return 0;
}

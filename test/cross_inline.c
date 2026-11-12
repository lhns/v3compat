// cross_inline: the lane-crossing forms and vpmovmskb the client expands inline, against C models. Shapes:
// register and memory sources, ymm13..15 as operands (the scratch moves to ymm10..12, which must come back
// unchanged), the destination equal to a source, and several immediates. Also checked per case: the sources
// and the scratch are unchanged. Runs the same under V3COMPAT_HELPERS_ONLY=1; natively it checks the models.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// run(in, m, out): ymm1 = in[0..32) (A), ymm2 = in[128..160) (B), ymm13..15 = in[32..128); out = ymm0, ymm13..15,
// ymm1, ymm2 and rax (8 bytes after them). (%rsi) is a copy of B.
#define STUBV(name, body)                                                                                 \
    __asm__(".text\n.globl " #name "\n.type " #name ",@function\n" #name ":\n"                            \
            "vmovdqu (%rdi), %ymm1\nvmovdqu 128(%rdi), %ymm2\nvmovdqu 32(%rdi), %ymm13\n"                    \
            "vmovdqu 64(%rdi), %ymm14\nvmovdqu 96(%rdi), %ymm15\nmov $-1, %rax\nmov $-1, %r9\n" body        \
            "vmovdqu %ymm0, (%rdx)\nvmovdqu %ymm13, 32(%rdx)\nvmovdqu %ymm14, 64(%rdx)\n"                 \
            "vmovdqu %ymm15, 96(%rdx)\nvmovdqu %ymm1, 128(%rdx)\nvmovdqu %ymm2, 160(%rdx)\n"                \
            "mov %rax, 192(%rdx)\nvzeroupper\nret\n")
// "hi": operands ymm13 (= A), ymm14 (= B), result in ymm15; ymm10..12 hold the filler and come back in the ymm13..15 slots.
#define PRE_HI "vmovdqu 32(%rdi), %ymm10\nvmovdqu 64(%rdi), %ymm11\nvmovdqu 96(%rdi), %ymm12\n"               \
               "vmovdqa %ymm1, %ymm13\nvmovdqa %ymm2, %ymm14\n"
#define POST_HI "vmovdqa %ymm15, %ymm0\nvmovdqa %ymm10, %ymm13\nvmovdqa %ymm11, %ymm14\nvmovdqa %ymm12, %ymm15\n"
#define KEEP1 "vmovdqa %ymm1, %ymm0\nvmovdqu (%rdi), %ymm1\n"
#define KEEP2 "vmovdqa %ymm2, %ymm0\nvmovdqu 128(%rdi), %ymm2\n"

// vpermd/vpermps: index A, source B.
#define PERMD(mn)                                                                                         \
    STUBV(t_##mn##_r, #mn " %ymm2, %ymm1, %ymm0\n");                                                      \
    STUBV(t_##mn##_m, #mn " (%rsi), %ymm1, %ymm0\n");                                                     \
    STUBV(t_##mn##_h, PRE_HI #mn " %ymm14, %ymm13, %ymm15\n" POST_HI);                                    \
    STUBV(t_##mn##_hm, PRE_HI #mn " (%rsi), %ymm13, %ymm15\n" POST_HI);                                   \
    STUBV(t_##mn##_a, #mn " %ymm2, %ymm1, %ymm1\n" KEEP1);                                                \
    STUBV(t_##mn##_b, #mn " %ymm2, %ymm1, %ymm2\n" KEEP2);                                                \
    STUBV(t_##mn##_s, #mn " %ymm2, %ymm2, %ymm0\n")
PERMD(vpermd)
PERMD(vpermps)

// Immediate forms: source B.
#define IMMS(X, mn) X(mn, 0x1b) X(mn, 0x4e) X(mn, 0x72) X(mn, 0xff) X(mn, 0x00)
#define IMM1(mn, i)                                                                                       \
    STUBV(t_##mn##_r##i, #mn " $" #i ", %ymm2, %ymm0\n");                                                 \
    STUBV(t_##mn##_m##i, #mn " $" #i ", (%rsi), %ymm0\n");                                                \
    STUBV(t_##mn##_h##i, PRE_HI #mn " $" #i ", %ymm14, %ymm15\n" POST_HI);                                \
    STUBV(t_##mn##_hm##i, PRE_HI #mn " $" #i ", (%rsi), %ymm15\n" POST_HI);                               \
    STUBV(t_##mn##_a##i, #mn " $" #i ", %ymm2, %ymm2\n" KEEP2);
#define IMMF(mn) IMMS(IMM1, mn)
#define IMMFORMS(X) X(vpermq) X(vpermpd) X(vpshufd) X(vpshufhw) X(vpshuflw)
IMMFORMS(IMMF)

// vpmovmskb: source B, to eax, r9d or rax (and rax as a 64-bit destination).
STUBV(t_msk_r, "vpmovmskb %ymm2, %eax\n");
STUBV(t_msk_q, "vpmovmskb %ymm2, %rax\n");
STUBV(t_msk_9, "vpmovmskb %ymm2, %r9d\nmov %r9, %rax\n");
STUBV(t_msk_h, PRE_HI "vpmovmskb %ymm14, %eax\n" POST_HI);

typedef void (*fn)(const void *, const void *, void *);
typedef struct { uint8_t y[6][32]; uint64_t rax; } out_t;

#define DECL_P(mn) extern void t_##mn##_r(), t_##mn##_m(), t_##mn##_h(), t_##mn##_hm(), t_##mn##_a(), t_##mn##_b(), t_##mn##_s();
DECL_P(vpermd) DECL_P(vpermps)
#define DECL_I1(mn, i) extern void t_##mn##_r##i(), t_##mn##_m##i(), t_##mn##_h##i(), t_##mn##_hm##i(), t_##mn##_a##i();
#define DECL_I(mn) IMMS(DECL_I1, mn)
IMMFORMS(DECL_I)
extern void t_msk_r(), t_msk_q(), t_msk_9(), t_msk_h();

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static int failures;

static void bad(const char *name, const char *shape, const uint8_t *got, const uint8_t *want) {
    printf("FAIL %s %s:", name, shape);
    for (int i = 0; i < 32; i++) printf(" %02x", got[i]);
    printf("\n  want   ");
    for (int i = 0; i < 32; i++) printf(" %02x", want[i]);
    printf("\n");
    failures++;
}

// The sources (unless the shape writes one: kept is 1 or 2) and the scratch slots must come back unchanged.
static void check(const char *name, const char *shape, fn f, const uint8_t *in, const uint8_t *want) {
    out_t o;
    uint8_t m[32] __attribute__((aligned(32)));
    memcpy(m, in + 128, 32);
    f(in, m, &o);
    if (memcmp(o.y[0], want, 32)) bad(name, shape, o.y[0], want);
    if (memcmp(o.y[1], in + 32, 96)) { printf("FAIL %s %s: ymm13..15 changed\n", name, shape); failures++; }
    if (memcmp(o.y[4], in, 32) || memcmp(o.y[5], in + 128, 32)) { printf("FAIL %s %s: a source changed\n", name, shape); failures++; }
}

static void m_permd(uint8_t *out, const uint8_t *idx, const uint8_t *src) {
    for (int i = 0; i < 8; i++) {
        uint32_t k;
        memcpy(&k, idx + 4 * i, 4);
        memcpy(out + 4 * i, src + 4 * (k & 7), 4);
    }
}
static void m_imm(int form, uint8_t *out, const uint8_t *s, unsigned imm) {
    switch (form) {
    case 0: case 1:   // vpermq, vpermpd
        for (int i = 0; i < 4; i++) memcpy(out + 8 * i, s + 8 * ((imm >> (2 * i)) & 3), 8);
        break;
    case 2:   // vpshufd
        for (int l = 0; l < 2; l++)
            for (int i = 0; i < 4; i++) memcpy(out + 16 * l + 4 * i, s + 16 * l + 4 * ((imm >> (2 * i)) & 3), 4);
        break;
    case 3:   // vpshufhw
        for (int l = 0; l < 2; l++) {
            memcpy(out + 16 * l, s + 16 * l, 8);
            for (int i = 0; i < 4; i++) memcpy(out + 16 * l + 8 + 2 * i, s + 16 * l + 8 + 2 * ((imm >> (2 * i)) & 3), 2);
        }
        break;
    default:   // vpshuflw
        for (int l = 0; l < 2; l++) {
            for (int i = 0; i < 4; i++) memcpy(out + 16 * l + 2 * i, s + 16 * l + 2 * ((imm >> (2 * i)) & 3), 2);
            memcpy(out + 16 * l + 8, s + 16 * l + 8, 8);
        }
    }
}

int main(void) {
    for (int iter = 0; iter < 200 && failures < 20; iter++) {
        uint8_t in[160] __attribute__((aligned(32))), want[32], w2[32];
        for (int i = 0; i < 160; i++) in[i] = (uint8_t)rnd();
        if (iter % 3 == 0) for (int i = 0; i < 32; i += 4) in[i] &= 7;   // in-range indices besides wide ones
        const uint8_t *A = in, *B = in + 128;
#define RUN_P(mn)                                                                                         \
        m_permd(want, A, B);                                                                              \
        check(#mn, "reg", (fn)t_##mn##_r, in, want);                                                      \
        check(#mn, "mem", (fn)t_##mn##_m, in, want);                                                      \
        check(#mn, "ymm13-15", (fn)t_##mn##_h, in, want);                                                 \
        check(#mn, "ymm13-15 mem", (fn)t_##mn##_hm, in, want);                                            \
        m_permd(w2, B, B);                                                                                \
        check(#mn, "index=source", (fn)t_##mn##_s, in, w2);                                               \
        {                                                                                                 \
            out_t o;                                                                                      \
            ((fn)t_##mn##_a)(in, B, &o);                                                                  \
            if (memcmp(o.y[0], want, 32)) bad(#mn, "dst=index", o.y[0], want);                            \
            ((fn)t_##mn##_b)(in, B, &o);                                                                  \
            if (memcmp(o.y[0], want, 32)) bad(#mn, "dst=source", o.y[0], want);                           \
        }
        RUN_P(vpermd) RUN_P(vpermps)
        int form = 0;
#define RUN_I1(mn, i)                                                                                     \
        m_imm(form, want, B, i);                                                                          \
        check(#mn "/" #i, "reg", (fn)t_##mn##_r##i, in, want);                                            \
        check(#mn "/" #i, "mem", (fn)t_##mn##_m##i, in, want);                                            \
        check(#mn "/" #i, "ymm13-15", (fn)t_##mn##_h##i, in, want);                                       \
        check(#mn "/" #i, "ymm13-15 mem", (fn)t_##mn##_hm##i, in, want);                                  \
        {                                                                                                 \
            out_t o;                                                                                      \
            ((fn)t_##mn##_a##i)(in, B, &o);                                                               \
            if (memcmp(o.y[0], want, 32)) bad(#mn "/" #i, "dst=source", o.y[0], want);                    \
        }
#define RUN_I(mn) IMMS(RUN_I1, mn) form++;
        IMMFORMS(RUN_I)
        uint64_t mask = 0;
        for (int i = 0; i < 32; i++) mask |= (uint64_t)(B[i] >> 7) << i;
        static const struct { const char *shape; void (*f)(); } msk[] = {
            {"eax", t_msk_r}, {"rax", t_msk_q}, {"r9d", t_msk_9}, {"ymm14", t_msk_h}};
        for (int k = 0; k < 4; k++) {
            out_t o;
            ((fn)msk[k].f)(in, B, &o);
            if (o.rax != mask) {
                printf("FAIL vpmovmskb %s: %llx want %llx\n", msk[k].shape, (unsigned long long)o.rax, (unsigned long long)mask);
                failures++;
            }
            if (memcmp(o.y[1], in + 32, 96)) { printf("FAIL vpmovmskb %s: ymm13..15 changed\n", msk[k].shape); failures++; }
        }
    }
    puts(failures ? "FAIL" : "PASS");
    return failures != 0;
}

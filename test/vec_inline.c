// vec_inline: the vector forms the client expands inline or by substitution, against a reference that does not
// involve the client: each lane-wise form is compared with the same operation on the two xmm halves (AVX1), the
// widening, broadcast and abs forms with a C model. Register and memory sources, random data. Also checked
// per case: the sources and the three highest ymm registers (the scratch) are unchanged, and a memory source
// is read for exactly the bytes the instruction reads (it sits at the end of a page, the next page is PROT_NONE).
// Runs the same under V3COMPAT_HELPERS_ONLY=1; natively it checks the model.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

// run(a, m, out): ymm1 = a[0..32), ymm13..15 = a[32..128), ymm2 = a[128..160); BODY leaves its result in ymm0;
// out = ymm0, ymm13..15, ymm1, ymm2 (32 bytes each). The instruction's memory operand is (%rsi), which the
// caller points at a copy of ymm2 (or at the end of a page).
#define STUBV(name, body)                                                                                 \
    __asm__(".text\n.globl " #name "\n.type " #name ",@function\n" #name ":\n"                            \
            "vmovdqu (%rdi), %ymm1\nvmovdqu (%rsi), %ymm2\nvmovdqu 32(%rdi), %ymm13\n"                    \
            "vmovdqu 64(%rdi), %ymm14\nvmovdqu 96(%rdi), %ymm15\n" body                                   \
            "vmovdqu %ymm0, (%rdx)\nvmovdqu %ymm13, 32(%rdx)\nvmovdqu %ymm14, 64(%rdx)\n"                 \
            "vmovdqu %ymm15, 96(%rdx)\nvmovdqu %ymm1, 128(%rdx)\nvmovdqu %ymm2, 160(%rdx)\nret\n");

// Lane-wise, two sources: the ymm form and its xmm-halves reference.
#define HALVES(mn, a, b)                                                                                  \
    "vextractf128 $1, %ymm1, %xmm5\nvextractf128 $1, %ymm2, %xmm6\n" mn " %xmm2, %xmm1, %xmm4\n"          \
    mn " %xmm6, %xmm5, %xmm5\nvinsertf128 $1, %xmm5, %ymm4, %ymm0\n"
#define BIN(mn)                                                                                           \
    STUBV(t_##mn##_r, #mn " %ymm2, %ymm1, %ymm0\n");                                                      \
    STUBV(t_##mn##_m, #mn " (%rsi), %ymm1, %ymm0\n");                                                     \
    STUBV(r_##mn, HALVES(#mn, 0, 0))
// Shifts: by an xmm count (register, memory) and by an immediate.
#define SHC(mn)                                                                                           \
    STUBV(t_##mn##_r, #mn " %xmm2, %ymm1, %ymm0\n");                                                      \
    STUBV(t_##mn##_m, #mn " (%rsi), %ymm1, %ymm0\n");                                                     \
    STUBV(r_##mn, "vextractf128 $1, %ymm1, %xmm5\n" #mn " %xmm2, %xmm1, %xmm4\n" #mn                      \
                  " %xmm2, %xmm5, %xmm5\nvinsertf128 $1, %xmm5, %ymm4, %ymm0\n");                         \
    STUBV(t_##mn##_i, #mn " $3, %ymm1, %ymm0\n");                                                         \
    STUBV(r_##mn##_i, "vextractf128 $1, %ymm1, %xmm5\n" #mn " $3, %xmm1, %xmm4\n" #mn                     \
                      " $3, %xmm5, %xmm5\nvinsertf128 $1, %xmm5, %ymm4, %ymm0\n")

#define BINS(X) X(vpaddb) X(vpaddw) X(vpaddd) X(vpaddq) X(vpsubb) X(vpsubw) X(vpsubd) X(vpsubq)               \
    X(vpcmpeqb) X(vpcmpeqw) X(vpcmpeqd) X(vpcmpeqq) X(vpcmpgtb) X(vpcmpgtw) X(vpcmpgtd) X(vpcmpgtq)          \
    X(vpminub) X(vpminuw) X(vpminud) X(vpminsb) X(vpminsw) X(vpminsd) X(vpmaxub) X(vpmaxuw) X(vpmaxud)       \
    X(vpmaxsb) X(vpmaxsw) X(vpmaxsd) X(vpmullw) X(vpmulld) X(vpmuludq) X(vpshufb)                            \
    X(vpand) X(vpandn) X(vpor) X(vpxor)
#define SHS(X) X(vpsllw) X(vpslld) X(vpsllq) X(vpsrlw) X(vpsrld) X(vpsrlq) X(vpsraw) X(vpsrad)
BINS(BIN)
SHS(SHC)

// One-source forms against a C model: source ymm2/xmm2 or (%rsi).
#define UN(mn) STUBV(t_##mn##_r, #mn " %xmm2, %ymm0\n"); STUBV(t_##mn##_m, #mn " (%rsi), %ymm0\n")
#define UNY(mn) STUBV(t_##mn##_r, #mn " %ymm2, %ymm0\n"); STUBV(t_##mn##_m, #mn " (%rsi), %ymm0\n")
#define WIDENS(X) X(vpmovzxbw) X(vpmovzxbd) X(vpmovzxbq) X(vpmovzxwd) X(vpmovzxwq) X(vpmovzxdq)                \
    X(vpmovsxbw) X(vpmovsxbd) X(vpmovsxbq) X(vpmovsxwd) X(vpmovsxwq) X(vpmovsxdq)
#define BCASTS(X) X(vpbroadcastb) X(vpbroadcastw) X(vpbroadcastd) X(vpbroadcastq)
#define ABSS(X) X(vpabsb) X(vpabsw) X(vpabsd)
WIDENS(UN)
BCASTS(UN)
ABSS(UNY)

typedef void (*fn)(const void *, const void *, void *);
#define DECL_BIN(mn) extern void t_##mn##_r(), t_##mn##_m(), r_##mn();
#define DECL_SH(mn) extern void t_##mn##_r(), t_##mn##_m(), r_##mn(), t_##mn##_i(), r_##mn##_i();
#define DECL_UN(mn) extern void t_##mn##_r(), t_##mn##_m();
BINS(DECL_BIN)
SHS(DECL_SH)
WIDENS(DECL_UN)
BCASTS(DECL_UN)
ABSS(DECL_UN)

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static int failures;
static const char *volatile cur;   // the case running, for a crash report
static void on_segv(int sig) {
    (void)sig;
    write(2, "SEGV in ", 8);
    write(2, cur, strlen(cur));
    write(2, "\n", 1);
    _exit(3);
}
typedef struct { uint8_t y[6][32]; } out_t;   // result, ymm13..15, ymm1, ymm2

static void check_common(const char *name, const char *form, const uint8_t *in, const uint8_t *b, const out_t *o) {
    if (memcmp(o->y[1], in + 32, 96)) { printf("FAIL %s %s: ymm13..15 changed\n", name, form); failures++; }
    if (memcmp(o->y[4], in, 32) || memcmp(o->y[5], in + 128, 32)) { printf("FAIL %s %s: a source changed\n", name, form); failures++; }
}

static void bad(const char *name, const char *form, const uint8_t *got, const uint8_t *want) {
    printf("FAIL %s %s:", name, form);
    for (int i = 0; i < 32; i++) printf(" %02x", got[i]);
    printf("\n  want   ");
    for (int i = 0; i < 32; i++) printf(" %02x", want[i]);
    printf("\n");
    failures++;
}

// The memory operand at the end of a page followed by a PROT_NONE page: `n` bytes are all the instruction may read.
static uint8_t *edge_buf(void) {
    uint8_t *p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(p + 4096, 4096, PROT_NONE);
    return p;
}

static void lanewise(const char *name, fn t_r, fn t_m, fn ref, int iter) {
    cur = name;
    uint8_t in[160] __attribute__((aligned(32))), b[32] __attribute__((aligned(32)));
    out_t o, want;
    for (int i = 0; i < 128; i++) in[i] = (uint8_t)rnd();
    for (int i = 0; i < 32; i++) b[i] = (iter % 3 == 0) ? in[i] : (uint8_t)rnd();   // equal operands: compare results
    if (iter % 4 == 1) for (int i = 0; i < 32; i++) b[i] &= 0x0f;                   // small shuffle indices
    if (iter % 4 == 2) memset(b + 1, 0, 15);                                         // a small shift count
    memcpy(in + 128, b, 32);
    ref(in, b, &want);
    t_r(in, b, &o);
    if (memcmp(o.y[0], want.y[0], 32)) bad(name, "reg", o.y[0], want.y[0]);
    check_common(name, "reg", in, b, &o);
    t_m(in, b, &o);
    if (memcmp(o.y[0], want.y[0], 32)) bad(name, "mem", o.y[0], want.y[0]);
    check_common(name, "mem", in, b, &o);
}

// C models of the one-source forms. e: source element bytes, d: destination element bytes.
static void model_widen(uint8_t *out, const uint8_t *b, int e, int d, int sign) {
    for (int i = 0; i < 32 / d; i++) {
        int64_t v = 0;
        memcpy(&v, b + i * e, e);
        if (sign && (v >> (8 * e - 1)) & 1) v |= -(1ll << (8 * e));
        memcpy(out + i * d, &v, d);
    }
}
static void model_bcast(uint8_t *out, const uint8_t *b, int e) {
    for (int i = 0; i < 32; i += e) memcpy(out + i, b, e);
}
static void model_abs(uint8_t *out, const uint8_t *b, int e) {
    for (int i = 0; i < 32; i += e) {
        int64_t v = 0;
        memcpy(&v, b + i, e);
        v = (v << (64 - 8 * e)) >> (64 - 8 * e);
        if (v < 0) v = -v;
        memcpy(out + i, &v, e);
    }
}

// kind 0 widen, 1 broadcast, 2 abs. `bytes` is what a memory source may read.
static void oneop(const char *name, fn t_r, fn t_m, int kind, int e, int d, int sign, int bytes, int iter) {
    cur = name;
    uint8_t in[160] __attribute__((aligned(32))), b[32] __attribute__((aligned(32))), want[32];
    out_t o;
    for (int i = 0; i < 128; i++) in[i] = (uint8_t)rnd();
    for (int i = 0; i < 32; i++) b[i] = (uint8_t)rnd();
    if (iter % 3 == 0) memset(b, 0x80, 32);
    if (iter % 3 == 1) for (int i = 0; i < 32; i++) b[i] &= 0x81;
    memcpy(in + 128, b, 32);
    if (kind == 0) model_widen(want, b, e, d, sign);
    else if (kind == 1) model_bcast(want, b, e);
    else model_abs(want, b, e);
    t_r(in, b, &o);
    if (memcmp(o.y[0], want, 32)) bad(name, "reg", o.y[0], want);
    check_common(name, "reg", in, b, &o);
    t_m(in, b, &o);
    if (memcmp(o.y[0], want, 32)) bad(name, "mem", o.y[0], want);
    check_common(name, "mem", in, b, &o);
    if (iter == 0 && bytes < 32) {   // exactly `bytes` readable: the form must not touch the PROT_NONE page
        static uint8_t *edge;
        if (!edge) edge = edge_buf();
        uint8_t *src = edge + 4096 - bytes;
        memcpy(src, b, bytes);
        out_t o2;
        t_m(in, src, &o2);   // reads (%rsi) = src
        if (memcmp(o2.y[0], want, 32)) bad(name, "mem at page end", o2.y[0], want);
    }
}

int main(void) {
    signal(SIGSEGV, on_segv);
    for (int iter = 0; iter < 60; iter++) {
#define RUN_BIN(mn) lanewise(#mn, (fn)t_##mn##_r, (fn)t_##mn##_m, (fn)r_##mn, iter);
        BINS(RUN_BIN)
#define RUN_SH(mn) lanewise(#mn, (fn)t_##mn##_r, (fn)t_##mn##_m, (fn)r_##mn, iter);                            \
    {                                                                                                         \
        uint8_t in[160] __attribute__((aligned(32))), b[32] __attribute__((aligned(32)));                      \
        out_t o, want;                                                                                        \
        for (int i = 0; i < 128; i++) in[i] = (uint8_t)rnd();                                                  \
        memset(b, 0, 32); memset(in + 128, 0, 32);                                                                                   \
        ((fn)r_##mn##_i)(in, b, &want);                                                                       \
        ((fn)t_##mn##_i)(in, b, &o);                                                                          \
        if (memcmp(o.y[0], want.y[0], 32)) bad(#mn, "imm", o.y[0], want.y[0]);                                  \
        check_common(#mn, "imm", in, b, &o);                                                                  \
    }
        SHS(RUN_SH)
#define W(mn, e, d, s, by) oneop(#mn, (fn)t_##mn##_r, (fn)t_##mn##_m, 0, e, d, s, by, iter);
        W(vpmovzxbw, 1, 2, 0, 16) W(vpmovzxbd, 1, 4, 0, 8) W(vpmovzxbq, 1, 8, 0, 4)
        W(vpmovzxwd, 2, 4, 0, 16) W(vpmovzxwq, 2, 8, 0, 8) W(vpmovzxdq, 4, 8, 0, 16)
        W(vpmovsxbw, 1, 2, 1, 16) W(vpmovsxbd, 1, 4, 1, 8) W(vpmovsxbq, 1, 8, 1, 4)
        W(vpmovsxwd, 2, 4, 1, 16) W(vpmovsxwq, 2, 8, 1, 8) W(vpmovsxdq, 4, 8, 1, 16)
#define B(mn, e) oneop(#mn, (fn)t_##mn##_r, (fn)t_##mn##_m, 1, e, 0, 0, e, iter);
        B(vpbroadcastb, 1) B(vpbroadcastw, 2) B(vpbroadcastd, 4) B(vpbroadcastq, 8)
#define A(mn, e) oneop(#mn, (fn)t_##mn##_r, (fn)t_##mn##_m, 2, e, 0, 0, 32, iter);
        A(vpabsb, 1) A(vpabsw, 2) A(vpabsd, 4)
        if (failures > 20) break;
    }
    puts(failures ? "FAIL" : "PASS");
    return failures != 0;
}

// vec_inline: the vector forms the client expands inline or by substitution, against a reference that does not
// involve the client: each lane-wise form is compared with the same operation on the two xmm halves (AVX1), the
// widening, broadcast and abs forms with a C model. Register and memory sources, random data. Also checked
// per case: the sources and the three highest ymm registers (the scratch) are unchanged, and a memory source
// is read for exactly the bytes the instruction reads (it sits at the end of a page, the next page is PROT_NONE).
// Operand shapes besides the plain one: ymm13..15 as operands (the scratch registers move to ymm10..12, which must come
// back unchanged), and the destination equal to a source. Shifts also take immediates at and beyond the element width
// and counts whose high 64 bits are not zero (only the low 64 bits count).
// Runs the same under V3COMPAT_HELPERS_ONLY=1; natively it checks the model.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

// run(a, m, out): ymm1 = a[0..32), ymm13..15 = a[32..128), ymm2 = a[128..160); BODY leaves its result in ymm0;
// out = ymm0, ymm13..15, ymm1, ymm2 (32 bytes each). The instruction's memory operand is (%rsi), which the
// caller points at a copy of ymm2 (or at the end of a page).
#define STUBV(name, body)                                                                                 \
    __asm__(".text\n.globl " #name "\n.type " #name ",@function\n" #name ":\n"                            \
            "vmovdqu (%rdi), %ymm1\nvmovdqu 128(%rdi), %ymm2\nvmovdqu 32(%rdi), %ymm13\n"                    \
            "vmovdqu 64(%rdi), %ymm14\nvmovdqu 96(%rdi), %ymm15\n" body                                   \
            "vmovdqu %ymm0, (%rdx)\nvmovdqu %ymm13, 32(%rdx)\nvmovdqu %ymm14, 64(%rdx)\n"                 \
            "vmovdqu %ymm15, 96(%rdx)\nvmovdqu %ymm1, 128(%rdx)\nvmovdqu %ymm2, 160(%rdx)\nret\n");

// "hi" shape: the operands are ymm13 (= ymm1), ymm14 (= ymm2) and ymm15, so the scratch registers are ymm10..12; they
// hold a known value before and are handed back in the ymm13..15 slots of out. KEEP1 and KEEP2: the result of an
// instruction that wrote ymm1 or ymm2 goes to ymm0 and the input comes back.
#define PRE_HI "vmovdqu 32(%rdi), %ymm10\nvmovdqu 64(%rdi), %ymm11\nvmovdqu 96(%rdi), %ymm12\n"               \
               "vmovdqa %ymm1, %ymm13\nvmovdqa %ymm2, %ymm14\n"
#define POST_HI "vmovdqa %ymm15, %ymm0\nvmovdqa %ymm10, %ymm13\nvmovdqa %ymm11, %ymm14\nvmovdqa %ymm12, %ymm15\n"
#define KEEP1 "vmovdqa %ymm1, %ymm0\nvmovdqu (%rdi), %ymm1\n"
#define KEEP2 "vmovdqa %ymm2, %ymm0\nvmovdqu 128(%rdi), %ymm2\n"

// Lane-wise, two sources: the ymm form and its xmm-halves reference.
#define HALVES(mn)                                                                                  \
    "vextractf128 $1, %ymm1, %xmm5\nvextractf128 $1, %ymm2, %xmm6\n" mn " %xmm2, %xmm1, %xmm4\n"          \
    mn " %xmm6, %xmm5, %xmm5\nvinsertf128 $1, %xmm5, %ymm4, %ymm0\n"
#define BIN(mn)                                                                                           \
    STUBV(t_##mn##_r, #mn " %ymm2, %ymm1, %ymm0\n");                                                      \
    STUBV(t_##mn##_m, #mn " (%rsi), %ymm1, %ymm0\n");                                                     \
    STUBV(t_##mn##_h, PRE_HI #mn " %ymm14, %ymm13, %ymm15\n" POST_HI);                                    \
    STUBV(t_##mn##_hm, PRE_HI #mn " (%rsi), %ymm13, %ymm15\n" POST_HI);                                   \
    STUBV(t_##mn##_a, #mn " %ymm2, %ymm1, %ymm1\n" KEEP1);                                                \
    STUBV(t_##mn##_am, #mn " (%rsi), %ymm1, %ymm1\n" KEEP1);                                              \
    STUBV(t_##mn##_b, #mn " %ymm2, %ymm1, %ymm2\n" KEEP2);                                                \
    STUBV(r_##mn, HALVES(#mn))
// Shifts: by an xmm count (register, memory) and by an immediate. The count of the "c" shape is the destination.
#define SHC(mn)                                                                                           \
    STUBV(t_##mn##_r, #mn " %xmm2, %ymm1, %ymm0\n");                                                      \
    STUBV(t_##mn##_m, #mn " (%rsi), %ymm1, %ymm0\n");                                                     \
    STUBV(t_##mn##_h, PRE_HI #mn " %xmm14, %ymm13, %ymm15\n" POST_HI);                                    \
    STUBV(t_##mn##_hm, PRE_HI #mn " (%rsi), %ymm13, %ymm15\n" POST_HI);                                   \
    STUBV(t_##mn##_a, #mn " %xmm2, %ymm1, %ymm1\n" KEEP1);                                                \
    STUBV(t_##mn##_am, #mn " (%rsi), %ymm1, %ymm1\n" KEEP1);                                              \
    STUBV(t_##mn##_c, #mn " %xmm2, %ymm1, %ymm2\n" KEEP2);                                                \
    STUBV(r_##mn, "vextractf128 $1, %ymm1, %xmm5\n" #mn " %xmm2, %xmm1, %xmm4\n" #mn                      \
                  " %xmm2, %xmm5, %xmm5\nvinsertf128 $1, %xmm5, %ymm4, %ymm0\n");                         \
    SHIMMS(SHI, mn)                                                                                       \
    STUBV(t_##mn##_ih, PRE_HI #mn " $3, %ymm13, %ymm15\n" POST_HI);                                       \
    STUBV(t_##mn##_ia, #mn " $3, %ymm1, %ymm1\n" KEEP1)
// Immediates below, at and above the element widths (16, 32, 64 bits).
#define SHIMMS(X, mn) X(mn, 3) X(mn, 16) X(mn, 32) X(mn, 64) X(mn, 255)
#define SHI(mn, n)                                                                                        \
    STUBV(t_##mn##_i##n, #mn " $" #n ", %ymm1, %ymm0\n");                                                 \
    STUBV(r_##mn##_i##n, "vextractf128 $1, %ymm1, %xmm5\n" #mn " $" #n ", %xmm1, %xmm4\n" #mn            \
                         " $" #n ", %xmm5, %xmm5\nvinsertf128 $1, %xmm5, %ymm4, %ymm0\n");

#define BINS(X) X(vpaddb) X(vpaddw) X(vpaddd) X(vpaddq) X(vpsubb) X(vpsubw) X(vpsubd) X(vpsubq)               \
    X(vpcmpeqb) X(vpcmpeqw) X(vpcmpeqd) X(vpcmpeqq) X(vpcmpgtb) X(vpcmpgtw) X(vpcmpgtd) X(vpcmpgtq)          \
    X(vpminub) X(vpminuw) X(vpminud) X(vpminsb) X(vpminsw) X(vpminsd) X(vpmaxub) X(vpmaxuw) X(vpmaxud)       \
    X(vpmaxsb) X(vpmaxsw) X(vpmaxsd) X(vpmullw) X(vpmulld) X(vpmuludq) X(vpshufb)                            \
    X(vpand) X(vpandn) X(vpor) X(vpxor)
#define SHS(X) X(vpsllw) X(vpslld) X(vpsllq) X(vpsrlw) X(vpsrld) X(vpsrlq) X(vpsraw) X(vpsrad)
BINS(BIN)
SHS(SHC)

// One-source forms against a C model: source ymm2/xmm2 or (%rsi).
#define UNX(mn, src)                                                                                      \
    STUBV(t_##mn##_r, #mn " %" src "2, %ymm0\n");                                                         \
    STUBV(t_##mn##_m, #mn " (%rsi), %ymm0\n");                                                            \
    STUBV(t_##mn##_h, PRE_HI #mn " %" src "14, %ymm15\n" POST_HI);                                        \
    STUBV(t_##mn##_hm, PRE_HI #mn " (%rsi), %ymm15\n" POST_HI);                                           \
    STUBV(t_##mn##_a, #mn " %" src "2, %ymm2\n" KEEP2)
#define UN(mn) UNX(mn, "xmm")
#define UNY(mn) UNX(mn, "ymm")
#define WIDENS(X) X(vpmovzxbw) X(vpmovzxbd) X(vpmovzxbq) X(vpmovzxwd) X(vpmovzxwq) X(vpmovzxdq)                \
    X(vpmovsxbw) X(vpmovsxbd) X(vpmovsxbq) X(vpmovsxwd) X(vpmovsxwq) X(vpmovsxdq)
#define BCASTS(X) X(vpbroadcastb) X(vpbroadcastw) X(vpbroadcastd) X(vpbroadcastq)
#define ABSS(X) X(vpabsb) X(vpabsw) X(vpabsd)
WIDENS(UN)
BCASTS(UN)
ABSS(UNY)

typedef void (*fn)(const void *, const void *, void *);
#define DECL_BIN(mn) extern void t_##mn##_r(), t_##mn##_m(), r_##mn(), t_##mn##_h(), t_##mn##_hm(), t_##mn##_a(), \
    t_##mn##_am(), t_##mn##_b();
#define DECL_SHI(mn, n) extern void t_##mn##_i##n(), r_##mn##_i##n();
#define DECL_SH(mn) extern void t_##mn##_r(), t_##mn##_m(), r_##mn(), t_##mn##_h(), t_##mn##_hm(), t_##mn##_a(), \
    t_##mn##_am(), t_##mn##_c(), t_##mn##_ih(), t_##mn##_ia(); SHIMMS(DECL_SHI, mn)
#define DECL_UN(mn) extern void t_##mn##_r(), t_##mn##_m(), t_##mn##_h(), t_##mn##_hm(), t_##mn##_a();
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
static void hex(char *out, unsigned long v) {
    for (int i = 15; i >= 0; i--, v >>= 4) out[i] = "0123456789abcdef"[v & 15];
}
static void on_segv(int sig, siginfo_t *si, void *vc) {
    (void)sig;
    char buf[64] = "  addr ................ rip ................\n";
    hex(buf + 7, (unsigned long)si->si_addr);
    hex(buf + 28, (unsigned long)((ucontext_t *)vc)->uc_mcontext.gregs[REG_RIP]);
    write(2, "SEGV in ", 8);
    write(2, cur, strlen(cur));
    write(2, buf, strlen(buf));
    _exit(3);
}
typedef struct { uint8_t y[6][32]; } out_t;   // result, ymm13..15, ymm1, ymm2

static void check_common(const char *name, const char *form, const uint8_t *in, const out_t *o) {
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
    if (iter % 4 == 3) {   // a count around the element widths, with the high 64 bits set: they do not count
        static const uint8_t cnt[] = {1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 255};
        memset(b, 0, 8);
        b[0] = cnt[(iter / 4) % sizeof cnt];
        memset(b + 8, 0xa5, 8);
    }
    memcpy(in + 128, b, 32);
    ref(in, b, &want);
    t_r(in, b, &o);
    if (memcmp(o.y[0], want.y[0], 32)) bad(name, "reg", o.y[0], want.y[0]);
    check_common(name, "reg", in, &o);
    t_m(in, b, &o);
    if (memcmp(o.y[0], want.y[0], 32)) bad(name, "mem", o.y[0], want.y[0]);
    check_common(name, "mem", in, &o);
}

// A shift by an immediate against its halves reference.
static void immcase(const char *name, fn t, fn ref) {
    cur = name;
    uint8_t in[160] __attribute__((aligned(32))), b[32] = {0};
    out_t o, want;
    for (int i = 0; i < 128; i++) in[i] = (uint8_t)rnd();
    memset(in + 128, 0, 32);
    ref(in, b, &want);
    t(in, b, &o);
    if (memcmp(o.y[0], want.y[0], 32)) bad(name, "imm", o.y[0], want.y[0]);
    check_common(name, "imm", in, &o);
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
    check_common(name, "reg", in, &o);
    t_m(in, b, &o);
    if (memcmp(o.y[0], want, 32)) bad(name, "mem", o.y[0], want);
    check_common(name, "mem", in, &o);
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
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    for (int iter = 0; iter < 60; iter++) {
#define RUN_BIN(mn)                                                                                       \
    lanewise(#mn, (fn)t_##mn##_r, (fn)t_##mn##_m, (fn)r_##mn, iter);                                      \
    lanewise(#mn "/ymm13-15", (fn)t_##mn##_h, (fn)t_##mn##_hm, (fn)r_##mn, iter);                         \
    lanewise(#mn "/dst=src1", (fn)t_##mn##_a, (fn)t_##mn##_am, (fn)r_##mn, iter);                         \
    lanewise(#mn "/dst=src2", (fn)t_##mn##_b, (fn)t_##mn##_b, (fn)r_##mn, iter);
        BINS(RUN_BIN)
#define RUN_IMM(mn, n) immcase(#mn "/imm" #n, (fn)t_##mn##_i##n, (fn)r_##mn##_i##n);
#define RUN_SH(mn)                                                                                        \
    lanewise(#mn, (fn)t_##mn##_r, (fn)t_##mn##_m, (fn)r_##mn, iter);                                      \
    lanewise(#mn "/ymm13-15", (fn)t_##mn##_h, (fn)t_##mn##_hm, (fn)r_##mn, iter);                         \
    lanewise(#mn "/dst=src", (fn)t_##mn##_a, (fn)t_##mn##_am, (fn)r_##mn, iter);                          \
    lanewise(#mn "/dst=count", (fn)t_##mn##_c, (fn)t_##mn##_c, (fn)r_##mn, iter);                         \
    SHIMMS(RUN_IMM, mn)                                                                                   \
    immcase(#mn "/imm3 ymm13-15", (fn)t_##mn##_ih, (fn)r_##mn##_i3);                                      \
    immcase(#mn "/imm3 dst=src", (fn)t_##mn##_ia, (fn)r_##mn##_i3);
        SHS(RUN_SH)
#define ONE(mn, kind, e, d, s, by)                                                                        \
    oneop(#mn, (fn)t_##mn##_r, (fn)t_##mn##_m, kind, e, d, s, by, iter);                                  \
    oneop(#mn "/ymm13-15", (fn)t_##mn##_h, (fn)t_##mn##_hm, kind, e, d, s, by, iter);                     \
    oneop(#mn "/dst=src", (fn)t_##mn##_a, (fn)t_##mn##_a, kind, e, d, s, by, iter);
#define W(mn, e, d, s, by) ONE(mn, 0, e, d, s, by)
        W(vpmovzxbw, 1, 2, 0, 16) W(vpmovzxbd, 1, 4, 0, 8) W(vpmovzxbq, 1, 8, 0, 4)
        W(vpmovzxwd, 2, 4, 0, 16) W(vpmovzxwq, 2, 8, 0, 8) W(vpmovzxdq, 4, 8, 0, 16)
        W(vpmovsxbw, 1, 2, 1, 16) W(vpmovsxbd, 1, 4, 1, 8) W(vpmovsxbq, 1, 8, 1, 4)
        W(vpmovsxwd, 2, 4, 1, 16) W(vpmovsxwq, 2, 8, 1, 8) W(vpmovsxdq, 4, 8, 1, 16)
#define B(mn, e) ONE(mn, 1, e, 0, 0, e)
        B(vpbroadcastb, 1) B(vpbroadcastw, 2) B(vpbroadcastd, 4) B(vpbroadcastq, 8)
#define A(mn, e) ONE(mn, 2, e, 0, 0, 32)
        A(vpabsb, 1) A(vpabsw, 2) A(vpabsd, 4)
        if (failures > 20) break;
    }
    puts(failures ? "FAIL" : "PASS");
    return failures != 0;
}

// approx: forms the audited list admits though vendors may differ (policy kind `approx`, ADR 0008), checked against
// an exact reference over special values. Run under the client on the host; prints PASS, or the first deviations.
//   rcp/rsqrt (ps, ss, VEX): the SDM's bound, |relative error| <= 1.5 * 2^-12.
//   dpps/vdpps: no documented bound. Allowed |out - exact| <= 4 * 2^-23 * sum|a_i*b_i| + 4 * 2^-149: a few single
//     ULPs of the sum of magnitudes (the order of rounding differs by vendor), not of a result that cancels.
//   f2xm1: the SDM documents the domain -1..+1 only. Allowed relative error 2^-60, 8 ULPs of the 64-bit mantissa.
// Ambiguous cases are skipped, never widened: a dpps result or product within 2^-20 of overflow.
#define _GNU_SOURCE
#define _GNU_SOURCE
#include <float.h>
#include <immintrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int fails, checks;
#define FAIL(...) do { if (fails++ < 25) { printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)
#define NEAR_MAX (FLT_MAX * (1 - 0x1p-20))

static float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static const float edge[] = { 0.f, -0.f, 1.f, -1.f, 0.5f, 3.f, -7.25f, 0.1f, 1.0000001f, 16777216.f, 1e-45f, -1e-45f,
    1.1754942e-38f, FLT_MIN, -FLT_MIN, FLT_MAX, -FLT_MAX, 1e20f, 1e-20f, INFINITY, -INFINITY, NAN, -NAN };
#define NEDGE ((int)(sizeof edge / sizeof *edge))

static float rand_float(void) {
    uint64_t r = rnd();
    if (r % 3 == 0) return edge[(r >> 8) % NEDGE];
    return bits_f((uint32_t)(r >> 16));   // any bit pattern: all exponents, denormals, NaNs
}

/* ---- rcp / rsqrt ---------------------------------------------------------------------------------------------- */
#define BOUND (1.5 / 4096)

/* One lane: x in, y out. */
static void check_rcp(const char *nm, float x, float y, int rsq) {
    double ref, ax = fabs((double)x);
    int neg = signbit(x);
    checks++;
    if (isnan(x)) { if (!isnan(y)) FAIL("%s(%a) = %a, want NaN", nm, x, y); return; }
    if (ax < FLT_MIN) {   // zero or denormal: infinity (a denormal is read as zero); a denormal may be approximated
        if (rsq && neg && x != 0) { if (!isnan(y) && !(isinf(y) && signbit(y))) FAIL("%s(%a) = %a", nm, x, y); return; }
        if (isinf(y) && signbit(y) == neg) return;
        if (x == 0 || !isfinite(y)) FAIL("%s(%a) = %a, want signed infinity", nm, x, y);
        return;
    }
    if (isinf(x)) {
        if (rsq && neg) { if (!isnan(y)) FAIL("%s(%a) = %a, want NaN", nm, x, y); }
        else if (y != 0 || signbit(y) != (rsq ? 0 : neg)) FAIL("%s(%a) = %a, want signed zero", nm, x, y);
        return;
    }
    if (rsq && neg) { if (!isnan(y)) FAIL("%s(%a) = %a, want NaN", nm, x, y); return; }
    ref = rsq ? 1.0 / sqrt(ax) : 1.0 / (double)x;
    if (fabs(ref) < FLT_MIN * (1 + 1.0 / 1024) && y == 0 && signbit(y) == (rsq ? 0 : neg)) return;  // tiny results flush to 0
    if (fabs(ref) > FLT_MAX) { if (!isinf(y)) FAIL("%s(%a) = %a, want infinity", nm, x, y); return; }
    if (!(fabs((double)y - ref) <= BOUND * fabs(ref))) FAIL("%s(%a) = %a, exact %a", nm, x, y, (float)ref);
}

#define AVX __attribute__((target("avx")))
/* One wrapper per form, so each compiles to that instruction (legacy SSE or VEX) and not to a neighbour. */
#define PS(NAME, TGT, LOAD, INSN, STORE, N) TGT static void NAME(const float *a, float *o) { STORE(o, INSN(LOAD(a))); } enum { NAME##_N = N };
#define SS(NAME, TGT, INSN) TGT static void NAME(const float *a, float *o) { \
    _mm_storeu_ps(o, INSN(_mm_loadu_ps(a))); \
    for (int i = 1; i < 4; i++) if (o[i] != a[i] && a[i] == a[i]) FAIL(#NAME " upper lane %d changed", i); } enum { NAME##_N = 1 };
PS(f_rcpps, , _mm_loadu_ps, _mm_rcp_ps, _mm_storeu_ps, 4) SS(f_rcpss, , _mm_rcp_ss)
PS(f_rsqrtps, , _mm_loadu_ps, _mm_rsqrt_ps, _mm_storeu_ps, 4) SS(f_rsqrtss, , _mm_rsqrt_ss)
PS(f_vrcpps, AVX, _mm256_loadu_ps, _mm256_rcp_ps, _mm256_storeu_ps, 8) SS(f_vrcpss, AVX, _mm_rcp_ss)
PS(f_vrsqrtps, AVX, _mm256_loadu_ps, _mm256_rsqrt_ps, _mm256_storeu_ps, 8) SS(f_vrsqrtss, AVX, _mm_rsqrt_ss)

#define RUN(NAME, FN, RSQ) static void NAME(void) { \
    for (int n = 0; n < 6000; n++) { \
        float in[8], out[8]; \
        for (int i = 0; i < 8; i++) in[i] = (n < NEDGE * 8) ? edge[(n + i * 7) % NEDGE] : rand_float(); \
        FN(in, out); \
        for (int i = 0; i < FN##_N; i++) check_rcp(#FN, in[i], out[i], RSQ); \
    } }
RUN(t_rcpps, f_rcpps, 0) RUN(t_rcpss, f_rcpss, 0) RUN(t_rsqrtps, f_rsqrtps, 1) RUN(t_rsqrtss, f_rsqrtss, 1)
RUN(t_vrcpps, f_vrcpps, 0) RUN(t_vrcpss, f_vrcpss, 0) RUN(t_vrsqrtps, f_vrsqrtps, 1) RUN(t_vrsqrtss, f_vrsqrtss, 1)

/* ---- dpps / vdpps: every immediate, by bytes (an immediate cannot be a variable) ------------------------------- */
static void check_dp(const char *nm, int imm, const float *a, const float *b, const float *out, int lanes) {
    for (int l = 0; l < lanes; l += 4) {
        double sum = 0, mag = 0, maxp = 0;
        for (int i = 0; i < 4; i++) if (imm >> (4 + i) & 1) {
            double p = (double)a[l + i] * (double)b[l + i];
            sum += p; mag += fabs(p);
            if (fabs(p) > maxp) maxp = fabs(p);
        }
        for (int j = 0; j < 4; j++) {
            float y = out[l + j];
            checks++;
            if (!(imm >> j & 1)) { if (y != 0 || signbit(y)) FAIL("%s imm=%02x lane %d = %a, want +0", nm, imm, l + j, y); continue; }
            if (isnan(sum)) { if (!isnan(y)) FAIL("%s imm=%02x lane %d = %a, want NaN", nm, imm, l + j, y); continue; }
            if (isinf(sum)) { if (y != (float)sum) FAIL("%s imm=%02x lane %d = %a, want %a", nm, imm, l + j, y, (float)sum); continue; }
            if (maxp > NEAR_MAX || fabs(sum) > NEAR_MAX) continue;   // ambiguous: a single-precision overflow
            if (!(fabs((double)y - sum) <= 4 * 0x1p-23 * mag + 4 * 0x1p-149))
                FAIL("%s imm=%02x lane %d = %a, exact %a (sum|p| %a)", nm, imm, l + j, y, sum, mag);
        }
    }
}

static unsigned char *stubs;   // [form][imm] 16-byte stubs: the instruction, ret
static void build_stubs(void) {
    static const unsigned char enc[3][5] = {
        { 0x66, 0x0f, 0x3a, 0x40, 0xc1 },   /* dpps xmm0, xmm1, imm */
        { 0xc4, 0xe3, 0x71, 0x40, 0xc2 },   /* vdpps xmm0, xmm1, xmm2, imm */
        { 0xc4, 0xe3, 0x75, 0x40, 0xc2 } }; /* vdpps ymm0, ymm1, ymm2, imm */
    stubs = mmap(0, 3 * 256 * 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stubs == MAP_FAILED) { puts("mmap failed"); _exit(2); }
    for (int f = 0; f < 3; f++) for (int i = 0; i < 256; i++) {
        unsigned char *s = stubs + (f * 256 + i) * 16;
        memcpy(s, enc[f], 5); s[5] = (unsigned char)i; s[6] = 0xc3;
    }
}
static void run_dp(int f, int imm, const float *a, const float *b, float *out) {
    void *code = stubs + (f * 256 + imm) * 16;
    const float *s1 = f ? a : b;   // dpps xmm0, xmm1: a in xmm0, b in xmm1. vdpps xmm0, xmm1, xmm2: a in xmm1, b in xmm2
    __asm__ volatile("vmovups (%4), %%ymm0\n\tvmovups (%1), %%ymm1\n\tvmovups (%2), %%ymm2\n\t"
                     "lea -128(%%rsp), %%rsp\n\tcall *%3\n\tlea 128(%%rsp), %%rsp\n\tvmovups %%ymm0, (%0)\n\tvzeroupper"
                     : : "r"(out), "r"(s1), "r"(b), "r"(code), "r"(a) : "xmm0", "xmm1", "xmm2", "memory", "cc");
}
static void t_dpps(void) {
    static const char *nm[3] = { "dpps", "vdpps-x", "vdpps-y" };
    for (int f = 0; f < 3; f++) for (int imm = 0; imm < 256; imm++) for (int n = 0; n < 48; n++) {
        float a[8], b[8], out[8];
        for (int i = 0; i < 8; i++) {
            a[i] = n < 8 ? edge[(n * 3 + i + imm) % NEDGE] : rand_float();
            b[i] = n < 8 ? edge[(n * 5 + i * 2 + imm) % NEDGE] : rand_float();
            if (n >= 16 && n < 40) {   // moderate exponents, so sums are not dominated by one huge or tiny term
                a[i] = bits_f(((uint32_t)rnd() & 0x807fffffu) | (uint32_t)(0x7f - 8 + (rnd() & 15)) << 23);
                b[i] = bits_f(((uint32_t)rnd() & 0x807fffffu) | (uint32_t)(0x7f - 8 + (rnd() & 15)) << 23);
            }
        }
        run_dp(f, imm, a, b, out);
        check_dp(nm[f], imm, a, b, out, f == 2 ? 8 : 4);
    }
}

/* ---- f2xm1 ---------------------------------------------------------------------------------------------------- */
static long double f2xm1(long double x) {
    long double r;
    __asm__ volatile("fldt %1\n\tf2xm1\n\tfstpt %0" : "=m"(r) : "m"(x) : "st", "memory");
    return r;
}
static void t_f2xm1(void) {
    static long double xs[8000];
    static const long double e[] = { 0.0L, -0.0L, 1.0L, -1.0L, 0.5L, -0.5L, 0x1p-16445L, -0x1p-16445L, 0x1p-16382L,
        -0x1p-16382L, 1e-300L, -1e-300L, 0x1p-64L, 0x1p-30L, -0x1p-30L, 0.999999999999L, -0.999999999999L, 0.25L };
    int n = 0;
    for (unsigned i = 0; i < sizeof e / sizeof *e; i++) xs[n++] = e[i];
    for (; n < 8000; n++) {
        long double x = ldexpl((long double)(rnd() | 1ull << 63), -64 - (int)(rnd() % 64));   // 2^-64 .. 1
        xs[n] = (rnd() & 1) ? -x : x;
    }
    for (int i = 0; i < n; i++) {
        long double x = xs[i], y = f2xm1(x), ref = expm1l(x * M_LN2l);
        checks++;
        if (x == 0) { if (y != 0 || signbit(y) != signbit(x)) FAIL("f2xm1(%Lg) = %Lg, want signed zero", x, y); continue; }
        if (!(fabsl(y - ref) <= 0x1p-60L * fabsl(ref))) FAIL("f2xm1(%La) = %La, exact %La", x, y, ref);
    }
}

int main(void) {
    build_stubs();
    t_rcpps(); t_rcpss(); t_rsqrtps(); t_rsqrtss(); t_vrcpps(); t_vrcpss(); t_vrsqrtps(); t_vrsqrtss();
    t_dpps(); t_f2xm1();
    printf("%d checks, %d deviations\n", checks, fails);
    puts(fails ? "FAIL" : "PASS");
    return fails != 0;
}

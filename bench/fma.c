// fma: FMA cost under the client. "micro" rows time one instruction form (64 copies a loop, rotating eight
// destinations, so dependency chains are short); "kernel" rows are compiled code (-O3 -march=x86-64-v3)
// and print a checksum of the result bits, which must equal native: FMA is correctly rounded.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

#define X8(s) s s s s s s s s
#define R8(op, w) \
    op " %%" w "8, %%" w "9, %%" w "0\n" op " %%" w "8, %%" w "9, %%" w "1\n" \
    op " %%" w "8, %%" w "9, %%" w "2\n" op " %%" w "8, %%" w "9, %%" w "3\n" \
    op " %%" w "8, %%" w "9, %%" w "4\n" op " %%" w "8, %%" w "9, %%" w "5\n" \
    op " %%" w "8, %%" w "9, %%" w "6\n" op " %%" w "8, %%" w "9, %%" w "7\n"

// Registers 0-7 are destinations (start 1.0), 8 and 9 sources near 1.0, so values stay finite and normal.
#define LOOP(name, body)                                                                            \
    static void name(uint64_t n) {                                                                  \
        static const double one[4] = {1, 1, 1, 1}, s8[4] = {1.0000001, 1.0000001, 1.0000001, 1.0000001}, \
                            s9[4] = {0.9999999, 0.9999999, 0.9999999, 0.9999999};                     \
        __asm__ volatile("vmovupd %1, %%ymm0\n vmovapd %%ymm0, %%ymm1\n vmovapd %%ymm0, %%ymm2\n"    \
                         "vmovapd %%ymm0, %%ymm3\n vmovapd %%ymm0, %%ymm4\n vmovapd %%ymm0, %%ymm5\n" \
                         "vmovapd %%ymm0, %%ymm6\n vmovapd %%ymm0, %%ymm7\n"                         \
                         "vmovupd %2, %%ymm8\n vmovupd %3, %%ymm9\n"                                 \
                         "1:\n" X8(body) "dec %0\n jnz 1b\n vzeroupper\n"                             \
                         : "+r"(n)                                                                  \
                         : "m"(one), "m"(s8), "m"(s9)                                               \
                         : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",    \
                           "xmm9", "cc", "memory");                                                 \
    }

LOOP(m_vaddpd_y, R8("vaddpd", "ymm"))
LOOP(m_fma_ss, R8("vfmadd231ss", "xmm"))
LOOP(m_fma_sd, R8("vfmadd231sd", "xmm"))
LOOP(m_fma_ps_x, R8("vfmadd231ps", "xmm"))
LOOP(m_fma_pd_x, R8("vfmadd231pd", "xmm"))
LOOP(m_fma_ps_y, R8("vfmadd231ps", "ymm"))
LOOP(m_fma_pd_y, R8("vfmadd231pd", "ymm"))
LOOP(m_vpermd_y, R8("vpermd", "ymm"))   // a frame helper with no FP: the call's own cost

static double ns_per_op(void (*f)(uint64_t)) {
    uint64_t n = 16;
    for (;;) {
        double t0 = now();
        f(n);
        if (now() - t0 > 0.05) break;
        n *= 2;
    }
    double best = 1e18;
    for (int r = 0; r < 5; r++) {
        double t0 = now();
        f(n);
        double dt = now() - t0;
        if (dt < best) best = dt;
    }
    return best * 1e9 / ((double)n * 64);
}

enum { N = 160 };
static double A[N * N], B[N * N], C[N * N];
static float Af[N * N], Bf[N * N], Cf[N * N];

static void dgemm(void) {
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++) {
            double a = A[i * N + k];
            for (int j = 0; j < N; j++) C[i * N + j] += a * B[k * N + j];
        }
}
static void sgemm(void) {
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++) {
            float a = Af[i * N + k];
            for (int j = 0; j < N; j++) Cf[i * N + j] += a * Bf[k * N + j];
        }
}
// Scalar: Horner with fma(), one element at a time.
__attribute__((optimize("no-tree-vectorize"))) static double horner(int n) {
    double s = 0;
    for (int i = 0; i < n; i++) {
        double x = (double)i / n, p = 0.5;
        for (int k = 0; k < 12; k++) p = fma(p, x, 1.0 / (k + 2));
        s += p;
    }
    return s;
}

static uint64_t hash(const void *p, size_t n) {
    const unsigned char *b = p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

int main(int argc, char **argv) {
    int scale = argc > 1 ? atoi(argv[1]) : 1;
    if (argc <= 2 || strcmp(argv[2], "kernels") != 0) {
        static const struct { const char *name; void (*f)(uint64_t); } rows[] = {
            {"vaddpd_ymm", m_vaddpd_y}, {"vpermd_ymm", m_vpermd_y}, {"vfmadd231ss", m_fma_ss},
            {"vfmadd231sd", m_fma_sd}, {"vfmadd231ps_xmm", m_fma_ps_x}, {"vfmadd231pd_xmm", m_fma_pd_x},
            {"vfmadd231ps_ymm", m_fma_ps_y}, {"vfmadd231pd_ymm", m_fma_pd_y}};
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
            printf("micro %-16s %9.2f ns/op\n", rows[i].name, ns_per_op(rows[i].f));
    }
    if (argc > 2 && strcmp(argv[2], "micro") == 0) return 0;
    for (int i = 0; i < N * N; i++) {
        A[i] = (double)(i % 97) / 97 + 0.1;
        B[i] = (double)(i % 89) / 89 - 0.3;
        Af[i] = (float)A[i];
        Bf[i] = (float)B[i];
    }
    double t0 = now();
    for (int r = 0; r < 4 * scale; r++) dgemm();
    double t1 = now();
    for (int r = 0; r < 4 * scale; r++) sgemm();
    double t2 = now();
    double h = 0;
    for (int r = 0; r < scale; r++) h += horner(200000);
    double t3 = now();
    printf("kernel dgemm  %8.3f s  sum %016llx\n", t1 - t0, (unsigned long long)hash(C, sizeof C));
    printf("kernel sgemm  %8.3f s  sum %016llx\n", t2 - t1, (unsigned long long)hash(Cf, sizeof Cf));
    printf("kernel horner %8.3f s  sum %016llx\n", t3 - t2, (unsigned long long)hash(&h, sizeof h));
    return 0;
}

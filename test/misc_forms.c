// misc_forms: forms the avxemu manifest marks refused (register-source vbroadcastss/sd, ymm
// vmovntdqa), and what the frame path does to MXCSR: nothing, except FMA, which ORs its flags in.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int bad;
static void expect(int ok, const char *what) {
    if (!ok) { printf("FAIL %s\n", what); bad = 1; }
}

static uint32_t mxcsr_after(const char *kind, uint32_t init) {
    static float a[8], b[8], c[8];
    uint32_t out, def = 0x1F80;
    for (int i = 0; i < 8; i++) { a[i] = b[i] = 1.0000001f; c[i] = 0.0f; }
    if (!strcmp(kind, "fma"))
        __asm__ volatile("ldmxcsr %1\nvmovups %2, %%ymm1\nvmovups %2, %%ymm2\nvmovups %3, %%ymm3\n"
                         "vfmadd231ps %%ymm1, %%ymm2, %%ymm3\nstmxcsr %0\nldmxcsr %4\n"
                         : "=m"(out) : "m"(init), "m"(a), "m"(c), "m"(def) : "xmm1", "xmm2", "xmm3", "memory");
    else if (!strcmp(kind, "fma-exact"))
        __asm__ volatile("ldmxcsr %1\nvmovups %2, %%ymm1\nvmovups %2, %%ymm2\nvmovups %3, %%ymm3\n"
                         "vfmadd231ss %%xmm1, %%xmm2, %%xmm3\nstmxcsr %0\nldmxcsr %4\n"
                         : "=m"(out) : "m"(init), "m"(c), "m"(c), "m"(def) : "xmm1", "xmm2", "xmm3", "memory");
    else
        __asm__ volatile("ldmxcsr %1\nvmovups %2, %%ymm1\nvpmaxsd %2, %%ymm1, %%ymm2\nvpermd %2, %%ymm1, %%ymm3\n"
                         "stmxcsr %0\nldmxcsr %3\n"
                         : "=m"(out) : "m"(init), "m"(a), "m"(def) : "xmm1", "xmm2", "xmm3", "memory");
    return out;
}

int main(void) {
    static uint8_t src[64] __attribute__((aligned(32))), dst[64] __attribute__((aligned(32)));
    for (int i = 0; i < 64; i++) src[i] = (uint8_t)(i * 7 + 3);

    // vbroadcastss/sd with a register source: the lowest element everywhere, xmm destination zero-extended.
    uint8_t pre[32];
    memset(pre, 0xEE, sizeof pre);
    __asm__ volatile("vmovdqu %1, %%ymm1\nvmovdqu %2, %%ymm2\nvbroadcastss %%xmm1, %%ymm2\nvmovdqu %%ymm2, %0"
                     : "=m"(dst) : "m"(src), "m"(pre) : "xmm1", "xmm2", "memory");
    int ok = 1;
    for (int i = 0; i < 32; i++) ok &= dst[i] == src[i % 4];
    expect(ok, "vbroadcastss ymm, xmm");
    __asm__ volatile("vmovdqu %1, %%ymm1\nvmovdqu %2, %%ymm2\nvbroadcastss %%xmm1, %%xmm2\nvmovdqu %%ymm2, %0"
                     : "=m"(dst) : "m"(src), "m"(pre) : "xmm1", "xmm2", "memory");
    ok = 1;
    for (int i = 0; i < 16; i++) ok &= dst[i] == src[i % 4];
    for (int i = 16; i < 32; i++) ok &= dst[i] == 0;
    expect(ok, "vbroadcastss xmm, xmm");
    __asm__ volatile("vmovdqu %1, %%ymm1\nvmovdqu %2, %%ymm2\nvbroadcastsd %%xmm1, %%ymm2\nvmovdqu %%ymm2, %0"
                     : "=m"(dst) : "m"(src), "m"(pre) : "xmm1", "xmm2", "memory");
    ok = 1;
    for (int i = 0; i < 32; i++) ok &= dst[i] == src[i % 8];
    expect(ok, "vbroadcastsd ymm, xmm");

    // ymm non-temporal load.
    __asm__ volatile("vmovntdqa %1, %%ymm3\nvmovdqu %%ymm3, %0" : "=m"(dst) : "m"(src) : "xmm3", "memory");
    expect(!memcmp(dst, src, 32), "vmovntdqa ymm");

    // MXCSR: untouched by the other frame forms, flags ORed in by FMA, existing flags kept.
    expect(mxcsr_after("other", 0x1F80 | 0x2009) == (0x1F80 | 0x2009), "frame form leaves MXCSR alone");
    expect(mxcsr_after("fma", 0x1F80 | 0x2009) == (0x1F80 | 0x2009 | 0x20), "fma ORs the inexact flag");
    expect(mxcsr_after("fma", 0x1F80) == (0x1F80 | 0x20), "fma inexact from clean flags");
    expect(mxcsr_after("fma-exact", 0x1F80 | 0x9) == (0x1F80 | 0x9), "exact fma keeps flags");
    puts(bad ? "FAIL" : "PASS");
    return bad;
}

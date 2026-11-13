// misc_forms: what the frame path does to MXCSR: nothing, except FMA, which ORs its flags in. The semantic
// suite cannot see this: it randomises MXCSR only for FP forms.
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
    // MXCSR: untouched by the other frame forms, flags ORed in by FMA, existing flags kept.
    expect(mxcsr_after("other", 0x1F80 | 0x2009) == (0x1F80 | 0x2009), "frame form leaves MXCSR alone");
    expect(mxcsr_after("fma", 0x1F80 | 0x2009) == (0x1F80 | 0x2009 | 0x20), "fma ORs the inexact flag");
    expect(mxcsr_after("fma", 0x1F80) == (0x1F80 | 0x20), "fma inexact from clean flags");
    expect(mxcsr_after("fma-exact", 0x1F80 | 0x9) == (0x1F80 | 0x9), "exact fma keeps flags");
    puts(bad ? "FAIL" : "PASS");
    return bad;
}

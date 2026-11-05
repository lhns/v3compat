// hadd_fma_forms: vphadd*/vphsub* and vfmaddsub/vfmsubadd, which the segment-operand tests need to run: xmm and
// ymm, register and memory source. A refused form exits 86; the two sources must give the same result.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static float a[8] __attribute__((aligned(32))) = {1.5f, -2.25f, 3.f, 4.5f, -5.f, 6.25f, 7.f, -8.5f};
static float b[8] __attribute__((aligned(32))) = {0.5f, 2.f, -1.f, 8.f, 3.5f, -4.f, 5.f, 6.f};
static float c[8] __attribute__((aligned(32))) = {10.f, 20.f, 30.f, 40.f, 50.f, 60.f, 70.f, 80.f};
static int bad;

#define CASE(mn, R, N)                                                                              \
    do {                                                                                              \
        uint8_t r1[32], r2[32];                                                                       \
        __asm__ volatile("vmovdqu (%2), %%" R "1\n vmovdqu (%3), %%" R "2\n vmovdqu (%4), %%" R "3\n" \
                         "vmovdqa %%" R "3, %%" R "4\n" #mn " %%" R "2, %%" R "1, %%" R "3\n"        \
                         #mn " (%3), %%" R "1, %%" R "4\n vmovdqu %%" R "3, (%0)\n vmovdqu %%" R "4, (%1)" \
                         : : "r"(r1), "r"(r2), "r"(a), "r"(b), "r"(c) : "memory", "xmm1", "xmm2", "xmm3", "xmm4"); \
        if (memcmp(r1, r2, N)) { printf("FAIL %s %s: register and memory sources differ\n", #mn, R); bad = 1; } \
    } while (0)
#define BOTH(mn) CASE(mn, "ymm", 32); CASE(mn, "xmm", 16)

int main(void) {
    BOTH(vphaddw); BOTH(vphaddsw); BOTH(vphsubd); BOTH(vphsubsw); BOTH(vphsubw);
    BOTH(vfmaddsub132ps); BOTH(vfmaddsub213ps); BOTH(vfmaddsub231ps);
    BOTH(vfmaddsub132pd); BOTH(vfmaddsub213pd); BOTH(vfmaddsub231pd);
    BOTH(vfmsubadd132ps); BOTH(vfmsubadd213ps); BOTH(vfmsubadd231ps);
    BOTH(vfmsubadd132pd); BOTH(vfmsubadd213pd); BOTH(vfmsubadd231pd);
    puts(bad ? "FAIL" : "PASS");
    return bad;
}

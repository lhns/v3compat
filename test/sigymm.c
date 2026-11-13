/* sigymm MODE SECONDS: an AVX2 integer loop keeps state in ymm8-ymm11 across signals whose handler clobbers every
 * ymm with AVX2 code, as the JVM's safepoint path does. Each block of iterations is checked against a scalar model.
 * MODE: h = a load in the loop faults on a page another thread protects (a safepoint poll); the handler unprotects it;
 *       s = the same, but the handler sends the thread to a stub that saves the ymm halves, runs AVX2 code and
 *           restores them before going back (as HotSpot's poll stub does);
 *       a = a 1 ms timer signal instead of the fault.
 * The handler also writes a counter into ymm13 in the frame: after the block ymm13 must hold it, upper half too.
 * Prints "ok" and exits 0 when every block and every frame was right. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

enum { N = 200000 };
static char *page;
static volatile int stop;
static volatile long sigs, frame_bad, bv_clear, edit_bad, in_sigs;
static volatile int in_block;
static void *bad_pc[5]; static uint32_t bad_lo[5], bad_hi[5];
static uint32_t K[8] __attribute__((aligned(32))) = {3, 5, 7, 9, 11, 13, 17, 19};
static uint32_t D0[8] __attribute__((aligned(32))) = {1, 2, 3, 4, 1000, 2000, 3000, 4000};
static char mode;
void *ret_pc;

/* Every ymm, with AVX2 instructions. */
__attribute__((noinline)) void clobber(void) {
    __asm__ volatile("vpcmpeqd %%ymm0, %%ymm0, %%ymm0\n vpaddd %%ymm0, %%ymm0, %%ymm1\n vpaddd %%ymm1, %%ymm0, %%ymm2\n"
                     "vpaddd %%ymm2, %%ymm0, %%ymm3\n vpaddd %%ymm3, %%ymm0, %%ymm4\n vpaddd %%ymm4, %%ymm0, %%ymm5\n"
                     "vpaddd %%ymm5, %%ymm0, %%ymm6\n vpaddd %%ymm6, %%ymm0, %%ymm7\n vpaddd %%ymm7, %%ymm0, %%ymm8\n"
                     "vpaddd %%ymm8, %%ymm0, %%ymm9\n vpaddd %%ymm9, %%ymm0, %%ymm10\n vpaddd %%ymm10, %%ymm0, %%ymm11\n"
                     "vpaddd %%ymm11, %%ymm0, %%ymm12\n vpaddd %%ymm12, %%ymm0, %%ymm13\n vpaddd %%ymm13, %%ymm0, %%ymm14\n"
                     "vpaddd %%ymm14, %%ymm0, %%ymm15\n"
                     ::: "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
                     "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

void stub_work(void) {
    clobber();
    mprotect(page, 4096, PROT_READ);
}

/* HotSpot-like: save GPRs, flags, xmm lows and ymm highs; run AVX2 code; restore; back to the poll. */
__asm__(".text\n .globl poll_stub\n poll_stub:\n"
        "pushfq\n push %rax\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n push %r8\n push %r9\n push %r10\n push %r11\n"
        "push %rbp\n mov %rsp, %rbp\n and $-32, %rsp\n sub $512, %rsp\n"
        ".irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n vmovdqu %xmm\\n, \\n*16(%rsp)\n vextractf128 $1, %ymm\\n, 256+\\n*16(%rsp)\n .endr\n"
        "call stub_work\n"
        ".irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n vmovdqu \\n*16(%rsp), %xmm\\n\n vinsertf128 $1, 256+\\n*16(%rsp), %ymm\\n, %ymm\\n\n .endr\n"
        "mov %rbp, %rsp\n pop %rbp\n"
        "pop %r11\n pop %r10\n pop %r9\n pop %r8\n pop %rdi\n pop %rsi\n pop %rdx\n pop %rcx\n pop %rax\n popfq\n"
        "jmp *ret_pc(%rip)\n");
extern char poll_stub[];

/* The frame's ymm n: low half in the fxsave area, high half in the XSAVE YMM_Hi128 component. */
static void frame_ymm(ucontext_t *uc, int n, uint32_t *lo, uint32_t *hi, int *bv) {
    unsigned char *fp = (unsigned char *)uc->uc_mcontext.fpregs;
    memcpy(lo, fp + 160 + n * 16, 16);
    memcpy(hi, fp + 576 + n * 16, 16);
    *bv = (int)((*(uint64_t *)(fp + 512) >> 2) & 1);
}

static void handler(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    long n = ++sigs;
    uint32_t lo[4], hi[4];
    int bv;
    frame_ymm(uc, 8, lo, hi, &bv);
    if (!bv) bv_clear++;
    if (in_block) in_sigs++;
    if (in_block && (memcmp(lo, K, 16) || (bv && memcmp(hi, K + 4, 16)))) {
        if (frame_bad < 5) { bad_pc[frame_bad] = (void *)uc->uc_mcontext.gregs[REG_RIP]; bad_lo[frame_bad] = lo[0]; bad_hi[frame_bad] = hi[0]; }
        frame_bad++;
    }
    /* ymm13 in the frame := n in every lane; YMM bit set so the high half is restored. */
    unsigned char *fp = (unsigned char *)uc->uc_mcontext.fpregs;
    uint32_t v[4] = {(uint32_t)n, (uint32_t)n, (uint32_t)n, (uint32_t)n};
    memcpy(fp + 160 + 13 * 16, v, 16);
    memcpy(fp + 576 + 13 * 16, v, 16);
    *(uint64_t *)(fp + 512) |= 4;
    if (mode == 's') {
        ret_pc = (void *)uc->uc_mcontext.gregs[REG_RIP];
        uc->uc_mcontext.gregs[REG_RIP] = (greg_t)poll_stub;
        return;
    }
    clobber();
    if (mode == 'h') mprotect(page, 4096, PROT_READ);
}

static void *toggler(void *a) {
    (void)a;
    while (!stop) {
        struct timespec t = {0, 300000};
        nanosleep(&t, 0);
        mprotect(page, 4096, PROT_NONE);
    }
    return 0;
}

/* One block: acc += d * K; d += K, per 32-bit lane, with a load from the poll page each iteration. ymm13 out. */
static void block(uint32_t *acc, uint32_t *d, uint32_t *y13) {
    __asm__ volatile("vmovdqu (%[k]), %%ymm8\n vmovdqu (%[acc]), %%ymm9\n vmovdqu (%[d]), %%ymm10\n"
                     "mov %[n], %%ecx\n"
                     "1: vpmulld %%ymm8, %%ymm10, %%ymm11\n vpaddd %%ymm11, %%ymm9, %%ymm9\n vpaddd %%ymm8, %%ymm10, %%ymm10\n"
                     "test %%eax, (%[p])\n dec %%ecx\n jnz 1b\n"
                     "vmovdqu %%ymm9, (%[acc])\n vmovdqu %%ymm10, (%[d])\n vmovdqu %%ymm13, (%[y])\n"
                     :: [k] "r"(K), [acc] "r"(acc), [d] "r"(d), [y] "r"(y13), [n] "i"(N), [p] "r"(page)
                     : "rcx", "rax", "memory", "cc", "xmm8", "xmm9", "xmm10", "xmm11", "xmm13");
}

int main(int argc, char **argv) {
    mode = argc > 1 ? argv[1][0] : 'h';
    int secs = argc > 2 ? atoi(argv[2]) : 3;
    page = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(mode == 'a' ? SIGALRM : SIGSEGV, &sa, 0);
    pthread_t t;
    if (mode == 'a') {
        struct itimerval it = {{0, 1000}, {0, 1000}};
        setitimer(ITIMER_REAL, &it, 0);
    } else {
        pthread_create(&t, 0, toggler, 0);
    }
    uint32_t acc[8] = {0}, d[8], y13[8], macc[8] = {0}, md[8];
    memcpy(d, D0, sizeof d);
    memcpy(md, D0, sizeof md);
    long blocks = 0, bad = 0;
    time_t end = time(0) + secs;
    while (time(0) < end) {
        long before = in_sigs;
        in_block = 1;
        block(acc, d, y13);
        in_block = 0;
        for (int i = 0; i < N; i++)
            for (int j = 0; j < 8; j++) { macc[j] += md[j] * K[j]; md[j] += K[j]; }
        if (memcmp(acc, macc, sizeof acc) || memcmp(d, md, sizeof d)) {
            if (bad < 5) fprintf(stderr, "block %ld wrong: lane0 %u vs %u, lane7 %u vs %u\n", blocks, acc[0], macc[0], acc[7], macc[7]);
            bad++;
            memcpy(acc, macc, sizeof acc);
            memcpy(d, md, sizeof d);
        }
        if (in_sigs != before) {
            /* The last signal in the block left its counter in ymm13 (all eight lanes). */
            int ok = 1;
            for (int j = 1; j < 8; j++) ok &= y13[j] == y13[0];
            if (!ok || y13[0] == 0 || y13[0] > (uint32_t)sigs) edit_bad++;
        }
        blocks++;
    }
    stop = 1;
    if (mode == 'a') {
        struct itimerval it = {{0, 0}, {0, 0}};
        setitimer(ITIMER_REAL, &it, 0);
    } else {
        pthread_join(t, 0);
    }
    int fail = bad || frame_bad || edit_bad;
    for (int i = 0; i < 5 && i < frame_bad; i++) {
        fprintf(stderr, "frame_bad pc=%p ymm8.lane0=%u lane4=%u (want %u %u) ", bad_pc[i], bad_lo[i], bad_hi[i], K[0], K[4]);
        FILE *m = fopen("/proc/self/maps", "r");
        char line[512];
        while (m && fgets(line, sizeof line, m)) {
            unsigned long lo2, hi2;
            if (sscanf(line, "%lx-%lx", &lo2, &hi2) == 2 && lo2 <= (unsigned long)bad_pc[i] && (unsigned long)bad_pc[i] < hi2)
                fputs(line, stderr);
        }
        if (m) fclose(m);
    }
    printf("%s mode=%c blocks=%ld bad=%ld signals=%ld in_block=%ld frame_bad=%ld bv_clear=%ld edit_bad=%ld\n", fail ? "FAIL" : "ok",
           mode, blocks, bad, sigs, in_sigs, frame_bad, bv_clear, edit_bad);
    return fail;
}

/* regstress THREADS SECONDS [jit] [sig] [xlsig] [idle] [badsig]: threads run hot shared code with every register at a
 * known value and check them all constantly, while other threads make DR flush, deliver signals or sit blocked. A wrong
 * register prints which and exits 77; badsig makes the handler flip a flag, which must be caught. sig sends SIGUSR1,
 * which DR holds to the end of the fragment; xlsig sends SIGXCPU, which it delivers at once from wherever the thread
 * is, so a signal lands inside an expansion and the handler sees the state DR rebuilt there. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

__thread uint64_t exp_[16] __attribute__((aligned(32)));
__thread uint64_t yexp[4 * 14] __attribute__((aligned(32)));
__thread uint64_t got[16];
__thread uint64_t ygot[4 * 14] __attribute__((aligned(32)));
__thread uint64_t failk, iters, svrax, gotf, expf;
__thread uint64_t rexp[4 * 5] __attribute__((aligned(32)));
__thread int isworker;
__thread uint8_t sbuf[64], dbuf[64];
__thread uint8_t fstack[65536] __attribute__((aligned(16)));
volatile char stopf;
void chk(void);
void *chkp = (void *)chk;
void setf1(void); void setf0(void);
void *setf1p = (void *)setf1, *setf0p = (void *)setf0;
void run_blocks(void);
extern char blocks_start[], blocks_end[];
static const char *names[] = {"rax","rbx","rcx","rdx","rsi","rdi","rbp","r8","r9","r10","r11","r12","r13","r14","r15","rsp"};

void report(uint64_t ret) {
    char b[1024]; int n;
    long k = (long)failk;
    n = snprintf(b, sizeof b, "@@FAIL iter=%lu kind=%ld(%s) block_off=%ld\n", (unsigned long)iters, k,
                 k < 16 ? names[k] : k == 16 ? "CF" : k == 17 ? "flags" : k >= 100 ? "ymm" : "?", (long)(ret - (uint64_t)blocks_start));
    write(2, b, n);
    if (k == 17) { n = snprintf(b, sizeof b, "@@FLAGS exp=%03lx got=%03lx\n", (unsigned long)expf, (unsigned long)gotf); write(2, b, n); }
    if (k < 16) { n = snprintf(b, sizeof b, "@@ exp=%016lx got=%016lx\n", (unsigned long)exp_[k], (unsigned long)got[k]); write(2, b, n); }
    if (k >= 100) { int y = k - 100; n = snprintf(b, sizeof b, "@@ ymm%d exp=%016lx %016lx %016lx %016lx got=%016lx %016lx %016lx %016lx\n", y,
        (unsigned long)yexp[y*4],(unsigned long)yexp[y*4+1],(unsigned long)yexp[y*4+2],(unsigned long)yexp[y*4+3],
        (unsigned long)ygot[y*4],(unsigned long)ygot[y*4+1],(unsigned long)ygot[y*4+2],(unsigned long)ygot[y*4+3]); write(2, b, n); }
    for (int i = 0; i < 16; i++) { n = snprintf(b, sizeof b, "@@ %s=%016lx%s\n", names[i], (unsigned long)got[i], got[i]==exp_[i]?"":" WRONG"); write(2, b, n); }
    _exit(77);
}

#define CMPR(i, r) "cmp %fs:exp_@tpoff+8*" #i ", %" #r "\n jne fail" #i "\n"
/* AVX1 only, so the check itself runs natively under the client. */
#define CMPY(i) "vmovdqu %fs:yexp@tpoff+32*" #i ", %ymm15\n vxorps %ymm" #i ", %ymm15, %ymm15\n vptest %ymm15, %ymm15\n jnz faily" #i "\n"
/* An emulated instruction; its address goes to section exppc, where the handler checks every GPR. */
#define EXP(insn) "13: " insn "\n .pushsection exppc,\"a\"\n .quad 13b\n .popsection\n"
/* An expansion's result in ymm14 against the value worked out in plain C. */
#define CHK14(k) "vmovdqu %fs:rexp@tpoff+32*" #k ", %ymm15\n vxorps %ymm14, %ymm15, %ymm15\n vptest %ymm15, %ymm15\n jnz failx\n"
#define FAILR(i) "fail" #i ":\n movq $" #i ", %fs:failk@tpoff\n jmp fail\n"
#define FAILY(i) "faily" #i ":\n movq $100+" #i ", %fs:failk@tpoff\n jmp fail\n"
__asm__(
".text\n"
".globl chk\n chk:\n jnc failcf\n"
CMPR(0,rax) CMPR(1,rbx) CMPR(2,rcx) CMPR(3,rdx) CMPR(4,rsi) CMPR(5,rdi) CMPR(6,rbp) CMPR(7,r8) CMPR(8,r9)
CMPR(9,r10) CMPR(10,r11) CMPR(11,r12) CMPR(12,r13) CMPR(13,r14) CMPR(14,r15) CMPR(15,rsp)
CMPY(0) CMPY(1) CMPY(2) CMPY(3) CMPY(4) CMPY(5) CMPY(6) CMPY(7) CMPY(8) CMPY(9) CMPY(10) CMPY(11) CMPY(12) CMPY(13)
" ret\n"
"failx:\n movq $18, %fs:failk@tpoff\n jmp fail\n"
"setf1:\n pushq $0x8d7\n popfq\n ret\n"
"setf0:\n pushq $0x002\n popfq\n ret\n"
"failflags:\n movq $17, %fs:failk@tpoff\n mov %rax,%fs:gotf@tpoff\n mov %fs:svrax@tpoff,%rax\n jmp fail\n"
".macro CHECKF pat\n mov %rax,%fs:svrax@tpoff\n pushfq\n pop %rax\n and $0x8d5,%eax\n movq $\\pat,%fs:expf@tpoff\n cmp $\\pat,%eax\n jne failflags\n mov %fs:svrax@tpoff,%rax\n.endm\n"
"failcf:\n movq $16, %fs:failk@tpoff\n jmp fail\n"
FAILR(0) FAILR(1) FAILR(2) FAILR(3) FAILR(4) FAILR(5) FAILR(6) FAILR(7) FAILR(8) FAILR(9) FAILR(10) FAILR(11) FAILR(12) FAILR(13) FAILR(14) FAILR(15)
FAILY(0) FAILY(1) FAILY(2) FAILY(3) FAILY(4) FAILY(5) FAILY(6) FAILY(7) FAILY(8) FAILY(9) FAILY(10) FAILY(11) FAILY(12) FAILY(13)
"fail:\n"
" mov %rax,%fs:got@tpoff+0\n mov %rbx,%fs:got@tpoff+8\n mov %rcx,%fs:got@tpoff+16\n mov %rdx,%fs:got@tpoff+24\n"
" mov %rsi,%fs:got@tpoff+32\n mov %rdi,%fs:got@tpoff+40\n mov %rbp,%fs:got@tpoff+48\n mov %r8,%fs:got@tpoff+56\n"
" mov %r9,%fs:got@tpoff+64\n mov %r10,%fs:got@tpoff+72\n mov %r11,%fs:got@tpoff+80\n mov %r12,%fs:got@tpoff+88\n"
" mov %r13,%fs:got@tpoff+96\n mov %r14,%fs:got@tpoff+104\n mov %r15,%fs:got@tpoff+112\n mov %rsp,%fs:got@tpoff+120\n"
" vmovdqu %ymm0,%fs:ygot@tpoff\n vmovdqu %ymm1,%fs:ygot@tpoff+32\n vmovdqu %ymm2,%fs:ygot@tpoff+64\n vmovdqu %ymm3,%fs:ygot@tpoff+96\n"
" vmovdqu %ymm4,%fs:ygot@tpoff+128\n vmovdqu %ymm5,%fs:ygot@tpoff+160\n vmovdqu %ymm6,%fs:ygot@tpoff+192\n vmovdqu %ymm7,%fs:ygot@tpoff+224\n"
" vmovdqu %ymm8,%fs:ygot@tpoff+256\n vmovdqu %ymm9,%fs:ygot@tpoff+288\n vmovdqu %ymm10,%fs:ygot@tpoff+320\n vmovdqu %ymm11,%fs:ygot@tpoff+352\n"
" vmovdqu %ymm12,%fs:ygot@tpoff+384\n vmovdqu %ymm13,%fs:ygot@tpoff+416\n"
" mov (%rsp),%rdi\n mov %fs:0,%rax\n lea fstack@tpoff+65000(%rax),%rsp\n and $-16,%rsp\n call report\n"
".macro LOADG\n"
" mov %fs:exp_@tpoff+0,%rax\n mov %fs:exp_@tpoff+8,%rbx\n mov %fs:exp_@tpoff+16,%rcx\n mov %fs:exp_@tpoff+24,%rdx\n"
" mov %fs:exp_@tpoff+32,%rsi\n mov %fs:exp_@tpoff+40,%rdi\n mov %fs:exp_@tpoff+48,%rbp\n mov %fs:exp_@tpoff+56,%r8\n"
" mov %fs:exp_@tpoff+64,%r9\n mov %fs:exp_@tpoff+72,%r10\n mov %fs:exp_@tpoff+80,%r11\n mov %fs:exp_@tpoff+88,%r12\n"
" mov %fs:exp_@tpoff+96,%r13\n mov %fs:exp_@tpoff+104,%r14\n mov %fs:exp_@tpoff+112,%r15\n"
".endm\n"
".globl run_blocks\n run_blocks:\n push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
" lea -8(%rsp),%rax\n mov %rax,%fs:exp_@tpoff+120\n"
" vmovdqu %fs:yexp@tpoff,%ymm0\n vmovdqu %fs:yexp@tpoff+32,%ymm1\n vmovdqu %fs:yexp@tpoff+64,%ymm2\n vmovdqu %fs:yexp@tpoff+96,%ymm3\n"
" vmovdqu %fs:yexp@tpoff+128,%ymm4\n vmovdqu %fs:yexp@tpoff+160,%ymm5\n vmovdqu %fs:yexp@tpoff+192,%ymm6\n vmovdqu %fs:yexp@tpoff+224,%ymm7\n"
" vmovdqu %fs:yexp@tpoff+256,%ymm8\n vmovdqu %fs:yexp@tpoff+288,%ymm9\n vmovdqu %fs:yexp@tpoff+320,%ymm10\n vmovdqu %fs:yexp@tpoff+352,%ymm11\n"
" vmovdqu %fs:yexp@tpoff+384,%ymm12\n vmovdqu %fs:yexp@tpoff+416,%ymm13\n"
" LOADG\n"
".globl blocks_start\nblocks_start:\n"
".rept 1500\n"
/* indirect jump through memory, call, direct */
" stc\n call chk\n jmp *1f(%rip)\n1: .quad 2f\n2:\n"
/* indirect call */
" stc\n call *chkp(%rip)\n jmp 3f\n nop\n3:\n"
/* rep movs */
" mov %fs:0,%rsi\n lea sbuf@tpoff(%rsi),%rsi\n mov %fs:0,%rdi\n lea dbuf@tpoff(%rdi),%rdi\n mov $64,%ecx\n rep movsb\n"
" mov %fs:exp_@tpoff+16,%rcx\n mov %fs:exp_@tpoff+32,%rsi\n mov %fs:exp_@tpoff+40,%rdi\n stc\n call chk\n jmp 4f\n4:\n"
/* loop, jrcxz, cmpxchg, flags live across a call */
" mov $3,%rcx\n5: loop 5b\n mov %fs:exp_@tpoff+16,%rcx\n jrcxz 7f\n"
" mov %rax,%rdx\n lock cmpxchg %rdx,%fs:iters@tpoff\n mov %fs:exp_@tpoff+0,%rax\n mov %fs:exp_@tpoff+24,%rdx\n"
" push %rax\n pop %rax\n stc\n call chk\n7:\n"
/* inline forms, then a helper form, each result checked */
EXP("vpaddq %ymm0, %ymm1, %ymm14") CHK14(0)
EXP("vpmuludq %ymm2, %ymm3, %ymm14") CHK14(1)
EXP("vpsllq $3, %ymm4, %ymm14") CHK14(2)
EXP("vpsrlq $1, %ymm5, %ymm14") CHK14(3)
EXP("vpaddd %ymm6, %ymm7, %ymm14") CHK14(4)
/* all arithmetic flags live across ret, indirect jump and indirect call */
" call setf1\n CHECKF 0x8d5\n call setf0\n CHECKF 0x000\n"
" pushq $0x8d7\n popfq\n jmp *6f(%rip)\n6: .quad 9f\n9:\n CHECKF 0x8d5\n"
" pushq $0x002\n popfq\n jmp *11f(%rip)\n11: .quad 12f\n12:\n CHECKF 0x000\n"
" call *setf1p(%rip)\n CHECKF 0x8d5\n call *setf0p(%rip)\n CHECKF 0x000\n"
".endr\n"
".globl blocks_end\nblocks_end:\n"
" incq %fs:iters@tpoff\n cmpb $0,stopf(%rip)\n jne 8f\n jmp blocks_start\n"
"8: vzeroupper\n pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n ret\n");

static uint64_t total;
static void *worker(void *arg) {
    uint64_t base = 0x1111000000000000ull * (uint64_t)((long)arg + 1);
    for (int i = 0; i < 15; i++) exp_[i] = base + 0x0101010101ull * (i + 1);
    for (int i = 0; i < 56; i++) yexp[i] = base ^ (0x0202020202ull * (i + 1));
    for (int l = 0; l < 4; l++) {
        rexp[0 * 4 + l] = yexp[1 * 4 + l] + yexp[0 * 4 + l];
        rexp[1 * 4 + l] = (yexp[3 * 4 + l] & 0xffffffffu) * (yexp[2 * 4 + l] & 0xffffffffu);
        rexp[2 * 4 + l] = yexp[4 * 4 + l] << 3;
        rexp[3 * 4 + l] = yexp[5 * 4 + l] >> 1;
        uint32_t a = (uint32_t)yexp[6 * 4 + l], b = (uint32_t)(yexp[6 * 4 + l] >> 32), c = (uint32_t)yexp[7 * 4 + l], d = (uint32_t)(yexp[7 * 4 + l] >> 32);
        rexp[4 * 4 + l] = (uint64_t)(uint32_t)(a + c) | ((uint64_t)(uint32_t)(b + d) << 32);
    }
    isworker = 1;
    run_blocks();
    __atomic_add_fetch(&total, iters, __ATOMIC_RELAXED);
    return NULL;
}
static uint64_t jits;
static void *jitter(void *arg) {
    (void)arg;
    for (uint32_t v = 1; !stopf; v++) {
        unsigned char *p = mmap(0, 8192, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) _exit(78);
        for (int k = 0; k < 40; k++) {             /* unique code, each hot enough to become a trace */
            unsigned char *q = p + k * 16;
            q[0] = 0xb8; memcpy(q + 1, &v, 4); q[5] = 0xc3;
            uint32_t (*f)(void) = (uint32_t (*)(void))q;
            for (int r = 0; r < 60; r++) if (f() != v) { write(2, "@@JITWRONG\n", 11); _exit(79); }
        }
        munmap(p, 8192);
        jits++;
    }
    return NULL;
}
#include <ucontext.h>
static int badsig;
static uint64_t nsig, nsigblk, nsigexp;
extern const uint64_t __start_exppc[], __stop_exppc[];
/* The emulated instructions' addresses, in order. */
static int at_exp(uint64_t pc) {
    long lo = 0, hi = __stop_exppc - __start_exppc - 1;
    while (lo <= hi) {
        long m = (lo + hi) / 2;
        if (__start_exppc[m] == pc) return 1;
        if (__start_exppc[m] < pc) lo = m + 1; else hi = m - 1;
    }
    return 0;
}
static void onsig(int s, siginfo_t *si, void *u) {
    (void)s; (void)si;
    ucontext_t *uc = (ucontext_t *)u;
    greg_t *g = uc->uc_mcontext.gregs;
    __atomic_add_fetch(&nsig, 1, __ATOMIC_RELAXED);
    if (badsig) g[REG_EFL] ^= 0x40;
    uint64_t pc = (uint64_t)g[REG_RIP];
    if (isworker && pc >= (uint64_t)blocks_start && pc < (uint64_t)blocks_end) {
        /* Registers the blocks never change must be right in what the handler sees, wherever it interrupted. */
        static const int gi[] = {REG_RBX, REG_RBP, REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
        static const int ei[] = {1, 6, 7, 8, 9, 10, 11, 12, 13, 14};
        char b[200]; int n;
        __atomic_add_fetch(&nsigblk, 1, __ATOMIC_RELAXED);
        for (int i = 0; i < 10; i++)
            if ((uint64_t)g[gi[i]] != exp_[ei[i]]) {
                n = snprintf(b, sizeof b, "@@FAIL handler gpr %s exp=%016lx got=%016lx pc_off=%ld\n", names[ei[i]], (unsigned long)exp_[ei[i]], (unsigned long)g[gi[i]], (long)(pc - (uint64_t)blocks_start));
                write(2, b, n); _exit(80);
            }
        if (at_exp(pc)) {
            /* Interrupted in or right before an expansion: every GPR holds its loaded value there. */
            static const int ga[] = {REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RSI, REG_RDI, REG_RBP, REG_R8, REG_R9,
                                     REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15, REG_RSP};
            __atomic_add_fetch(&nsigexp, 1, __ATOMIC_RELAXED);
            for (int i = 0; i < 16; i++)
                if ((uint64_t)g[ga[i]] != exp_[i] + (i == 15 ? 8 : 0)) {   /* exp_ holds rsp as chk sees it */
                    n = snprintf(b, sizeof b, "@@FAIL handler at an expansion: %s exp=%016lx got=%016lx pc_off=%ld\n", names[i], (unsigned long)exp_[i], (unsigned long)g[ga[i]], (long)(pc - (uint64_t)blocks_start));
                    write(2, b, n); _exit(82);
                }
        }
        char *fp = (char *)uc->uc_mcontext.fpregs;
        for (int y = 0; y < 14; y++) {
            uint64_t lo[2], hi[2];
            memcpy(lo, fp + 160 + 16 * y, 16); memcpy(hi, fp + 576 + 16 * y, 16);
            if (lo[0] != yexp[y*4] || lo[1] != yexp[y*4+1] || hi[0] != yexp[y*4+2] || hi[1] != yexp[y*4+3]) {
                n = snprintf(b, sizeof b, "@@FAIL handler ymm%d got=%016lx %016lx %016lx %016lx pc_off=%ld\n", y, (unsigned long)lo[0], (unsigned long)lo[1], (unsigned long)hi[0], (unsigned long)hi[1], (long)(pc - (uint64_t)blocks_start));
                write(2, b, n); _exit(81);
            }
        }
    }
    /* Run an inline expansion in the handler and leave every vector register changed: sigreturn must restore them. */
    __asm__ volatile("vpaddq %%ymm0, %%ymm1, %%ymm14\n vzeroall\n" : : : "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15");
}
static void *idler(void *arg) { (void)arg; for (;;) pause(); return NULL; }
int main(int c, char **v) {
    int n = atoi(v[1]), secs = atoi(v[2]), jit = 0, sg = 0, idle = 0, signo = SIGUSR1;
    for (int i = 3; i < c; i++) { if (!strcmp(v[i], "jit")) jit = 1; if (!strcmp(v[i], "sig")) sg = 1; if (!strcmp(v[i], "xlsig")) { sg = 1; signo = SIGXCPU; } if (!strcmp(v[i], "idle")) idle = 1; if (!strcmp(v[i], "badsig")) { badsig = 1; sg = 1; } }
    pthread_t t[64], j, d;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = onsig; sa.sa_flags = SA_SIGINFO | SA_RESTART; sigaction(SIGUSR1, &sa, 0); sigaction(SIGXCPU, &sa, 0);
    for (long i = 0; i < n; i++) pthread_create(&t[i], 0, worker, (void *)i);
    if (jit) pthread_create(&j, 0, jitter, 0);
    if (idle) pthread_create(&d, 0, idler, 0);
    for (int ms = 0; ms < secs * 100; ms++) {
        if (!sg) { usleep(10000); continue; }
        for (int k = 0; k < 20; k++) { usleep(400); for (int i = 0; i < n; i++) pthread_kill(t[i], signo); }
    }
    stopf = 1;
    for (int i = 0; i < n; i++) pthread_join(t[i], 0);
    if (jit) pthread_join(j, 0);
    printf("ok sig=%lu inblocks=%lu atexp=%lu iters=%lu jits=%lu\n", (unsigned long)nsig, (unsigned long)nsigblk, (unsigned long)nsigexp, (unsigned long)total, (unsigned long)jits);
    return 0;
}

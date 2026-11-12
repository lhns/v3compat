// fault_state: an expansion that faults must leave exactly the state of the application instruction.
// Each case loads every ymm and GPR with known values and executes one instruction that faults (a
// PROT_NONE operand, or an unmasked FP exception). The signal handler checks rip, the fault address
// or code, all ymm (upper halves from the xsave area), all GPRs and the arithmetic flags, then skips
// the instruction and the program carries on.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

#define ARITH 0x8D5ul
#define PAGE 4096

uint8_t ymm_init[16][32] __attribute__((aligned(32)));
uint64_t gpr_init[16];   // hardware order: rax rcx rdx rbx rsp rbp rsi rdi r8..r15
uint64_t flags_init = ARITH | 2;
uint32_t mxcsr_init = 0x1F80;
uint64_t saved_rsp;
uint8_t bad_page[PAGE] __attribute__((aligned(PAGE)));
uint8_t split[2 * PAGE] __attribute__((aligned(PAGE)));   // page 0 writable, page 1 PROT_NONE
uint8_t good[64] __attribute__((aligned(64)));

#define Y(i) "vmovdqu ymm_init+" #i "*32(%rip), %ymm" #i "\n"
#define G(k, r) "mov gpr_init+" #k "*8(%rip), %" #r "\n"
#define STUB(name, insn)                                                                         \
    __asm__(".text\n.globl " #name "\n.type " #name ",@function\n" #name ":\n"                  \
            "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\n"                 \
            "mov %rsp, saved_rsp(%rip)\n"                                                        \
            Y(0) Y(1) Y(2) Y(3) Y(4) Y(5) Y(6) Y(7) Y(8) Y(9) Y(10) Y(11) Y(12) Y(13) Y(14) Y(15) \
            G(0, rax) G(1, rcx) G(2, rdx) G(3, rbx) G(5, rbp) G(6, rsi) G(7, rdi)                \
            G(8, r8) G(9, r9) G(10, r10) G(11, r11) G(12, r12) G(13, r13) G(14, r14) G(15, r15)  \
            "ldmxcsr mxcsr_init(%rip)\n"                                                         \
            "push flags_init(%rip)\npopfq\n"                                                     \
            ".globl ins_" #name "\nins_" #name ":\n" insn "\n"                                   \
            ".globl after_" #name "\nafter_" #name ":\n"                                         \
            "mov saved_rsp(%rip), %rsp\n"                                                        \
            "pop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n")

// Memory operand cases: r15 points at the PROT_NONE page, r14 at the end of the writable split page.
STUB(c_inline, "vpaddq (%r15), %ymm1, %ymm2");
STUB(c_inline_shift, "vpsrlq (%r15), %ymm3, %ymm4");
STUB(c_frame, "vpmaxsd (%r15), %ymm5, %ymm6");
STUB(c_frame_rip, "vpunpcklbw bad_page(%rip), %ymm9, %ymm10");
STUB(c_frame_fma, "vfmadd231ps (%r15), %ymm1, %ymm2");
STUB(c_frame_bcast, "vpbroadcastb (%r15), %ymm11");
STUB(c_frame_cvt, "vpmovzxbw (%r15), %ymm12");
STUB(c_subst_maskld, "vpmaskmovd (%r15), %ymm1, %ymm2");
STUB(c_subst_maskst, "vpmaskmovd %ymm2, %ymm1, (%r15)");
STUB(c_subst_bc128, "vbroadcasti128 (%r15), %ymm3");
STUB(c_subst_bcd, "vpbroadcastd (%r15), %ymm4");
STUB(c_subst_ntld, "vmovntdqa (%r15), %ymm5");
STUB(c_scalar, "pdep (%r15), %rbx, %rsi");
STUB(c_scalar_lz, "lzcnt (%r15), %rax");
STUB(c_movbe_store, "movbe %rax, (%r15)");
STUB(c_movbe_split, "movbe %rdx, (%r14)");
// BMI forms with a memory source: the destination, flags and every register stay as they were.
STUB(c_rorx, "rorx $5, (%r15), %rbx");
STUB(c_rorx_rip, "rorx $5, bad_page(%rip), %rbx");
STUB(c_andn, "andn (%r15), %rcx, %rbx");
STUB(c_andn_dst, "andn (%r15), %rcx, %r15");
STUB(c_mulx, "mulx (%r15), %rcx, %rbx");
STUB(c_mulx_lo, "mulx (%r15), %r15, %rbx");
STUB(c_mulx32_rip, "mulx bad_page(%rip), %ecx, %ebx");
STUB(c_movbe_ld, "movbe (%r15), %rbx");
STUB(c_movbe_ld16, "movbe (%r15), %bx");
STUB(c_movbe_ld32, "movbe (%r15), %r12d");
STUB(c_movbe_ld_dst, "movbe (%r15), %r15");
STUB(c_movbe_ld_rip, "movbe bad_page(%rip), %rbx");
STUB(c_movbe_ld_split, "movbe (%r14), %rbx");
// Inline scalar and vector forms added with the speed work.
STUB(c_shlx, "shlx %rcx, (%r15), %rbx");
STUB(c_sarx_cnt_rax, "sarx %rax, (%r15), %rcx");
STUB(c_bzhi, "bzhi %rcx, (%r15), %rbx");
STUB(c_bzhi_dst, "bzhi %rcx, (%r15), %r15");
STUB(c_tzcnt, "tzcnt (%r15), %rbx");
STUB(c_lzcnt, "lzcnt (%r15), %rbx");
STUB(c_lzcnt32_rip, "lzcnt bad_page(%rip), %ebx");
STUB(c_tzcnt_dst, "tzcnt (%r15), %r15");
STUB(c_v_cmp, "vpcmpeqd (%r15), %ymm1, %ymm2");
STUB(c_v_shift_d, "vpslld (%r15), %ymm3, %ymm4");
STUB(c_v_logic, "vpand (%r15), %ymm1, %ymm2");
STUB(c_v_abs, "vpabsd (%r15), %ymm3");
STUB(c_v_widen, "vpmovzxbq (%r15), %ymm4");
STUB(c_v_widen_sx, "vpmovsxdq (%r15), %ymm5");
STUB(c_v_bcast_b, "vpbroadcastb (%r15), %ymm6");
STUB(c_v_bcast_w, "vpbroadcastw (%r15), %ymm7");
STUB(c_v_bcast_d, "vpbroadcastd (%r15), %ymm8");
STUB(c_v_permd, "vpermd (%r15), %ymm1, %ymm9");
STUB(c_v_permps_idx, "vpermps (%r15), %ymm10, %ymm10");
STUB(c_v_permq, "vpermq $0x4e, (%r15), %ymm11");
STUB(c_v_shufd, "vpshufd $0x1b, (%r15), %ymm12");
STUB(c_v_bcast_x, "vpbroadcastb (%r15), %xmm13");
// Unmasked FP exceptions: ymm1 and ymm2 are set up per case.
STUB(c_fma_ps, "vfmadd231ps %ymm1, %ymm2, %ymm3");
STUB(c_fma_ss, "vfmadd231ss %xmm1, %xmm2, %xmm3");
STUB(c_fma_sd_mem, "vfnmsub132sd (%r13), %xmm2, %xmm3");

typedef struct {
    const char *name;
    void (*run)(void);
    const char *ins, *after;
    int sig, code;          // expected signal; for SIGFPE the si_code
    int kind;               // 0: bad_page, 1: split store (page 0 untouched), 2: FP
    void (*setup)(void);
} tcase;

extern char ins_c_inline[], after_c_inline[], ins_c_inline_shift[], after_c_inline_shift[], ins_c_frame[],
    after_c_frame[], ins_c_frame_rip[], after_c_frame_rip[], ins_c_frame_fma[], after_c_frame_fma[],
    ins_c_frame_bcast[], after_c_frame_bcast[], ins_c_frame_cvt[], after_c_frame_cvt[],
    ins_c_subst_maskld[], after_c_subst_maskld[], ins_c_subst_maskst[], after_c_subst_maskst[],
    ins_c_subst_bc128[], after_c_subst_bc128[], ins_c_subst_bcd[], after_c_subst_bcd[],
    ins_c_subst_ntld[], after_c_subst_ntld[], ins_c_scalar[], after_c_scalar[], ins_c_scalar_lz[],
    after_c_scalar_lz[], ins_c_movbe_store[], after_c_movbe_store[], ins_c_movbe_split[],
    after_c_movbe_split[], ins_c_fma_ps[], after_c_fma_ps[], ins_c_fma_ss[], after_c_fma_ss[],
    ins_c_fma_sd_mem[], after_c_fma_sd_mem[], ins_c_rorx[], after_c_rorx[], ins_c_rorx_rip[],
    after_c_rorx_rip[], ins_c_andn[], after_c_andn[], ins_c_andn_dst[], after_c_andn_dst[], ins_c_mulx[],
    after_c_mulx[], ins_c_mulx_lo[], after_c_mulx_lo[], ins_c_mulx32_rip[], after_c_mulx32_rip[],
    ins_c_movbe_ld[], after_c_movbe_ld[], ins_c_movbe_ld16[], after_c_movbe_ld16[], ins_c_movbe_ld32[],
    after_c_movbe_ld32[], ins_c_movbe_ld_dst[], after_c_movbe_ld_dst[], ins_c_movbe_ld_rip[],
    after_c_movbe_ld_rip[], ins_c_movbe_ld_split[], after_c_movbe_ld_split[];
extern void c_inline(void), c_inline_shift(void), c_frame(void), c_frame_rip(void), c_frame_fma(void),
    c_frame_bcast(void), c_frame_cvt(void), c_subst_maskld(void), c_subst_maskst(void), c_subst_bc128(void),
    c_subst_bcd(void), c_subst_ntld(void), c_scalar(void), c_scalar_lz(void), c_movbe_store(void),
    c_movbe_split(void), c_fma_ps(void), c_fma_ss(void), c_fma_sd_mem(void), c_rorx(void), c_rorx_rip(void),
    c_andn(void), c_andn_dst(void), c_mulx(void), c_mulx_lo(void), c_mulx32_rip(void), c_movbe_ld(void),
    c_movbe_ld16(void), c_movbe_ld32(void), c_movbe_ld_dst(void), c_movbe_ld_rip(void),
    c_movbe_ld_split(void);
extern char ins_c_shlx[], after_c_shlx[], ins_c_sarx_cnt_rax[], after_c_sarx_cnt_rax[], ins_c_bzhi[], after_c_bzhi[], ins_c_bzhi_dst[], after_c_bzhi_dst[], ins_c_tzcnt[], after_c_tzcnt[], ins_c_lzcnt[], after_c_lzcnt[], ins_c_lzcnt32_rip[], after_c_lzcnt32_rip[], ins_c_tzcnt_dst[], after_c_tzcnt_dst[], ins_c_v_cmp[], after_c_v_cmp[], ins_c_v_shift_d[], after_c_v_shift_d[], ins_c_v_logic[], after_c_v_logic[], ins_c_v_abs[], after_c_v_abs[], ins_c_v_widen[], after_c_v_widen[], ins_c_v_widen_sx[], after_c_v_widen_sx[], ins_c_v_bcast_b[], after_c_v_bcast_b[], ins_c_v_bcast_w[], after_c_v_bcast_w[], ins_c_v_bcast_d[], after_c_v_bcast_d[], ins_c_v_permd[], after_c_v_permd[], ins_c_v_permps_idx[], after_c_v_permps_idx[], ins_c_v_permq[], after_c_v_permq[], ins_c_v_shufd[], after_c_v_shufd[], ins_c_v_bcast_x[], after_c_v_bcast_x[];
extern void c_shlx(void), c_sarx_cnt_rax(void), c_bzhi(void), c_bzhi_dst(void), c_tzcnt(void), c_lzcnt(void), c_lzcnt32_rip(void), c_tzcnt_dst(void), c_v_cmp(void), c_v_shift_d(void), c_v_logic(void), c_v_abs(void), c_v_widen(void), c_v_widen_sx(void), c_v_bcast_b(void), c_v_bcast_w(void), c_v_bcast_d(void), c_v_permd(void), c_v_permps_idx(void), c_v_permq(void), c_v_shufd(void), c_v_bcast_x(void);

static void put_f32(int reg, float v) {
    for (int i = 0; i < 8; i++) memcpy(&ymm_init[reg][4 * i], &v, 4);
}
static void put_f64(int reg, double v) {
    for (int i = 0; i < 4; i++) memcpy(&ymm_init[reg][8 * i], &v, 8);
}
static void inf_times_zero(void) { put_f32(1, 0.0f); put_f32(2, __builtin_inff()); mxcsr_init = 0x1F80 & ~0x80u; }
static void overflow(void) { put_f32(1, 1e30f); put_f32(2, 1e30f); put_f32(3, 0.0f); mxcsr_init = 0x1F80 & ~0x400u; }
static void underflow(void) { put_f32(1, 1e-30f); put_f32(2, 1e-30f); put_f32(3, 0.0f); mxcsr_init = 0x1F80 & ~0x800u; }
static void inexact(void) {
    put_f32(1, 1.0000001f); put_f32(2, 1.0000001f); put_f32(3, 0.0f);
    mxcsr_init = 0x1F80 & ~0x1000u;
}
static void sd_invalid(void) {
    put_f64(2, 1.0);
    put_f64(3, 0.0);
    uint64_t inf = 0x7ff0000000000000ull;
    memcpy(good, &inf, 8);
    mxcsr_init = 0x1F80 & ~0x80u;
}

static const tcase cases[] = {
    {"inline", c_inline, ins_c_inline, after_c_inline, SIGSEGV, 0, 0, 0},
    {"inline-shift", c_inline_shift, ins_c_inline_shift, after_c_inline_shift, SIGSEGV, 0, 0, 0},
    {"frame", c_frame, ins_c_frame, after_c_frame, SIGSEGV, 0, 0, 0},
    {"frame-rip", c_frame_rip, ins_c_frame_rip, after_c_frame_rip, SIGSEGV, 0, 0, 0},
    {"frame-fma", c_frame_fma, ins_c_frame_fma, after_c_frame_fma, SIGSEGV, 0, 0, 0},
    {"frame-broadcast", c_frame_bcast, ins_c_frame_bcast, after_c_frame_bcast, SIGSEGV, 0, 0, 0},
    {"frame-pmovzx", c_frame_cvt, ins_c_frame_cvt, after_c_frame_cvt, SIGSEGV, 0, 0, 0},
    {"subst-maskload", c_subst_maskld, ins_c_subst_maskld, after_c_subst_maskld, SIGSEGV, 0, 0, 0},
    {"subst-maskstore", c_subst_maskst, ins_c_subst_maskst, after_c_subst_maskst, SIGSEGV, 0, 0, 0},
    {"subst-bcast128", c_subst_bc128, ins_c_subst_bc128, after_c_subst_bc128, SIGSEGV, 0, 0, 0},
    {"subst-bcastd", c_subst_bcd, ins_c_subst_bcd, after_c_subst_bcd, SIGSEGV, 0, 0, 0},
    {"subst-ntload", c_subst_ntld, ins_c_subst_ntld, after_c_subst_ntld, SIGSEGV, 0, 0, 0},
    {"scalar", c_scalar, ins_c_scalar, after_c_scalar, SIGSEGV, 0, 0, 0},
    {"scalar-lzcnt", c_scalar_lz, ins_c_scalar_lz, after_c_scalar_lz, SIGSEGV, 0, 0, 0},
    {"movbe-store", c_movbe_store, ins_c_movbe_store, after_c_movbe_store, SIGSEGV, 0, 0, 0},
    {"movbe-split", c_movbe_split, ins_c_movbe_split, after_c_movbe_split, SIGSEGV, 1, 1, 0},
    {"rorx", c_rorx, ins_c_rorx, after_c_rorx, SIGSEGV, 0, 0, 0},
    {"rorx-rip", c_rorx_rip, ins_c_rorx_rip, after_c_rorx_rip, SIGSEGV, 0, 0, 0},
    {"andn", c_andn, ins_c_andn, after_c_andn, SIGSEGV, 0, 0, 0},
    {"andn-dst", c_andn_dst, ins_c_andn_dst, after_c_andn_dst, SIGSEGV, 0, 0, 0},
    {"mulx", c_mulx, ins_c_mulx, after_c_mulx, SIGSEGV, 0, 0, 0},
    {"mulx-lo", c_mulx_lo, ins_c_mulx_lo, after_c_mulx_lo, SIGSEGV, 0, 0, 0},
    {"mulx32-rip", c_mulx32_rip, ins_c_mulx32_rip, after_c_mulx32_rip, SIGSEGV, 0, 0, 0},
    {"movbe-ld", c_movbe_ld, ins_c_movbe_ld, after_c_movbe_ld, SIGSEGV, 0, 0, 0},
    {"movbe-ld16", c_movbe_ld16, ins_c_movbe_ld16, after_c_movbe_ld16, SIGSEGV, 0, 0, 0},
    {"movbe-ld32", c_movbe_ld32, ins_c_movbe_ld32, after_c_movbe_ld32, SIGSEGV, 0, 0, 0},
    {"movbe-ld-dst", c_movbe_ld_dst, ins_c_movbe_ld_dst, after_c_movbe_ld_dst, SIGSEGV, 0, 0, 0},
    {"movbe-ld-rip", c_movbe_ld_rip, ins_c_movbe_ld_rip, after_c_movbe_ld_rip, SIGSEGV, 0, 0, 0},
    {"movbe-ld-split", c_movbe_ld_split, ins_c_movbe_ld_split, after_c_movbe_ld_split, SIGSEGV, 0, 1, 0},
    {"shlx", c_shlx, ins_c_shlx, after_c_shlx, SIGSEGV, 0, 0, 0},
    {"sarx-cnt-rax", c_sarx_cnt_rax, ins_c_sarx_cnt_rax, after_c_sarx_cnt_rax, SIGSEGV, 0, 0, 0},
    {"bzhi", c_bzhi, ins_c_bzhi, after_c_bzhi, SIGSEGV, 0, 0, 0},
    {"bzhi-dst", c_bzhi_dst, ins_c_bzhi_dst, after_c_bzhi_dst, SIGSEGV, 0, 0, 0},
    {"tzcnt", c_tzcnt, ins_c_tzcnt, after_c_tzcnt, SIGSEGV, 0, 0, 0},
    {"lzcnt", c_lzcnt, ins_c_lzcnt, after_c_lzcnt, SIGSEGV, 0, 0, 0},
    {"lzcnt32-rip", c_lzcnt32_rip, ins_c_lzcnt32_rip, after_c_lzcnt32_rip, SIGSEGV, 0, 0, 0},
    {"tzcnt-dst", c_tzcnt_dst, ins_c_tzcnt_dst, after_c_tzcnt_dst, SIGSEGV, 0, 0, 0},
    {"v-cmp", c_v_cmp, ins_c_v_cmp, after_c_v_cmp, SIGSEGV, 0, 0, 0},
    {"v-shift-d", c_v_shift_d, ins_c_v_shift_d, after_c_v_shift_d, SIGSEGV, 0, 0, 0},
    {"v-logic", c_v_logic, ins_c_v_logic, after_c_v_logic, SIGSEGV, 0, 0, 0},
    {"v-abs", c_v_abs, ins_c_v_abs, after_c_v_abs, SIGSEGV, 0, 0, 0},
    {"v-widen", c_v_widen, ins_c_v_widen, after_c_v_widen, SIGSEGV, 0, 0, 0},
    {"v-widen-sx", c_v_widen_sx, ins_c_v_widen_sx, after_c_v_widen_sx, SIGSEGV, 0, 0, 0},
    {"v-bcast-b", c_v_bcast_b, ins_c_v_bcast_b, after_c_v_bcast_b, SIGSEGV, 0, 0, 0},
    {"v-bcast-w", c_v_bcast_w, ins_c_v_bcast_w, after_c_v_bcast_w, SIGSEGV, 0, 0, 0},
    {"v-bcast-d", c_v_bcast_d, ins_c_v_bcast_d, after_c_v_bcast_d, SIGSEGV, 0, 0, 0},
    {"v-permd", c_v_permd, ins_c_v_permd, after_c_v_permd, SIGSEGV, 0, 0, 0},
    {"v-permps-dst=index", c_v_permps_idx, ins_c_v_permps_idx, after_c_v_permps_idx, SIGSEGV, 0, 0, 0},
    {"v-permq", c_v_permq, ins_c_v_permq, after_c_v_permq, SIGSEGV, 0, 0, 0},
    {"v-shufd", c_v_shufd, ins_c_v_shufd, after_c_v_shufd, SIGSEGV, 0, 0, 0},
    {"v-bcast-xmm", c_v_bcast_x, ins_c_v_bcast_x, after_c_v_bcast_x, SIGSEGV, 0, 0, 0},
    {"fp-invalid", c_fma_ps, ins_c_fma_ps, after_c_fma_ps, SIGFPE, FPE_FLTINV, 2, inf_times_zero},
    {"fp-invalid-scalar", c_fma_ss, ins_c_fma_ss, after_c_fma_ss, SIGFPE, FPE_FLTINV, 2, inf_times_zero},
    {"fp-overflow", c_fma_ps, ins_c_fma_ps, after_c_fma_ps, SIGFPE, FPE_FLTOVF, 2, overflow},
    {"fp-underflow", c_fma_ps, ins_c_fma_ps, after_c_fma_ps, SIGFPE, FPE_FLTUND, 2, underflow},
    {"fp-inexact", c_fma_ps, ins_c_fma_ps, after_c_fma_ps, SIGFPE, FPE_FLTRES, 2, inexact},
    {"fp-invalid-mem", c_fma_sd_mem, ins_c_fma_sd_mem, after_c_fma_sd_mem, SIGFPE, FPE_FLTINV, 2, sd_invalid},
};

static struct {
    int sig, code;
    void *addr;
    greg_t g[NGREG];
    uint8_t fp[832];
    int fp_ok;
    int count;
} snap;
static const char *volatile resume;

static void handler(int sig, siginfo_t *si, void *vc) {
    ucontext_t *uc = vc;
    snap.sig = sig;
    snap.code = si->si_code;
    snap.addr = si->si_addr;
    memcpy(snap.g, uc->uc_mcontext.gregs, sizeof snap.g);
    snap.fp_ok = uc->uc_mcontext.fpregs != NULL;
    if (snap.fp_ok) memcpy(snap.fp, uc->uc_mcontext.fpregs, sizeof snap.fp);
    snap.count++;
    if (sig == SIGFPE) ((uint32_t *)((char *)uc->uc_mcontext.fpregs + 24))[0] |= 0x1F80;   // do not re-fault
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)resume;
}

static const int greg_of[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};

static int failures;
static void fail(const tcase *c, const char *what) {
    printf("FAIL %s: %s\n", c->name, what);
    failures++;
}

static void check(const tcase *c, const uint8_t *page0_before) {
    if (snap.count != 1) { fail(c, "no signal, or more than one"); return; }
    if (snap.sig != c->sig) { fail(c, "wrong signal"); return; }
    if (snap.g[REG_RIP] != (greg_t)c->ins) { fail(c, "rip is not the instruction"); }
    if (c->kind != 2) {
        uintptr_t lo = (uintptr_t)(c->kind == 1 ? split + PAGE : bad_page);
        if ((uintptr_t)snap.addr < lo || (uintptr_t)snap.addr >= lo + PAGE) fail(c, "si_addr outside the bad page");
    } else {
        if (snap.code != c->code) { printf("  si_code %d, wanted %d\n", snap.code, c->code); fail(c, "wrong si_code"); }
        if (snap.addr != (void *)c->ins) fail(c, "si_addr is not the instruction");
    }
    for (int k = 0; k < 16; k++)
        if (k != 4 && (uint64_t)snap.g[greg_of[k]] != gpr_init[k]) { printf("  gpr %d: %llx != %llx\n", k, (unsigned long long)snap.g[greg_of[k]], (unsigned long long)gpr_init[k]); fail(c, "GPR changed"); }
    if (((uint64_t)snap.g[REG_EFL] & ARITH) != (flags_init & ARITH)) {
        printf("  flags %llx\n", (unsigned long long)snap.g[REG_EFL]);
        fail(c, "flags changed");
    }
    if (!snap.fp_ok || *(uint32_t *)(snap.fp + 464) != 0x46505853u || !(snap.fp[512] & 4)) { fail(c, "no xsave ymm state"); return; }
    for (int i = 0; i < 16; i++) {
        if (memcmp(snap.fp + 160 + 16 * i, ymm_init[i], 16) || memcmp(snap.fp + 576 + 16 * i, ymm_init[i] + 16, 16)) {
            printf("  ymm%d\n", i);
            fail(c, "ymm changed");
        }
    }
    if (c->kind == 1 && memcmp(split, page0_before, PAGE)) fail(c, "the split store wrote");
}

int main(void) {
    if (mprotect(bad_page, PAGE, PROT_NONE) || mprotect(split + PAGE, PAGE, PROT_NONE)) return 2;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGFPE, &sa, 0);
    for (size_t n = 0; n < sizeof cases / sizeof cases[0]; n++) {
        const tcase *c = &cases[n];
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 32; j++) ymm_init[i][j] = (uint8_t)(i * 37 + j * 11 + 1);
        for (int k = 0; k < 16; k++) gpr_init[k] = 0x0101010101010101ull * (uint64_t)(k + 1) ^ 0x1122334455667788ull;
        gpr_init[15] = (uint64_t)bad_page;
        gpr_init[14] = (uint64_t)(split + PAGE - 4);
        gpr_init[13] = (uint64_t)good;
        mxcsr_init = 0x1F80;
        memset(good, 0, sizeof good);
        memset(split, 0x5a, PAGE);
        uint8_t before[PAGE];
        memcpy(before, split, PAGE);
        if (c->setup) c->setup();
        memset(&snap, 0, sizeof snap);
        resume = c->after;
        c->run();
        // The program carried on past the instruction: this line is the check.
        check(c, before);
        __asm__ volatile("ldmxcsr %0" : : "m"((uint32_t){0x1F80}));
    }
    puts(failures ? "FAIL" : "PASS");
    return failures != 0;
}

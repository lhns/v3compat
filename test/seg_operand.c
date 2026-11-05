// seg_operand: every form the client expands that takes a memory operand, with an fs: or gs: override and
// each addressing shape. Under the client a meta instruction reaches DR's segments, not the application's,
// so the client rewrites the operand. Per case, one real instruction faults on a PROT_NONE page: the signal
// must show the instruction's own state (rip, fault address, every GPR, flags, every ymm). The handler then
// makes the page readable and returns to the instruction, which must complete with the same registers,
// flags and memory as the same instruction on a plain (%r13) operand. Native, the plain run is the model.
// gs: the application's gs base is 0 (a program setting it with arch_prctl crashes under DR itself), so the operand is an address.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <ucontext.h>

#define PAGE 4096
#define ARITH 0x8D5ul

uint8_t gpage[3 * PAGE] __attribute__((aligned(PAGE)));          // gs: page 1 is the guard
__thread uint8_t tpage[3 * PAGE] __attribute__((aligned(PAGE))); // fs: page 1 is the guard
uint8_t refpage[2 * PAGE] __attribute__((aligned(PAGE)));        // the plain operand's page
uint8_t ymm_init[16][32] __attribute__((aligned(32)));
uint64_t gpr_init[16];   // hardware order: rax rcx rdx rbx rsp rbp rsi rdi r8..r15; [4] is rsp for the rsp shape
uint64_t flags_init = ARITH | 2;
uint64_t saved_rsp;
struct { uint64_t g[16]; uint8_t y[16][32]; uint64_t fl; } d_out __attribute__((aligned(32)));

#define Y(i) "vmovdqu ymm_init+" #i "*32(%rip), %ymm" #i "\n"
#define G(k, r) "mov gpr_init+" #k "*8(%rip), %" #r "\n"
#define DG(k, r) "mov %" #r ", d_out+" #k "*8(%rip)\n"
#define DY(i) "vmovdqu %ymm" #i ", d_out+128+" #i "*32(%rip)\n"
#define RSPLD "mov gpr_init+32(%rip), %rsp\n"
#define STUB(name, rspld, insn)                                                                       \
    __asm__(".text\n.globl " #name "\n.type " #name ",@function\n" #name ":\n"                       \
            "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\n"                      \
            "mov %rsp, saved_rsp(%rip)\n"                                                             \
            Y(0) Y(1) Y(2) Y(3) Y(4) Y(5) Y(6) Y(7) Y(8) Y(9) Y(10) Y(11) Y(12) Y(13) Y(14) Y(15)     \
            G(0, rax) G(1, rcx) G(2, rdx) G(3, rbx) G(5, rbp) G(6, rsi) G(7, rdi)                     \
            G(8, r8) G(9, r9) G(10, r10) G(11, r11) G(12, r12) G(13, r13) G(14, r14) G(15, r15)       \
            "push flags_init(%rip)\npopfq\n" rspld                                                    \
            ".globl ins_" #name "\nins_" #name ":\n" insn "\n"                                        \
            DG(0, rax) DG(1, rcx) DG(2, rdx) DG(3, rbx) DG(4, rsp) DG(5, rbp) DG(6, rsi) DG(7, rdi)   \
            DG(8, r8) DG(9, r9) DG(10, r10) DG(11, r11) DG(12, r12) DG(13, r13) DG(14, r14) DG(15, r15) \
            DY(0) DY(1) DY(2) DY(3) DY(4) DY(5) DY(6) DY(7) DY(8) DY(9) DY(10) DY(11) DY(12) DY(13) DY(14) DY(15) \
            "mov saved_rsp(%rip), %rsp\npushfq\npop d_out+640(%rip)\n"                                \
            "pop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n");

// Operand shapes. Registers are set so that every shape reaches the guard page (see main).
#define SEGM(s, body) "%" #s ":" body
#define SM_b_rax(s) SEGM(s, "(%rax)")
#define SM_b_rsp(s) SEGM(s, "(%rsp)")
#define SM_b_dst(s) SEGM(s, "(%rbx)")                       // the base is the GPR forms' destination
#define SM_bi_rax_rcx4(s) SEGM(s, "0x10(%rax,%rcx,4)")      // the scratch may be neither
#define SM_bi_rdx_rax8(s) SEGM(s, "-0x20(%rdx,%rax,8)")
#define SM_i_rcx2(s) SEGM(s, "0x40(,%rcx,2)")               // index only
#define SM_bi_r15_r14(s) SEGM(s, "8(%r15,%r14,1)")
#define SM_absd(s) SM_absd_##s                              // no base, no index: the local-exec TLS form
#define SM_vsy_base(s) SEGM(s, "0x10(%rax,%ymm3,4)")       // gathers: a vector index
#define SM_vsx_base(s) SEGM(s, "0x10(%rax,%xmm3,4)")
#define SM_vsy_nob(s) SM_vsy_nob_##s
#define SM_vsx_nob(s) SM_vsx_nob_##s
#define SM_vsy_nob_fs "%fs:tpage@tpoff+4096(,%ymm3,4)"
#define SM_vsy_nob_gs "%gs:gpage+4096(,%ymm3,4)"
#define SM_vsx_nob_fs "%fs:tpage@tpoff+4096(,%xmm3,4)"
#define SM_vsx_nob_gs "%gs:gpage+4096(,%xmm3,4)"
#define SM_absd_fs "%fs:tpage@tpoff+4096"
#define SM_absd_gs "%gs:gpage+4096"

// Instruction templates by category. M is the memory operand. Destinations are rbx or ymm2 (xmm2).
#define T_G3(mn, M) #mn " " M ", %rcx, %rbx"          // andn pdep pext mulx
#define T_ANDN32(mn, M) "andn " M ", %ecx, %ebx"
#define T_MULX32(mn, M) "mulx " M ", %ecx, %ebx"
#define T_GX(mn, M) #mn " %rcx, " M ", %rbx"          // bextr bzhi sarx shlx shrx
#define T_BZHI32(mn, M) "bzhi %ecx, " M ", %ebx"
#define T_G2(mn, M) #mn " " M ", %rbx"                // blsi blsmsk blsr lzcnt tzcnt
#define T_LZ32(mn, M) "lzcnt " M ", %ebx"
#define T_LZ16(mn, M) "lzcnt " M ", %bx"
#define T_TZ32(mn, M) "tzcnt " M ", %ebx"
#define T_RORX(mn, M) "rorx $5, " M ", %rbx"
#define T_RORX_32(mn, M) "rorx $5, " M ", %ebx"
#define T_MBL64(mn, M) "movbe " M ", %rbx"
#define T_MBL32(mn, M) "movbe " M ", %ebx"
#define T_MBL16(mn, M) "movbe " M ", %bx"
#define T_MBS64(mn, M) "movbe %rsi, " M
#define T_MBS32(mn, M) "movbe %esi, " M
#define T_MBS16(mn, M) "movbe %si, " M
#define T_BIN(mn, M) #mn " " M ", %ymm1, %ymm2"
#define T_BINX(mn, M) #mn " " M ", %xmm1, %xmm2"
#define T_UN(mn, M) #mn " " M ", %ymm2"
#define T_UNI(mn, M) #mn " $0x1b, " M ", %ymm2"
#define T_IMM3(mn, M) #mn " $5, " M ", %ymm1, %ymm2"
#define T_INS(mn, M) #mn " $1, " M ", %ymm1, %ymm2"
#define T_BLENDV(mn, M) "vpblendvb %ymm3, " M ", %ymm1, %ymm2"
#define T_EXT(mn, M) "vextracti128 $1, %ymm1, " M
#define T_CVTST(mn, M) "vcvtps2ph $0, %ymm1, " M
#define T_GI_Y(mn, M) #mn " %ymm0, " M ", %ymm2"      // gathers: ymm0 is the mask, ymm3 the index
#define T_GI_YX(mn, M) #mn " %xmm0, " M ", %xmm2"
#define T_GI_X(mn, M) #mn " %ymm0, " M ", %ymm2"
#define PM_GI_Y "(%r13,%ymm3,4)"
#define PM_GI_YX "(%r13,%ymm3,4)"
#define PM_GI_X "(%r13,%xmm3,4)"
#define T_MASKLD(mn, M) #mn " " M ", %ymm0, %ymm2"    // ymm0 is all ones: lane 0 is active
#define T_MASKSTD(mn, M) "vpmaskmovd %ymm1, %ymm0, " M
#define T_MASKSTQ(mn, M) "vpmaskmovq %ymm1, %ymm0, " M

#define FMA3(F, X, op) F(X, BIN, vf##op##132ps) F(X, BIN, vf##op##213ps) F(X, BIN, vf##op##231ps) \
    F(X, BIN, vf##op##132pd) F(X, BIN, vf##op##213pd) F(X, BIN, vf##op##231pd)
#define FMAS(F, X, op) F(X, BINX, vf##op##132ss) F(X, BINX, vf##op##213ss) F(X, BINX, vf##op##231ss) \
    F(X, BINX, vf##op##132sd) F(X, BINX, vf##op##213sd) F(X, BINX, vf##op##231sd)

// One representative of each expansion path (inline, helper, frame, substituted, scalar), in every shape.
#define L_REP(F, X) \
    F(X, G3, andn) F(X, RORX, rorx) F(X, G3, mulx) F(X, MBL64, movbe_ld64) F(X, MBL32, movbe_ld32) \
    F(X, MBL16, movbe_ld16) F(X, MBS64, movbe_st64) F(X, MBS32, movbe_st32) F(X, MBS16, movbe_st16) \
    F(X, G2, lzcnt) F(X, ANDN32, andn_32) F(X, BIN, vpaddq) F(X, BIN, vpsrlq) F(X, BIN, vpmaxsd) \
    F(X, BIN, vpermd) F(X, MASKLD, vpmaskmovd) F(X, MASKSTD, vpmaskmovd_st) F(X, UN, vpbroadcastb) \
    F(X, UN, vpbroadcastd) F(X, UN, vpmovzxbw) F(X, BIN, vfmadd231ps) F(X, BINX, vfmadd231sd) \
    F(X, EXT, vextracti128) F(X, INS, vinserti128) F(X, CVTST, vcvtps2ph) F(X, UN, vmovntdqa) \
    F(X, BLENDV, vpblendvb)
// The rest of the memory forms in test/allow/emulated-iforms.txt (seg_coverage.sh checks the list), in two shapes.
#define L_WIDE(F, X) \
    F(X, GX, bextr) F(X, GX, bzhi) F(X, GX, sarx) F(X, GX, shlx) F(X, GX, shrx) F(X, G2, blsi) \
    F(X, G2, blsmsk) F(X, G2, blsr) F(X, G2, tzcnt) F(X, G3, pdep) F(X, G3, pext) F(X, MULX32, mulx_32) \
    F(X, RORX_32, rorx_32) F(X, LZ32, lzcnt_32) F(X, LZ16, lzcnt_16) F(X, TZ32, tzcnt_32) \
    F(X, BZHI32, bzhi_32) F(X, MASKLD, vpmaskmovq) F(X, MASKSTQ, vpmaskmovq_st) \
    F(X, UN, vbroadcasti128) F(X, UN, vcvtph2ps) \
    F(X, IMM3, vmpsadbw) F(X, IMM3, vpalignr) F(X, IMM3, vpblendd) F(X, IMM3, vpblendw) F(X, IMM3, vperm2i128) \
    F(X, UN, vpabsb) F(X, UN, vpabsd) F(X, UN, vpabsw) \
    F(X, BIN, vpackssdw) F(X, BIN, vpacksswb) F(X, BIN, vpackusdw) F(X, BIN, vpackuswb) \
    F(X, BIN, vphaddsw) F(X, BIN, vphaddw) F(X, BIN, vphsubd) F(X, BIN, vphsubsw) F(X, BIN, vphsubw) \
    FMA3(F, X, maddsub) FMA3(F, X, msubadd) F(X, BIN, vpaddb) F(X, BIN, vpaddd) F(X, BIN, vpaddsb) F(X, BIN, vpaddsw) F(X, BIN, vpaddusb) \
    F(X, BIN, vpaddusw) F(X, BIN, vpaddw) F(X, BIN, vpand) F(X, BIN, vpandn) F(X, BIN, vpavgb) F(X, BIN, vpavgw) \
    F(X, UN, vpbroadcastq) F(X, UN, vpbroadcastw) \
    F(X, BIN, vpcmpeqb) F(X, BIN, vpcmpeqd) F(X, BIN, vpcmpeqq) F(X, BIN, vpcmpeqw) \
    F(X, BIN, vpcmpgtb) F(X, BIN, vpcmpgtd) F(X, BIN, vpcmpgtq) F(X, BIN, vpcmpgtw) \
    F(X, UNI, vpermpd) F(X, BIN, vpermps) F(X, UNI, vpermq) \
    F(X, BIN, vphaddd) \
    F(X, BIN, vpmaddubsw) F(X, BIN, vpmaddwd) \
    F(X, BIN, vpmaxsb) F(X, BIN, vpmaxsw) F(X, BIN, vpmaxub) F(X, BIN, vpmaxud) F(X, BIN, vpmaxuw) \
    F(X, BIN, vpminsb) F(X, BIN, vpminsd) F(X, BIN, vpminsw) F(X, BIN, vpminub) F(X, BIN, vpminud) F(X, BIN, vpminuw) \
    F(X, UN, vpmovsxbd) F(X, UN, vpmovsxbq) F(X, UN, vpmovsxbw) F(X, UN, vpmovsxdq) F(X, UN, vpmovsxwd) F(X, UN, vpmovsxwq) \
    F(X, UN, vpmovzxbd) F(X, UN, vpmovzxbq) F(X, UN, vpmovzxdq) F(X, UN, vpmovzxwd) F(X, UN, vpmovzxwq) \
    F(X, BIN, vpmuldq) F(X, BIN, vpmulhrsw) F(X, BIN, vpmulhuw) F(X, BIN, vpmulhw) F(X, BIN, vpmulld) \
    F(X, BIN, vpmullw) F(X, BIN, vpmuludq) F(X, BIN, vpor) F(X, BIN, vpsadbw) F(X, BIN, vpshufb) \
    F(X, UNI, vpshufd) F(X, UNI, vpshufhw) F(X, UNI, vpshuflw) F(X, BIN, vpsignb) F(X, BIN, vpsignd) F(X, BIN, vpsignw) \
    F(X, BIN, vpslld) F(X, BIN, vpsllq) F(X, BIN, vpsllw) F(X, BIN, vpsllvd) F(X, BIN, vpsllvq) \
    F(X, BIN, vpsrad) F(X, BIN, vpsraw) F(X, BIN, vpsravd) F(X, BIN, vpsrld) \
    F(X, BIN, vpsrlw) F(X, BIN, vpsrlvd) F(X, BIN, vpsrlvq) \
    F(X, BIN, vpsubb) F(X, BIN, vpsubd) F(X, BIN, vpsubq) F(X, BIN, vpsubsb) F(X, BIN, vpsubsw) \
    F(X, BIN, vpsubusb) F(X, BIN, vpsubusw) F(X, BIN, vpsubw) \
    F(X, BIN, vpunpckhbw) F(X, BIN, vpunpckhdq) F(X, BIN, vpunpckhqdq) F(X, BIN, vpunpckhwd) \
    F(X, BIN, vpunpcklbw) F(X, BIN, vpunpckldq) F(X, BIN, vpunpcklqdq) F(X, BIN, vpunpcklwd) F(X, BIN, vpxor) \
    F(X, BIN, vfmadd132ps) F(X, BIN, vfmadd213ps) F(X, BIN, vfmadd132pd) F(X, BIN, vfmadd213pd) F(X, BIN, vfmadd231pd) \
    F(X, BINX, vfmadd132ss) F(X, BINX, vfmadd213ss) F(X, BINX, vfmadd231ss) F(X, BINX, vfmadd132sd) F(X, BINX, vfmadd213sd) \
    FMA3(F, X, msub) FMA3(F, X, nmadd) FMA3(F, X, nmsub) \
    FMAS(F, X, msub) FMAS(F, X, nmadd) FMAS(F, X, nmsub) \

#define L_ALL(F, X) L_REP(F, X) L_WIDE(F, X)
// Gathers: a fault on element 0 leaves the state untouched; the restart completes all elements.
#define L_GATHER_Y(F, X) F(X, GI_Y, vgatherdps) F(X, GI_Y, vpgatherdd) F(X, GI_Y, vgatherqpd) F(X, GI_Y, vpgatherqq) F(X, GI_YX, vgatherqps) F(X, GI_YX, vpgatherqd)
#define L_GATHER_X(F, X) F(X, GI_X, vgatherdpd) F(X, GI_X, vpgatherdq)
#define L_GATHERS(F, X) L_GATHER_Y(F, X) L_GATHER_X(F, X)

#define SHAPES_FULL(X, seg, cat, mn) \
    X(seg, b_rax, cat, mn, "") X(seg, b_rsp, cat, mn, RSPLD) X(seg, b_dst, cat, mn, "") \
    X(seg, bi_rax_rcx4, cat, mn, "") X(seg, bi_rdx_rax8, cat, mn, "") X(seg, i_rcx2, cat, mn, "") \
    X(seg, bi_r15_r14, cat, mn, "") X(seg, absd, cat, mn, "")
#define SHAPES_WIDE(X, seg, cat, mn) X(seg, bi_rdx_rax8, cat, mn, "") X(seg, absd, cat, mn, "")
#define SHAPES_VSY(X, seg, cat, mn) X(seg, vsy_base, cat, mn, "") X(seg, vsy_nob, cat, mn, "")
#define SHAPES_VSX(X, seg, cat, mn) X(seg, vsx_base, cat, mn, "") X(seg, vsx_nob, cat, mn, "")
#define SEGS_VSY(X, cat, mn) SHAPES_VSY(X, fs, cat, mn) SHAPES_VSY(X, gs, cat, mn)
#define SEGS_VSX(X, cat, mn) SHAPES_VSX(X, fs, cat, mn) SHAPES_VSX(X, gs, cat, mn)
#define SEGS_FULL(X, cat, mn) SHAPES_FULL(X, fs, cat, mn) SHAPES_FULL(X, gs, cat, mn)
#define SEGS_WIDE(X, cat, mn) SHAPES_WIDE(X, fs, cat, mn) SHAPES_WIDE(X, gs, cat, mn)

#define STUB_X(seg, sh, cat, mn, rl) STUB(s_##seg##_##sh##_##mn, rl, T_##cat(mn, SM_##sh(seg)))
#define PLAIN_F(X, cat, mn) STUB(p_##mn, "", T_##cat(mn, "(%r13)"))
#define DECL_X(seg, sh, cat, mn, rl) extern void s_##seg##_##sh##_##mn(void); extern char ins_s_##seg##_##sh##_##mn[];
#define DECL_P(X, cat, mn) extern void p_##mn(void);
#define TAB_X(seg, sh, cat, mn, rl) {#seg "/" #sh "/" #mn, s_##seg##_##sh##_##mn, p_##mn, ins_s_##seg##_##sh##_##mn, \
                                     SEG_##seg, SHN_##sh},

L_REP(SEGS_FULL, STUB_X)
L_WIDE(SEGS_WIDE, STUB_X)
L_GATHER_Y(SEGS_VSY, STUB_X)
L_GATHER_X(SEGS_VSX, STUB_X)
#define PLAIN_G(X, cat, mn) STUB(p_##mn, "", T_##cat(mn, PM_##cat))
L_ALL(PLAIN_F, 0)
L_GATHERS(PLAIN_G, 0)
L_REP(SEGS_FULL, DECL_X)
L_WIDE(SEGS_WIDE, DECL_X)
L_GATHER_Y(SEGS_VSY, DECL_X)
L_GATHER_X(SEGS_VSX, DECL_X)
L_ALL(DECL_P, 0)
L_GATHERS(DECL_P, 0)

enum { SEG_fs, SEG_gs };
typedef struct { int base, idx, scale, disp, rsp; } shape_t;
enum { SHN_b_rax, SHN_b_rsp, SHN_b_dst, SHN_bi_rax_rcx4, SHN_bi_rdx_rax8, SHN_i_rcx2, SHN_bi_r15_r14, SHN_absd,
       SHN_vsy_base, SHN_vsy_nob, SHN_vsx_base, SHN_vsx_nob };
static const shape_t shapes[] = {   // register numbers in hardware order
    [SHN_b_rax] = {0, -1, 0, 0, 0},         [SHN_b_rsp] = {4, -1, 0, 0, 1},
    [SHN_b_dst] = {3, -1, 0, 0, 0},         [SHN_bi_rax_rcx4] = {0, 1, 4, 0x10, 0},
    [SHN_bi_rdx_rax8] = {2, 0, 8, -0x20, 0}, [SHN_i_rcx2] = {-1, 1, 2, 0x40, 0},
    [SHN_bi_r15_r14] = {15, 14, 1, 8, 0},   [SHN_absd] = {-1, -1, 0, 0, 0},
    [SHN_vsy_base] = {0, -1, 0, 0x10, 0}, [SHN_vsx_base] = {0, -1, 0, 0x10, 0},
    [SHN_vsy_nob] = {-1, -1, 0, 0, 0}, [SHN_vsx_nob] = {-1, -1, 0, 0, 0},
};
typedef struct { const char *name; void (*run)(void), (*plain)(void); const char *ins; int seg, shape; } tcase;
static const tcase cases[] = {
    L_REP(SEGS_FULL, TAB_X)
    L_WIDE(SEGS_WIDE, TAB_X)
    L_GATHER_Y(SEGS_VSY, TAB_X)
    L_GATHER_X(SEGS_VSX, TAB_X)
};

static struct { int count, sig; void *addr; greg_t g[NGREG]; uint8_t fp[832]; int fp_ok; } snap;
static uint8_t *volatile guard;
static void handler(int sig, siginfo_t *si, void *vc) {
    ucontext_t *uc = vc;
    if (++snap.count > 1) _exit(3);   // the restart faulted again
    snap.sig = sig;
    snap.addr = si->si_addr;
    memcpy(snap.g, uc->uc_mcontext.gregs, sizeof snap.g);
    snap.fp_ok = uc->uc_mcontext.fpregs != NULL;
    if (snap.fp_ok) memcpy(snap.fp, uc->uc_mcontext.fpregs, sizeof snap.fp);
    mprotect((void *)guard, PAGE, PROT_READ | PROT_WRITE);   // rip stays: the instruction restarts
}

static const int greg_of[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
static int failures, cross;
// Accesses of 8 bytes or more through the base register, whose start can lie before the guard page.
static int crossable(const tcase *c) {
    static const char *const ok[] = {"/andn", "/movbe_ld64", "/movbe_st64", "/vpaddq", "/vpmaxsd", "/vpermd",
                                     "/vfmadd231ps", "/vextracti128", "/vpmaskmovd"};
    if (!strstr(c->name, "/b_rax/")) return 0;
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        const char *p = strstr(c->name, ok[i]);
        if (p && p[strlen(ok[i])] == 0) return 1;
    }
    return 0;
}
static void fail(const tcase *c, const char *what) {
    if (failures++ < 40) printf("FAIL %s%s: %s\n", c->name, cross ? " (crossing)" : "", what);
}

static void check(const tcase *c, uint8_t *g, const shape_t *sh) {
    if (snap.count != 1 || snap.sig != SIGSEGV) { fail(c, "no single SIGSEGV"); return; }
    if ((const char *)snap.g[REG_RIP] != c->ins) fail(c, "rip is not the instruction");
    if (snap.addr != (void *)g) fail(c, "si_addr is not the operand's address");
    for (int k = 0; k < 16; k++)
        if ((k != 4 || sh->rsp) && (uint64_t)snap.g[greg_of[k]] != gpr_init[k]) { printf("  gpr %d\n", k); fail(c, "GPR changed"); }
    if (((uint64_t)snap.g[REG_EFL] & ARITH) != (flags_init & ARITH)) fail(c, "flags changed");
    if (!snap.fp_ok || *(uint32_t *)(snap.fp + 464) != 0x46505853u || !(snap.fp[512] & 4)) { fail(c, "no xsave ymm state"); return; }
    for (int i = 0; i < 16; i++)
        if (memcmp(snap.fp + 160 + 16 * i, ymm_init[i], 16) || memcmp(snap.fp + 576 + 16 * i, ymm_init[i] + 16, 16)) {
            printf("  ymm%d\n", i);
            fail(c, "ymm changed");
        }
}

int main(int argc, char **argv) {   // an argument selects the cases whose name contains it
    static char alt[65536];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, 0);   // the rsp shape faults with rsp pointing nowhere
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, 0);
    uint64_t fsb;
    __asm__("mov %%fs:0, %0" : "=r"(fsb));
    if (fsb % 64) { puts("FAIL setup: fs base not 64-aligned"); return 2; }
    int ran = 0;
    for (size_t n2 = 0; n2 < 2 * (sizeof cases / sizeof cases[0]); n2++) {
        const tcase *c = &cases[n2 / 2];
        cross = n2 % 2;   // second pass: the access starts 4 bytes before the guard page and crosses into it
        if (cross && !crossable(c)) continue;
        if (argc > 1 && !strstr(c->name, argv[1])) continue;
        ran++;
        const shape_t *sh = &shapes[c->shape];
        uint8_t *g = c->seg == SEG_fs ? tpage + PAGE : gpage + PAGE;
        int64_t o = (int64_t)(uintptr_t)g - (c->seg == SEG_fs ? (int64_t)fsb : 0) - sh->disp - (cross ? 4 : 0);
        for (int i = 0; i < 16; i++)
            for (int j = 0; j < 32; j++) ymm_init[i][j] = (uint8_t)(i * 37 + j * 11 + 1);
        memset(ymm_init[0], 0xff, 32);   // the mask of the masked forms and gathers
        for (int j = 0; j < 4; j++) { uint64_t v = (uint64_t)j; memcpy(ymm_init[3] + 8 * j, &v, 8); }   // the gathers' index
        for (int k = 0; k < 16; k++) gpr_init[k] = 0x0101010101010101ull * (uint64_t)(k + 1) ^ 0x1122334455667788ull;
        gpr_init[13] = (uint64_t)(refpage + PAGE - (cross ? 4 : 0));
        if (sh->idx >= 0 && sh->base >= 0) { gpr_init[sh->idx] = 3; gpr_init[sh->base] = (uint64_t)(o - 3 * sh->scale); }
        else if (sh->base >= 0) gpr_init[sh->base] = (uint64_t)o;
        else if (sh->idx >= 0) {
            if (o % sh->scale) { puts("FAIL setup: index not divisible"); return 2; }
            gpr_init[sh->idx] = (uint64_t)(o / sh->scale);
        }
        guard = g;
        mprotect(g, PAGE, PROT_READ | PROT_WRITE);
        for (int i = -16; i < 128; i++) g[i] = refpage[PAGE + i] = (uint8_t)(i * 29 + 7);
        mprotect(g, PAGE, PROT_NONE);
        c->plain();
        uint8_t d0[sizeof d_out];
        memcpy(d0, &d_out, sizeof d0);
        memset(&snap, 0, sizeof snap);
        c->run();
        // The program carried on past the instruction: this line is the check.
        check(c, g, sh);
        if (memcmp(g - 16, refpage + PAGE - 16, 144)) fail(c, "memory differs from the plain operand's");
        uint8_t d1[sizeof d_out];
        memcpy(d1, &d_out, sizeof d1);
        memset(d0 + 32, 0, 8);   // rsp: the stub's own
        memset(d1 + 32, 0, 8);
        if (memcmp(d0, d1, sizeof d0)) fail(c, "result registers or flags differ from the plain operand's");
    }
    printf("%d cases\n", ran);
    puts(failures ? "FAIL" : "PASS");
    return failures != 0;
}

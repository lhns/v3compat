// gather: the AVX2 gathers against a C model of the SDM's pseudocode, as seen by an application.
//   gather          every form, many base registers and address shapes (no base, 0x67, fs), random masks and
//                   indices: a PROT_NONE guard page beside the data makes active elements fault (the SIGSEGV
//                   handler checks the fault address, rip, all GPRs, the flags and the partial progress in
//                   dst and mask, then opens the guard so the instruction restarts), while masked-off elements
//                   on the guard must not be read. Results, GPRs and flags are compared at the end.
// GATHER_ONLY=name restricts it to one form. On hardware with AVX2 it runs natively too, which checks the model.
#define _GNU_SOURCE
#include <signal.h>
#include <stdarg.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <ucontext.h>

#define ARITH 0x8D5ul
#define PAGE 4096
#define DST 0
#define MASK 32
#define IDX 64
#define GIN 96
#define FIN 224
#define ODST 232
#define OMASK 264
#define OIDX 296
#define GOUT 328
#define FOUT 456

typedef struct {
    uint8_t dst[32], mask[32], idx[32];
    uint64_t gin[16];
    uint64_t fin;
    uint8_t odst[32], omask[32], oidx[32];
    uint64_t gout[16];
    uint64_t fout;
} ctx_t;
_Static_assert(offsetof(ctx_t, dst) == DST && offsetof(ctx_t, mask) == MASK && offsetof(ctx_t, idx) == IDX &&
               offsetof(ctx_t, gin) == GIN && offsetof(ctx_t, fin) == FIN && offsetof(ctx_t, odst) == ODST &&
               offsetof(ctx_t, omask) == OMASK && offsetof(ctx_t, oidx) == OIDX && offsetof(ctx_t, gout) == GOUT &&
               offsetof(ctx_t, fout) == FOUT, "layout");

#define LD(k, r) "mov " #k "*8+96(%rdi), %" #r "\n"
#define ST(k, r) "mov %" #r ", " #k "*8+328(%rdi)\n"
// dst ymm1/xmm1, mask 2, index 3. Every register but rsp and rdi (the context) is loaded from ctx and stored back.
#define GATHER(name, pre, insn)                                                                      \
    __asm__(".text\n.globl " #name "\n.type " #name ",@function\n" #name ":\n"                       \
            "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\n"                     \
            "vmovdqu 0(%rdi), %ymm1\nvmovdqu 32(%rdi), %ymm2\nvmovdqu 64(%rdi), %ymm3\n"             \
            LD(0, rax) LD(1, rcx) LD(2, rdx) LD(3, rbx) LD(5, rbp) LD(6, rsi)                        \
            LD(8, r8) LD(9, r9) LD(10, r10) LD(11, r11) LD(12, r12) LD(13, r13) LD(14, r14) LD(15, r15) \
            "push 224(%rdi)\npopfq\n"                                                                \
            ".globl ins_" #name "\nins_" #name ":\n" pre insn "\n.globl aft_" #name "\naft_" #name ":\n"       \
            "pushfq\npopq 456(%rdi)\n"                                                               \
            ST(0, rax) ST(1, rcx) ST(2, rdx) ST(3, rbx) ST(5, rbp) ST(6, rsi)                        \
            ST(8, r8) ST(9, r9) ST(10, r10) ST(11, r11) ST(12, r12) ST(13, r13) ST(14, r14) ST(15, r15) \
            "vmovdqu %ymm1, 232(%rdi)\nvmovdqu %ymm2, 264(%rdi)\nvmovdqu %ymm3, 296(%rdi)\n"          \
            "pop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n")

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { NOBASE = -1 };

// name, op, vector registers (mask, index, destination), segment, displacement, base, scale,
//   then for the model: base index in hardware order, 0x67, element bytes, index bytes, elements.
#define FORMS(X)                                                                                              \
    X(dd_x, "vpgatherdd", xmm2, xmm3, xmm1, "", 0, rcx, 4, RCX, 0, 4, 4, 4)                                  \
    X(dd_y, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, rcx, 4, RCX, 0, 4, 4, 8)                                  \
    X(dq_x, "vpgatherdq", xmm2, xmm3, xmm1, "", 0, rcx, 8, RCX, 0, 8, 4, 2)                                  \
    X(dq_y, "vpgatherdq", ymm2, xmm3, ymm1, "", 0, rcx, 1, RCX, 0, 8, 4, 4)                                  \
    X(qd_x, "vpgatherqd", xmm2, xmm3, xmm1, "", 0, rcx, 4, RCX, 0, 4, 8, 2)                                  \
    X(qd_y, "vpgatherqd", xmm2, ymm3, xmm1, "", 0, rcx, 2, RCX, 0, 4, 8, 4)                                  \
    X(qq_x, "vpgatherqq", xmm2, xmm3, xmm1, "", 0, rcx, 8, RCX, 0, 8, 8, 2)                                  \
    X(qq_y, "vpgatherqq", ymm2, ymm3, ymm1, "", 0, rcx, 4, RCX, 0, 8, 8, 4)                                  \
    X(dps_x, "vgatherdps", xmm2, xmm3, xmm1, "", 0, rcx, 4, RCX, 0, 4, 4, 4)                                 \
    X(dps_y, "vgatherdps", ymm2, ymm3, ymm1, "", 16, rcx, 4, RCX, 0, 4, 4, 8)                                \
    X(dpd_x, "vgatherdpd", xmm2, xmm3, xmm1, "", 0, rcx, 8, RCX, 0, 8, 4, 2)                                 \
    X(dpd_y, "vgatherdpd", ymm2, xmm3, ymm1, "", -64, rcx, 8, RCX, 0, 8, 4, 4)                               \
    X(qps_x, "vgatherqps", xmm2, xmm3, xmm1, "", 0, rcx, 4, RCX, 0, 4, 8, 2)                                 \
    X(qps_y, "vgatherqps", xmm2, ymm3, xmm1, "", 4, rcx, 4, RCX, 0, 4, 8, 4)                                 \
    X(qpd_x, "vgatherqpd", xmm2, xmm3, xmm1, "", 0, rcx, 8, RCX, 0, 8, 8, 2)                                 \
    X(qpd_y, "vgatherqpd", ymm2, ymm3, ymm1, "", 0x1000, rcx, 2, RCX, 0, 8, 8, 4)                            \
    /* every base register the client may use as scratch, or that the flag save uses */                       \
    X(b_rax, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, rax, 4, RAX, 0, 4, 4, 8)                                 \
    X(b_rdx, "vpgatherdd", ymm2, ymm3, ymm1, "", 4, rdx, 1, RDX, 0, 4, 4, 8)                                 \
    X(b_rbx, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, rbx, 2, RBX, 0, 4, 4, 8)                                 \
    X(b_rbp, "vpgatherdd", ymm2, ymm3, ymm1, "", -8, rbp, 8, RBP, 0, 4, 4, 8)                                \
    X(b_rsi, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, rsi, 4, RSI, 0, 4, 4, 8)                                 \
    X(b_r8, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, r8, 4, R8, 0, 4, 4, 8)                                    \
    X(b_r9, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, r9, 2, R9, 0, 4, 4, 8)                                    \
    X(b_r10, "vpgatherdd", ymm2, ymm3, ymm1, "", 12, r10, 4, R10, 0, 4, 4, 8)                                \
    X(b_r11, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, r11, 4, R11, 0, 4, 4, 8)                                 \
    X(b_r12, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, r12, 8, R12, 0, 4, 4, 8)                                 \
    X(b_r13, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, r13, 4, R13, 0, 4, 4, 8)                                 \
    X(b_r14, "vpgatherqq", ymm2, ymm3, ymm1, "", 0, r14, 4, R14, 0, 8, 8, 4)                                 \
    X(b_r15, "vpgatherqd", xmm2, ymm3, xmm1, "", 0, r15, 4, R15, 0, 4, 8, 4)                                 \
    /* 0x67: the address wraps at 32 bits, high bits of the base register ignored */                         \
    X(a32_rbx, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, ebx, 4, RBX, 1, 4, 4, 8)                               \
    X(a32_r9, "vpgatherqq", ymm2, ymm3, ymm1, "", 8, r9d, 2, R9, 1, 8, 8, 4)

// No base register: the displacement alone, so the indices are addresses.
#define NOBASE_FORMS(X)                                                                                       \
    X(nb_dd, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, 4, 4, 4, 8, "")                                          \
    X(nb_qq, "vpgatherqq", ymm2, ymm3, ymm1, "", 0x1000, 8, 8, 8, 4, "")                                     \
    X(nb_dps, "vgatherdps", xmm2, xmm3, xmm1, "", -0x100, 1, 4, 4, 4, "")                                    \
    X(nb_a32, "vpgatherdd", ymm2, ymm3, ymm1, "", 0, 4, 4, 4, 8, ".byte 0x67\n")

// fs: the thread pointer's base is added; the program passes target - fs base in the register.
#define FS_FORMS(X)                                                                                           \
    X(fs_dd, "vpgatherdd", ymm2, ymm3, ymm1, "%fs:", 0, rcx, 4, RCX, 0, 4, 4, 8)                             \
    X(fs_qq, "vpgatherqq", ymm2, ymm3, ymm1, "%fs:", 24, rdx, 8, RDX, 0, 8, 8, 4)

#define S_FULL(name, op, m, i, d, seg, disp, b, sc, bi, a32, es, is, n) \
    GATHER(g_##name, "", op " %" #m ", " seg #disp "(%" #b ",%" #i "," #sc "), %" #d);
#define S_NOB(name, op, m, i, d, seg, disp, sc, es, is, n, pre) \
    GATHER(g_##name, pre, op " %" #m ", " #disp "(,%" #i "," #sc "), %" #d);
FORMS(S_FULL)
FS_FORMS(S_FULL)
NOBASE_FORMS(S_NOB)

// #UD: destination, index and mask must differ.
GATHER(g_ud_maskdst, "", "vpgatherdd %ymm1, (%rcx,%ymm3,4), %ymm1");
GATHER(g_ud_idxdst, "", "vpgatherdd %ymm2, (%rcx,%ymm1,4), %ymm1");
GATHER(g_ud_idxmask, "", "vpgatherqq %ymm2, (%rcx,%ymm2,8), %ymm1");

typedef struct {
    const char *name;
    void (*fn)(ctx_t *);
    void *ins;
    int base, scale, disp, a32, fs, es, is, n;
} form_t;
#define EXT(name) extern void g_##name(ctx_t *), ins_g_##name(void);
#define T_FULL(name, op, m, i, d, seg, disp, b, sc, bi, a32, es, is, n) \
    {#name, g_##name, (void *)ins_g_##name, bi, sc, disp, a32, seg[0] != 0, es, is, n},
#define T_NOB(name, op, m, i, d, seg, disp, sc, es, is, n, pre) \
    {#name, g_##name, (void *)ins_g_##name, NOBASE, sc, disp, pre[0] != 0 && pre[1] == 'b', 0, es, is, n},
#define X_EXT_F(name, ...) EXT(name)
#define X_EXT_N(name, ...) EXT(name)
FORMS(X_EXT_F) FS_FORMS(X_EXT_F) NOBASE_FORMS(X_EXT_N)
static const form_t forms[] = {FORMS(T_FULL) FS_FORMS(T_FULL) NOBASE_FORMS(T_NOB)};

// ---- memory: two data pages with a guard page on each side ----
static uint8_t *region, *shadow;   // region: 4 pages; shadow: the same bytes, always readable
static int64_t lo, end_;           // data = [lo, end_)
static uint64_t fsbase;

static void guards(int prot) {
    mprotect(region, PAGE, prot);
    mprotect(region + 3 * PAGE, PAGE, prot);
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return rng;
}
static int64_t fdiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
static int64_t cdiv(int64_t a, int64_t b) { return -fdiv(-a, b); }

// ---- the model ----
typedef struct {
    int fault;           // the first active element that cannot be read, or -1
    uint64_t addr;       // the address the fault reports
    uint8_t dst[32], mask[32];   // the state when it faults (partial), or at the end
    uint8_t fdst[32], fmask[32];
} result_t;

static int64_t elem_index(const uint8_t *idx, int is, int k) {
    if (is == 4) { int32_t v; memcpy(&v, idx + 4 * k, 4); return v; }
    int64_t v; memcpy(&v, idx + 8 * k, 8); return v;
}

static uint64_t eaddr(const form_t *f, const ctx_t *c, uint64_t abase, int k) {
    uint64_t a = abase + (uint64_t)f->disp + (uint64_t)(elem_index(c->idx, f->is, k) * f->scale);
    return f->a32 ? (uint32_t)a : a;
}

// fault: the first active element with a byte outside the data; fdst and fmask are the state when it faults.
static void model(const form_t *f, const ctx_t *c, uint64_t abase, result_t *r) {
    memcpy(r->dst, c->dst, 32);
    memcpy(r->mask, c->mask, 32);
    r->fault = -1;
    for (int k = 0; k < f->n; k++) {
        uint8_t *m = r->mask + k * f->es;
        if (!(m[f->es - 1] & 0x80)) { memset(m, 0, f->es); continue; }   // a masked-off element's mask is cleared too
        uint64_t a = eaddr(f, c, abase, k);
        if (r->fault < 0)
            for (int b = 0; b < f->es; b++)
                if ((int64_t)(a + b) < lo || (int64_t)(a + b) >= end_) {
                    r->fault = k;
                    r->addr = a + b;
                    memcpy(r->fdst, r->dst, 32);
                    memcpy(r->fmask, r->mask, 32);
                    break;
                }
        memcpy(r->dst + k * f->es, shadow + (a - (uint64_t)region), f->es);   // the restart reads it too
        memset(m, 0, f->es);
    }
    memset(r->dst + f->n * f->es, 0, 32 - f->n * f->es);
    memset(r->mask + f->n * f->es, 0, 32 - f->n * f->es);
}

// ---- the signal handlers ----
static struct {
    const form_t *f;
    ctx_t *c;
    const result_t *r;
    int faults, bad;
    char why[200];
} cur;
static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!cur.bad) vsnprintf(cur.why, sizeof cur.why, fmt, ap);
    va_end(ap);
    cur.bad = 1;
}

static void read_ymm(ucontext_t *uc, int n, uint8_t *out) {
    char *fp = (char *)uc->uc_mcontext.fpregs;
    memcpy(out, fp + 160 + 16 * n, 16);
    memcpy(out + 16, fp + 576 + 16 * n, 16);
}
static const int greg_of[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};

// The registers and flags are the application's at any interruption.
static void check_app_state(ucontext_t *uc, int check_rip) {
    for (int g = 0; g < 16; g++) {
        if (g == RSP || g == RDI) continue;
        if ((uint64_t)uc->uc_mcontext.gregs[greg_of[g]] != cur.c->gin[g])
            fail("gpr %d is %llx, expected %llx", g, (unsigned long long)uc->uc_mcontext.gregs[greg_of[g]],
                 (unsigned long long)cur.c->gin[g]);
    }
    if ((uint64_t)uc->uc_mcontext.gregs[REG_RDI] != (uint64_t)cur.c) fail("rdi changed");
    if (((uint64_t)uc->uc_mcontext.gregs[REG_EFL] & ARITH) != (cur.c->fin & ARITH))
        fail("flags %llx, expected %llx", (unsigned long long)uc->uc_mcontext.gregs[REG_EFL] & ARITH,
             (unsigned long long)cur.c->fin & ARITH);
    if (check_rip && (uint64_t)uc->uc_mcontext.gregs[REG_RIP] != (uint64_t)cur.f->ins)
        fail("rip %llx, expected %p", (unsigned long long)uc->uc_mcontext.gregs[REG_RIP], cur.f->ins);
}

static void on_segv(int sig, siginfo_t *si, void *u) {
    ucontext_t *uc = u;
    cur.faults++;
    if (sig != SIGSEGV || cur.faults > 1 || cur.r == NULL || cur.r->fault < 0) {
        fail("unexpected signal %d, %d faults, at %p", sig, cur.faults, si->si_addr);
        if (cur.faults > 3) { printf("FAIL %s: unexpected fault at %p, rip %llx, lo %llx fs %llx rcx %llx idx0 %lld\n", cur.f->name, si->si_addr, (unsigned long long)uc->uc_mcontext.gregs[REG_RIP], (unsigned long long)lo, (unsigned long long)fsbase, (unsigned long long)uc->uc_mcontext.gregs[REG_RCX], (long long)elem_index(cur.c->idx, cur.f->is, 0)); _exit(1); }
        guards(PROT_READ | PROT_WRITE);
        return;
    }
    check_app_state(uc, 1);
    if ((uint64_t)si->si_addr != cur.r->addr)
        fail("fault address %p, expected %llx", si->si_addr, (unsigned long long)cur.r->addr);
    uint8_t d[32], m[32], x[32];
    read_ymm(uc, 1, d); read_ymm(uc, 2, m); read_ymm(uc, 3, x);
    const form_t *f = cur.f;
    for (int k = 0; k < f->n; k++) {
        const uint8_t *om = cur.c->mask + k * f->es, *gm = m + k * f->es;
        if (memcmp(d + k * f->es, cur.r->fdst + k * f->es, f->es))
            fail("element %d of dst at the fault differs (fault at %d)", k, cur.r->fault);
        if (k < cur.r->fault) {
            for (int b = 0; b < f->es; b++)
                if (gm[b]) { fail("mask element %d not cleared before the fault at %d", k, cur.r->fault); break; }
        } else if ((om[f->es - 1] ^ gm[f->es - 1]) & 0x80) {
            fail("mask element %d has MSB %d at the fault at %d", k, gm[f->es - 1] >> 7, cur.r->fault);
        }
    }
    if (memcmp(x, cur.c->idx, 32)) fail("the index register changed");
    guards(PROT_READ | PROT_WRITE);
}

// ---- #UD: SIGILL at the gather with registers and memory as they were ----
extern void g_ud_maskdst(ctx_t *), g_ud_idxdst(ctx_t *), g_ud_idxmask(ctx_t *);
extern void ins_g_ud_maskdst(void), ins_g_ud_idxdst(void), ins_g_ud_idxmask(void);
extern void aft_g_ud_maskdst(void), aft_g_ud_idxdst(void), aft_g_ud_idxmask(void);
static int ill_seen;
static void *ill_ins, *ill_aft;
static void on_ill(int sig, siginfo_t *si, void *u) {
    ucontext_t *uc = u;
    ill_seen++;
    if ((void *)uc->uc_mcontext.gregs[REG_RIP] != ill_ins) {
        printf("FAIL #UD: rip %llx, expected %p%s", (unsigned long long)uc->uc_mcontext.gregs[REG_RIP], ill_ins, "\n");
        _exit(1);
    }
    cur.c->gout[0] = 0;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)ill_aft;
}

static int run_ud(const char *name, void (*fn)(ctx_t *), void *ins, void *aft) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_ill;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    ctx_t c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 96; i++) c.dst[i] = (uint8_t)rnd();   // dst, mask and index
    for (int g = 0; g < 16; g++) c.gin[g] = rnd();
    c.gin[RCX] = (uint64_t)region + PAGE;
    for (int i = 0; i < 32; i++) c.mask[i] |= 0x80 * (i % 4 == 3);   // elements active, so a wrong expansion would read
    c.fin = 2;
    memcpy(c.oidx, c.idx, 32);
    cur.c = &c;
    ill_seen = 0; ill_ins = ins; ill_aft = aft;
    fn(&c);
    int bad = ill_seen != 1;
    for (int g = 0; g < 16; g++) if (g != RSP && g != RDI && g != RAX && c.gout[g] != c.gin[g]) bad = 1;
    if (bad) printf("FAIL %s: %d SIGILL, registers %s\n", name, ill_seen, "changed");
    return bad;
}

// ---- one case ----
static void gen_case(const form_t *f, ctx_t *c, int trial, uint64_t *abase) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 32; i++) c->dst[i] = (uint8_t)rnd();
    for (int g = 0; g < 16; g++) c->gin[g] = rnd();
    c->fin = (rnd() & 1 ? ARITH : 0) | 2;
    int64_t C = lo + (int64_t)(rnd() % (2 * PAGE));
    if (f->base == NOBASE) { *abase = 0; C = f->disp; }
    else {
        uint64_t b = (uint64_t)(C - f->disp);
        if (f->fs) b -= fsbase;
        if (f->a32) b = (uint32_t)(C - f->disp) | (rnd() << 32);
        c->gin[f->base] = b;
        *abase = f->fs ? b + fsbase : b;
    }
    int scale = f->scale, es = f->es, is = f->is;
    int mode = trial % 8, fj = -1;
    if (mode == 3 || mode == 4 || mode == 5 || mode == 6) fj = mode == 5 ? f->n - 1 : (int)(rnd() % f->n);
    int64_t c0 = (int64_t)*abase + f->disp;
    if (f->a32) c0 = (uint32_t)c0;
    for (int k = 0; k < f->n; k++) {
        int on = (mode <= 1 || mode == 4 || mode == 6) ? 1 : mode == 2 ? 0 : (int)(rnd() & 1);
        if (mode == 1) on = rnd() % 4 != 0;
        if (k == fj) on = 1;
        uint8_t *m = c->mask + k * es;
        for (int b = 0; b < es; b++) m[b] = (uint8_t)rnd();
        m[es - 1] = on ? (m[es - 1] | 0x80) : (m[es - 1] & 0x7f);
        // where the element points
        int zone;   // 0 data, 1 guard below, 2 guard above or straddling into it, 3 wild
        if (k == fj) zone = mode == 6 ? 2 : (rnd() & 1 ? 1 : 2);
        else if (!on) zone = rnd() % 4;
        else zone = 0;
        int64_t v;
        if (zone == 3) {
            v = (int64_t)rnd();
            if (is == 4) v = (int32_t)v;
            else v >>= 8;   // keeps the address arithmetic from wrapping into the data
        } else {
            int64_t a_lo = zone == 0 ? lo : zone == 1 ? lo - PAGE : end_ - es + 1;
            int64_t a_hi = zone == 0 ? end_ - es : zone == 1 ? lo - 1 : end_ + PAGE - es;
            int64_t i_lo = cdiv(a_lo - c0, scale), i_hi = fdiv(a_hi - c0, scale);
            v = i_lo + (int64_t)(rnd() % (uint64_t)(i_hi - i_lo + 1));
        }
        memcpy(c->idx + k * is, &v, is);
    }
    for (int i = f->n * is; i < 32; i++) c->idx[i] = (uint8_t)rnd();
    // the unused part of the mask: random, to see it cleared
    for (int i = f->n * es; i < 32; i++) c->mask[i] = (uint8_t)rnd();
}

static int run_form(const form_t *f, int trials) {
    int bad = 0;
    for (int t = 0; t < trials; t++) {
        ctx_t c;
        uint64_t abase;
        gen_case(f, &c, t, &abase);
        result_t r;
        model(f, &c, abase, &r);
        memset(&cur, 0, sizeof cur);
        cur.f = f; cur.c = &c; cur.r = &r;
        guards(PROT_NONE);
        f->fn(&c);
        guards(PROT_READ | PROT_WRITE);
        const char *why = NULL;
        char buf[160];
        if (cur.bad) why = cur.why;
        else if (cur.faults != (r.fault >= 0)) { snprintf(buf, sizeof buf, "%d faults, expected %d", cur.faults, r.fault >= 0); why = buf; }
        else if (memcmp(c.odst, r.dst, 32)) why = "dst differs";
        else if (memcmp(c.omask, r.mask, 32)) why = "mask differs";
        else if (memcmp(c.oidx, c.idx, 32)) why = "index register changed";
        else if ((c.fout & ARITH) != (c.fin & ARITH)) why = "flags changed";
        else {
            for (int g = 0; g < 16; g++)
                if (g != RSP && g != RDI && c.gout[g] != c.gin[g]) { snprintf(buf, sizeof buf, "gpr %d changed", g); why = buf; break; }
        }
        if (why) {
            printf("FAIL %s trial %d (mode %d): %s\n", f->name, t, t % 8, why);
            if (++bad >= 3) break;
        }
    }
    return bad;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    region = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    shadow = malloc(4 * PAGE);
    if (region == MAP_FAILED || !shadow) return 2;
    for (int i = 0; i < 4 * PAGE; i++) region[i] = shadow[i] = (uint8_t)rnd();
    lo = (int64_t)(uintptr_t)region + PAGE;
    end_ = lo + 2 * PAGE;
    __asm__("mov %%fs:0, %0" : "=r"(fsbase));
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    int bad = 0;
    int trials = argc > 1 ? atoi(argv[1]) : 400, nf = (int)(sizeof forms / sizeof forms[0]);
    for (int i = 0; i < nf; i++)
        if (!getenv("GATHER_ONLY") || !strcmp(getenv("GATHER_ONLY"), forms[i].name)) bad += run_form(&forms[i], trials);
    bad += run_ud("mask=dst", g_ud_maskdst, ins_g_ud_maskdst, aft_g_ud_maskdst);
    bad += run_ud("index=dst", g_ud_idxdst, ins_g_ud_idxdst, aft_g_ud_idxdst);
    bad += run_ud("index=mask", g_ud_idxmask, ins_g_ud_idxmask, aft_g_ud_idxmask);
    printf("%d forms x %d cases\n", nf, trials);
    puts(bad ? "FAIL" : "PASS");
    return bad != 0;
}

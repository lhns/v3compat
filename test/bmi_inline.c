// bmi_inline: rorx, andn, mulx and movbe compared with a C model, in the register aliasings and memory
// forms an inline expansion could get wrong: a destination that is a source, the address base, rax or rdx;
// 16-, 32- and 64-bit; rsp-based and rip-relative memory. Every case also checks the flags (kept, or for
// andn SF ZF CF OF) and that no other register changed. Runs the same under V3COMPAT_HELPERS_ONLY=1.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARITH 0x8D5ul

uint64_t tin[16], tout[16];   // hardware register order: rax rcx rdx rbx rsp rbp rsi rdi r8..r15
uint64_t tfl_in, tfl_out;
uint8_t tmem[16] __attribute__((aligned(16)));

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15, MEM = -1 };
enum { RORX, ANDN, MULX, MOVBE_LD, MOVBE_ST };

#define LD(k, r) "mov tin+" #k "*8(%rip), %" #r "\n"
#define ST(k, r) "mov %" #r ", tout+" #k "*8(%rip)\n"
#define RSP_PRE "pushq tmem(%rip)\n"      // the memory operand is (%rsp): a copy of tmem's first 8 bytes
#define RSP_POST "popq tmem(%rip)\n"
#define STUB(name, pre, insn, post)                                                                  \
    __asm__(".text\n.globl " #name "\n.type " #name ",@function\n" #name ":\n"                       \
            "push %rbx\npush %rbp\npush %r12\npush %r13\npush %r14\npush %r15\n"                     \
            LD(0, rax) LD(1, rcx) LD(2, rdx) LD(3, rbx) LD(5, rbp) LD(6, rsi) LD(7, rdi)             \
            LD(8, r8) LD(9, r9) LD(10, r10) LD(11, r11) LD(12, r12) LD(13, r13) LD(14, r14) LD(15, r15) \
            pre "push tfl_in(%rip)\npopfq\n" insn "\n"                                                \
            "pushfq\npopq tfl_out(%rip)\n" post                                                       \
            ST(0, rax) ST(1, rcx) ST(2, rdx) ST(3, rbx) ST(5, rbp) ST(6, rsi) ST(7, rdi)             \
            ST(8, r8) ST(9, r9) ST(10, r10) ST(11, r11) ST(12, r12) ST(13, r13) ST(14, r14) ST(15, r15) \
            "pop %r15\npop %r14\npop %r13\npop %r12\npop %rbp\npop %rbx\nret\n")

// name, instruction, kind, p0 p1 p2, bits, imm, pre, post.
//   rorx: p0 = destination, p1 = source or MEM.   andn: p0 = destination, p1 = vvvv (inverted), p2 = r/m or MEM.
//   mulx: p0 = low (vvvv), p1 = high, p2 = source or MEM.   movbe load: p0 = destination.   store: p0 = source.
#define CASES(X)                                                                                         \
    X(rorx_rr, "rorx $13, %rcx, %rdx", RORX, RDX, RCX, 0, 64, 13, "", "")                               \
    X(rorx_alias, "rorx $13, %rcx, %rcx", RORX, RCX, RCX, 0, 64, 13, "", "")                            \
    X(rorx_rax_same, "rorx $63, %rax, %rax", RORX, RAX, RAX, 0, 64, 63, "", "")                         \
    X(rorx_to_rax, "rorx $7, %rsi, %rax", RORX, RAX, RSI, 0, 64, 7, "", "")                             \
    X(rorx_from_rax, "rorx $1, %rax, %r9", RORX, R9, RAX, 0, 64, 1, "", "")                             \
    X(rorx_high, "rorx $29, %r12, %r15", RORX, R15, R12, 0, 64, 29, "", "")                             \
    X(rorx32, "rorx $5, %ecx, %edx", RORX, RDX, RCX, 0, 32, 5, "", "")                                  \
    X(rorx32_alias, "rorx $31, %eax, %eax", RORX, RAX, RAX, 0, 32, 31, "", "")                          \
    X(rorx_zero, "rorx $0, %r8, %r9", RORX, R9, R8, 0, 64, 0, "", "")                                   \
    X(rorx32_mask, "rorx $33, %r8d, %r9d", RORX, R9, R8, 0, 32, 33, "", "")                             \
    X(rorx64_mask, "rorx $127, %r8, %r9", RORX, R9, R8, 0, 64, 127, "", "")                             \
    X(rorx_mem, "rorx $9, (%r14), %r10", RORX, R10, MEM, 0, 64, 9, "", "")                              \
    X(rorx_mem_rax, "rorx $9, (%r14), %rax", RORX, RAX, MEM, 0, 64, 9, "", "")                          \
    X(rorx_mem_base, "rorx $9, (%r14), %r14", RORX, R14, MEM, 0, 64, 9, "", "")                         \
    X(rorx_mem32, "rorx $17, (%r14), %r10d", RORX, R10, MEM, 0, 32, 17, "", "")                         \
    X(rorx_rsp, "rorx $11, (%rsp), %r10", RORX, R10, MEM, 0, 64, 11, RSP_PRE, RSP_POST)                 \
    X(rorx_rip, "rorx $11, tmem(%rip), %r10", RORX, R10, MEM, 0, 64, 11, "", "")                        \
    X(andn_rr, "andn %rcx, %rdx, %rsi", ANDN, RSI, RDX, RCX, 64, 0, "", "")                             \
    X(andn_dst_v, "andn %rcx, %rdx, %rdx", ANDN, RDX, RDX, RCX, 64, 0, "", "")                          \
    X(andn_dst_rm, "andn %rcx, %rdx, %rcx", ANDN, RCX, RDX, RCX, 64, 0, "", "")                         \
    X(andn_same, "andn %rcx, %rcx, %rax", ANDN, RAX, RCX, RCX, 64, 0, "", "")                           \
    X(andn_rax, "andn %rax, %rdx, %rbx", ANDN, RBX, RDX, RAX, 64, 0, "", "")                            \
    X(andn32, "andn %ecx, %edx, %esi", ANDN, RSI, RDX, RCX, 32, 0, "", "")                              \
    X(andn_mem, "andn (%r14), %rdx, %rsi", ANDN, RSI, RDX, MEM, 64, 0, "", "")                          \
    X(andn_mem32, "andn (%r14), %edx, %esi", ANDN, RSI, RDX, MEM, 32, 0, "", "")                        \
    X(andn_mem_base, "andn (%r14), %rdx, %r14", ANDN, R14, RDX, MEM, 64, 0, "", "")                     \
    X(andn_mem_v, "andn (%r14), %r14, %rsi", ANDN, RSI, R14, MEM, 64, 0, "", "")                        \
    X(andn_rsp, "andn (%rsp), %rdx, %rsi", ANDN, RSI, RDX, MEM, 64, 0, RSP_PRE, RSP_POST)               \
    X(andn_rip, "andn tmem(%rip), %edx, %esi", ANDN, RSI, RDX, MEM, 32, 0, "", "")                      \
    X(mulx_rr, "mulx %rcx, %rsi, %rdi", MULX, RSI, RDI, RCX, 64, 0, "", "")                             \
    X(mulx_same, "mulx %rcx, %rsi, %rsi", MULX, RSI, RSI, RCX, 64, 0, "", "")                           \
    X(mulx_src_rdx, "mulx %rdx, %rsi, %rdi", MULX, RSI, RDI, RDX, 64, 0, "", "")                        \
    X(mulx_src_rax, "mulx %rax, %rsi, %rdi", MULX, RSI, RDI, RAX, 64, 0, "", "")                        \
    X(mulx_lo_rax_hi_rdx, "mulx %rcx, %rax, %rdx", MULX, RAX, RDX, RCX, 64, 0, "", "")                  \
    X(mulx_lo_rdx_hi_rax, "mulx %rcx, %rdx, %rax", MULX, RDX, RAX, RCX, 64, 0, "", "")                  \
    X(mulx_lo_src, "mulx %rcx, %rcx, %rdi", MULX, RCX, RDI, RCX, 64, 0, "", "")                         \
    X(mulx_hi_src, "mulx %rcx, %rsi, %rcx", MULX, RSI, RCX, RCX, 64, 0, "", "")                         \
    X(mulx_all_rdx, "mulx %rcx, %rdx, %rdx", MULX, RDX, RDX, RCX, 64, 0, "", "")                        \
    X(mulx_all_rax, "mulx %rcx, %rax, %rax", MULX, RAX, RAX, RCX, 64, 0, "", "")                        \
    X(mulx_rax_rax_rdx, "mulx %rax, %rax, %rdx", MULX, RAX, RDX, RAX, 64, 0, "", "")                    \
    X(mulx_rdx_rdx_rax, "mulx %rdx, %rdx, %rax", MULX, RDX, RAX, RDX, 64, 0, "", "")                    \
    X(mulx_hi_rdx_src_rdx, "mulx %rdx, %rsi, %rdx", MULX, RSI, RDX, RDX, 64, 0, "", "")                 \
    X(mulx_regs, "mulx %rbx, %rcx, %rsi", MULX, RCX, RSI, RBX, 64, 0, "", "")                           \
    X(mulx_high_regs, "mulx %r11, %r12, %r13", MULX, R12, R13, R11, 64, 0, "", "")                      \
    X(mulx32, "mulx %ecx, %esi, %edi", MULX, RSI, RDI, RCX, 32, 0, "", "")                              \
    X(mulx32_same, "mulx %ecx, %esi, %esi", MULX, RSI, RSI, RCX, 32, 0, "", "")                         \
    X(mulx32_ax_dx, "mulx %ecx, %eax, %edx", MULX, RAX, RDX, RCX, 32, 0, "", "")                        \
    X(mulx32_src_rdx, "mulx %edx, %esi, %edi", MULX, RSI, RDI, RDX, 32, 0, "", "")                      \
    X(mulx32_alias, "mulx %ecx, %ecx, %ecx", MULX, RCX, RCX, RCX, 32, 0, "", "")                        \
    X(mulx_mem, "mulx (%r14), %rsi, %rdi", MULX, RSI, RDI, MEM, 64, 0, "", "")                          \
    X(mulx_mem32, "mulx (%r14), %esi, %edi", MULX, RSI, RDI, MEM, 32, 0, "", "")                        \
    X(mulx_mem_lo_base, "mulx (%r14), %r14, %rdi", MULX, R14, RDI, MEM, 64, 0, "", "")                  \
    X(mulx_mem_hi_base, "mulx (%r14), %rsi, %r14", MULX, RSI, R14, MEM, 64, 0, "", "")                  \
    X(mulx_mem_rdx_rax, "mulx (%r14), %rdx, %rax", MULX, RDX, RAX, MEM, 64, 0, "", "")                  \
    X(mulx_rsp, "mulx (%rsp), %rsi, %rdi", MULX, RSI, RDI, MEM, 64, 0, RSP_PRE, RSP_POST)               \
    X(mulx_rip, "mulx tmem(%rip), %esi, %edi", MULX, RSI, RDI, MEM, 32, 0, "", "")                      \
    X(movbe_ld64, "movbe (%r14), %rsi", MOVBE_LD, RSI, 0, 0, 64, 0, "", "")                             \
    X(movbe_ld32, "movbe (%r14), %esi", MOVBE_LD, RSI, 0, 0, 32, 0, "", "")                             \
    X(movbe_ld16, "movbe (%r14), %si", MOVBE_LD, RSI, 0, 0, 16, 0, "", "")                              \
    X(movbe_ld16_ax, "movbe (%r14), %ax", MOVBE_LD, RAX, 0, 0, 16, 0, "", "")                           \
    X(movbe_ld16_dx, "movbe (%r14), %dx", MOVBE_LD, RDX, 0, 0, 16, 0, "", "")                           \
    X(movbe_ld16_bx, "movbe (%r14), %bx", MOVBE_LD, RBX, 0, 0, 16, 0, "", "")                           \
    X(movbe_ld64_rax, "movbe (%r14), %rax", MOVBE_LD, RAX, 0, 0, 64, 0, "", "")                         \
    X(movbe_ld64_base, "movbe (%r14), %r14", MOVBE_LD, R14, 0, 0, 64, 0, "", "")                        \
    X(movbe_ld32_base, "movbe (%r14), %r14d", MOVBE_LD, R14, 0, 0, 32, 0, "", "")                       \
    X(movbe_ld16_base, "movbe (%r14), %r14w", MOVBE_LD, R14, 0, 0, 16, 0, "", "")                       \
    X(movbe_ld_rsp, "movbe (%rsp), %rsi", MOVBE_LD, RSI, 0, 0, 64, 0, RSP_PRE, RSP_POST)                \
    X(movbe_ld_rip, "movbe tmem(%rip), %rsi", MOVBE_LD, RSI, 0, 0, 64, 0, "", "")                       \
    X(movbe_ld16_rip, "movbe tmem(%rip), %bx", MOVBE_LD, RBX, 0, 0, 16, 0, "", "")                      \
    X(movbe_st64, "movbe %rsi, (%r14)", MOVBE_ST, RSI, 0, 0, 64, 0, "", "")                             \
    X(movbe_st32, "movbe %esi, (%r14)", MOVBE_ST, RSI, 0, 0, 32, 0, "", "")                             \
    X(movbe_st16, "movbe %si, (%r14)", MOVBE_ST, RSI, 0, 0, 16, 0, "", "")                              \
    X(movbe_st16_ax, "movbe %ax, (%r14)", MOVBE_ST, RAX, 0, 0, 16, 0, "", "")                           \
    X(movbe_st16_bx, "movbe %bx, (%r14)", MOVBE_ST, RBX, 0, 0, 16, 0, "", "")                           \
    X(movbe_st16_base, "movbe %r14w, (%r14)", MOVBE_ST, R14, 0, 0, 16, 0, "", "")                       \
    X(movbe_st_rsp, "movbe %rsi, (%rsp)", MOVBE_ST, RSI, 0, 0, 64, 0, RSP_PRE, RSP_POST)                \
    X(movbe_st_rip, "movbe %esi, tmem(%rip)", MOVBE_ST, RSI, 0, 0, 32, 0, "", "")

#define DEF(name, insn, kind, p0, p1, p2, bits, imm, pre, post) STUB(name, pre, insn, post);
CASES(DEF)
#define DECL(name, ...) extern void name(void);
CASES(DECL)

typedef struct {
    const char *name;
    void (*fn)(void);
    int kind, p0, p1, p2, bits, imm;
} tcase;
#define ROW(name, insn, kind, p0, p1, p2, bits, imm, pre, post) {#name, name, kind, p0, p1, p2, bits, imm},
static const tcase cases[] = {CASES(ROW)};

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}
// Mostly random, with the values that expose carries, sign and zero results.
static uint64_t pick(int i) {
    static const uint64_t special[] = {0, ~0ull, 1, 0x8000000000000000ull, 0x7fffffffffffffffull, 0xffffffffull,
                                       0x80000000ull, 0x100000000ull};
    return (rnd() & 7) == 0 ? special[rnd() % 8] : (i & 1 ? rnd() : rnd() & 0xffffffffull);
}

static int failures;

static void run_case(const tcase *c, int iter) {
    uint64_t m = c->bits == 64 ? ~0ull : 0xffffffffull;
    for (int k = 0; k < 16; k++) tin[k] = pick(iter + k);
    tin[R14] = (uint64_t)tmem;
    for (int k = 0; k < 16; k++) tmem[k] = (uint8_t)rnd();
    tfl_in = 2 | (rnd() & ARITH);
    if (iter == 0) tfl_in = 2;
    if (iter == 1) tfl_in = 2 | ARITH;
    uint64_t exp[16], memv = 0, fl = tfl_in & ARITH, flmask = ARITH;
    uint8_t expmem[16];
    memcpy(exp, tin, sizeof exp);
    memcpy(expmem, tmem, sizeof expmem);
    memcpy(&memv, tmem, c->bits / 8);
    switch (c->kind) {
    case RORX: {
        uint64_t s = (c->p1 == MEM ? memv : tin[c->p1]) & m;
        int cnt = c->imm & (c->bits - 1);
        exp[c->p0] = (cnt ? (s >> cnt) | (s << (c->bits - cnt)) : s) & m;
        break;
    }
    case ANDN: {
        uint64_t r = ~(tin[c->p1] & m) & ((c->p2 == MEM ? memv : tin[c->p2]) & m);
        exp[c->p0] = r;
        fl = (r == 0 ? 0x40 : 0) | ((r >> (c->bits - 1)) & 1 ? 0x80 : 0);
        flmask = 0x8C1;   // SF ZF CF OF; AF and PF are undefined
        break;
    }
    case MULX: {
        unsigned __int128 p = (unsigned __int128)(tin[RDX] & m) * ((c->p2 == MEM ? memv : tin[c->p2]) & m);
        exp[c->p0] = (uint64_t)p & m;
        exp[c->p1] = (uint64_t)(p >> c->bits) & m;   // the high half wins when both are one register
        break;
    }
    case MOVBE_LD: {
        uint64_t v = c->bits == 64 ? __builtin_bswap64(memv)
                     : c->bits == 32 ? __builtin_bswap32((uint32_t)memv) : __builtin_bswap16((uint16_t)memv);
        exp[c->p0] = c->bits == 16 ? (tin[c->p0] & ~0xffffull) | v : v;
        break;
    }
    case MOVBE_ST: {
        uint64_t s = tin[c->p0];
        uint64_t v = c->bits == 64 ? __builtin_bswap64(s)
                     : c->bits == 32 ? __builtin_bswap32((uint32_t)s) : __builtin_bswap16((uint16_t)s);
        memcpy(expmem, &v, c->bits / 8);
        break;
    }
    }
    c->fn();
    int bad = 0;
    for (int k = 0; k < 16; k++)
        if (k != RSP && tout[k] != exp[k]) {
            printf("FAIL %s: reg %d is %llx, wanted %llx\n", c->name, k, (unsigned long long)tout[k],
                   (unsigned long long)exp[k]);
            bad = 1;
        }
    if ((tfl_out & flmask) != (fl & flmask)) {
        printf("FAIL %s: flags %llx from %llx, wanted %llx under %llx\n", c->name, (unsigned long long)tfl_out,
               (unsigned long long)tfl_in, (unsigned long long)fl, (unsigned long long)flmask);
        bad = 1;
    }
    if (memcmp(tmem, expmem, sizeof tmem)) {
        printf("FAIL %s: memory changed\n", c->name);
        bad = 1;
    }
    if (bad) {
        printf("  inputs rax=%llx rcx=%llx rdx=%llx rsi=%llx mem=%llx\n", (unsigned long long)tin[RAX],
               (unsigned long long)tin[RCX], (unsigned long long)tin[RDX], (unsigned long long)tin[RSI],
               (unsigned long long)memv);
        failures++;
    }
}

int main(void) {
    for (size_t n = 0; n < sizeof cases / sizeof cases[0]; n++)
        for (int i = 0; i < 200 && failures < 20; i++) run_case(&cases[n], i);
    puts(failures ? "FAIL" : "PASS");
    return failures != 0;
}

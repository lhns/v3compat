// seq_rwx SEED COUNT: random straight-line sequences of emulated and native instructions, generated into RWX memory
// and run once each; prints a digest of every register (ymm, GPRs, CF and ZF) and the scratch memory after each.
// test/seq_rwx.sh compares the digests under the client against native. Sequences cross DynamoRIO's block ends (256
// instructions, the sixth store of a sandboxed block), keep CF and ZF live across emulated instructions, and some
// store into their own code (an immediate further on) right before an emulated instruction.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef struct {
    uint8_t ymm[16][32];
    uint64_t g[16];
    uint64_t flags;
    uint8_t scratch[512];
} __attribute__((aligned(32))) state_t;

#define OFF_Y(i) (offsetof(state_t, ymm) + 32 * (i))
#define OFF_G(i) (offsetof(state_t, g) + 8 * (i))
#define OFF_F offsetof(state_t, flags)
#define OFF_S offsetof(state_t, scratch)
#define CODE_SIZE (1 << 16)

static uint64_t rng;
static uint64_t next(void) {   // splitmix64
    uint64_t z = (rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

static uint8_t *code, *p;
static void b(unsigned x) { *p++ = (uint8_t)x; }
static void d32(uint32_t x) { memcpy(p, &x, 4); p += 4; }

// GPRs a sequence may use: all but rsp (4) and rdi (7), which holds the state.
static const int gprs[] = {0, 1, 2, 3, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15};
static int gpr(void) { return gprs[pick(sizeof gprs / sizeof gprs[0])]; }
static int ymm(void) { return (int)pick(16); }

// Three-byte VEX, register-register (mod 11), or [rdi+disp32] when mem.
static void vex(int map, int w, int vvvv, int l, int pp, int op, int reg, int rm, int mem, uint32_t disp) {
    b(0xC4);
    b((reg & 8 ? 0 : 0x80) | 0x40 | (!mem && (rm & 8) ? 0 : 0x20) | map);
    b((w << 7) | ((~vvvv & 15) << 3) | (l << 2) | pp);
    b(op);
    if (mem) { b(0x80 | (reg & 7) << 3 | 7); d32(disp); }
    else b(0xC0 | (reg & 7) << 3 | (rm & 7));
}
static void rex_rr(int w, int reg, int rm) {
    b(0x40 | (w << 3) | (reg & 8 ? 4 : 0) | (rm & 8 ? 1 : 0));
}
static void alu_rr(int op, int dst, int src) { rex_rr(1, src, dst); b(op); b(0xC0 | (src & 7) << 3 | (dst & 7)); }
static void mov_ld(int r, uint32_t off) { rex_rr(1, r, 7); b(0x8B); b(0x80 | (r & 7) << 3 | 7); d32(off); }
static void mov_st(int r, uint32_t off) { rex_rr(1, r, 7); b(0x89); b(0x80 | (r & 7) << 3 | 7); d32(off); }
static void vmovdqu_ld(int y, uint32_t off) { vex(1, 0, 0, 1, 2, 0x6F, y, 7, 1, off); }
static void vmovdqu_st(int y, uint32_t off) { vex(1, 0, 0, 1, 2, 0x7F, y, 7, 1, off); }
static uint32_t scratch_off(int align) { return (uint32_t)(OFF_S + (pick(512 / align) * align)); }

static int emit_emulated(void) {
    int d = ymm(), s1 = ymm(), s2 = ymm(), g = gpr(), g1 = gpr(), g2 = gpr();
    switch (pick(20)) {
    case 0: vex(1, 0, s1, 1, 1, 0xFE, d, s2, 0, 0); break;                        // vpaddd ymm
    case 1: vex(1, 0, s1, 1, 1, 0xFB, d, s2, 0, 0); break;                        // vpsubq ymm
    case 2: vex(2, 0, s1, 1, 1, 0x40, d, s2, 0, 0); break;                        // vpmulld ymm
    case 3: vex(2, 0, s1, 1, 1, 0x47, d, s2, 0, 0); break;                        // vpsllvd ymm (helper)
    case 4: vex(2, 0, s1, 1, 1, 0x00, d, s2, 0, 0); break;                        // vpshufb ymm
    case 5: vex(1, 0, s1, 1, 1, 0x66, d, s2, 0, 0); break;                        // vpcmpgtd ymm
    case 6: vex(3, 1, 0, 1, 1, 0x00, d, s1, 0, 0); b(pick(256)); break;          // vpermq ymm, imm
    case 7: vex(2, 0, 0, 1, 1, 0x58, d, s1, 0, 0); break;                         // vpbroadcastd ymm, xmm
    case 8: vex(2, 1, s1, 1, 1, 0xB8, d, s2, 0, 0); break;                        // vfmadd231pd ymm
    case 9: vex(1, 0, s1, 1, 1, 0xFE, d, 0, 1, scratch_off(32)); break;           // vpaddd ymm, mem
    case 10: vex(2, 1, g2, 0, 1, 0xF7, g, g1, 0, 0); break;                       // shlx
    case 11: vex(2, 1, g2, 0, 2, 0xF7, g, g1, 0, 0); break;                       // sarx
    case 12: vex(3, 1, 0, 0, 3, 0xF0, g, g1, 0, 0); b(pick(64)); break;           // rorx
    case 13: vex(2, 1, g1, 0, 0, 0xF2, g, g2, 0, 0); break;                       // andn (CF ZF)
    case 14: vex(2, 1, g2, 0, 0, 0xF5, g, g1, 0, 0); break;                       // bzhi (CF ZF)
    case 15: b(0xF3); rex_rr(1, g, g1); b(0x0F); b(0xBD); b(0xC0 | (g & 7) << 3 | (g1 & 7)); break;   // lzcnt
    case 16: b(0xF3); rex_rr(1, g, g1); b(0x0F); b(0xBC); b(0xC0 | (g & 7) << 3 | (g1 & 7)); break;   // tzcnt
    case 17: if (g == g1) g1 = g == 0 ? 1 : 0;                                    // mulx: hi g, lo g1, src g2 * rdx
             vex(2, 1, g1, 0, 3, 0xF6, g, g2, 0, 0); break;
    case 18: vex(2, 1, g1, 0, 3, 0xF5, g, g2, 0, 0); break;                       // pdep (mcontext helper)
    case 19: vex(2, 1, g2, 0, 0, 0xF7, g, g1, 0, 0); break;                       // bextr (mcontext helper, ZF)
    }
    return 1;
}

// Returns 1 if it stored.
static int emit_native(void) {
    int g = gpr(), g1 = gpr(), y = ymm();
    switch (pick(10)) {
    case 0: alu_rr(0x01, g, g1); return 0;                                        // add (sets CF ZF)
    case 1: alu_rr(0x11, g, g1); return 0;                                        // adc (reads CF)
    case 2: alu_rr(0x31, g, g1); return 0;                                        // xor
    case 3: rex_rr(1, g, g1); b(0x0F); b(0x44); b(0xC0 | (g & 7) << 3 | (g1 & 7)); return 0;   // cmovz (reads ZF)
    case 4: mov_ld(g, scratch_off(8)); return 0;
    case 5: case 6: mov_st(g, scratch_off(8)); return 1;
    case 7: vmovdqu_st(y, scratch_off(32)); return 1;
    case 8: vex(1, 0, ymm(), 0, 1, 0xFE, y, ymm(), 0, 0); return 0;               // vpaddd xmm: AVX1, upper half zeroed
    default: rex_rr(1, 0, g); b(0x81); b(0xC0 | (g & 7)); d32((uint32_t)next()); return 0;   // add imm32
    }
}

typedef void (*fn_t)(state_t *);

// A store into this sequence's own code: the immediate of an `add $imm32, %g` emitted after it, followed at once by an
// emulated instruction. Natively the later add sees the new immediate.
static void emit_selfmod(void) {
    uint8_t *st = p;
    b(0xC7); b(0x05); d32(0); d32((uint32_t)next());   // movl $new, rel32(%rip), patched below
    uint8_t *after = p;
    emit_emulated();
    for (int k = (int)pick(6); k > 0; k--) emit_native();
    int g = gpr();
    rex_rr(1, 0, g); b(0x81); b(0xC0 | (g & 7));
    int32_t rel = (int32_t)(p - after);
    memcpy(st + 2, &rel, 4);
    d32((uint32_t)next());
}

static void generate(void) {
    p = code;
    memset(code, 0xCC, CODE_SIZE);
    static const int saved[] = {3, 5, 12, 13, 14, 15};
    for (int i = 0; i < 6; i++) { if (saved[i] & 8) b(0x41); b(0x50 + (saved[i] & 7)); }   // push
    for (int i = 0; i < 16; i++) vmovdqu_ld(i, OFF_Y(i));
    b(0xFF); b(0xB7); d32(OFF_F); b(0x9D);                      // push [rdi+flags]; popfq
    for (size_t i = 0; i < sizeof gprs / sizeof gprs[0]; i++) mov_ld(gprs[i], OFF_G(gprs[i]));
    int n = 300 + (int)pick(700);
    for (int i = 0; i < n && p < code + CODE_SIZE - 4096; i++) {
        unsigned k = pick(16);
        if (k < 7) emit_emulated();
        else if (k < 15) emit_native();
        else emit_selfmod();
    }
    b(0x9C); b(0x8F); b(0x87); d32(OFF_F);                      // pushfq; pop [rdi+flags]
    for (size_t i = 0; i < sizeof gprs / sizeof gprs[0]; i++) mov_st(gprs[i], OFF_G(gprs[i]));
    for (int i = 0; i < 16; i++) vmovdqu_st(i, OFF_Y(i));
    for (int i = 5; i >= 0; i--) { if (saved[i] & 8) b(0x41); b(0x58 + (saved[i] & 7)); }   // pop
    b(0xC5); b(0xF8); b(0x77);                                  // vzeroupper
    b(0xC3);
}

static uint64_t fnv(const void *v, size_t n, uint64_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)v)[i]) * 0x100000001B3ull;
    return h;
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: seq_rwx SEED COUNT\n"); return 2; }
    uint64_t seed = strtoull(argv[1], NULL, 0);
    int count = atoi(argv[2]);
    code = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 2; }
    static state_t s;
    uint64_t all = 0xCBF29CE484222325ull;
    for (int i = 0; i < count; i++) {
        rng = seed * 1000003u + (uint64_t)i;
        for (size_t k = 0; k < sizeof s; k++) ((uint8_t *)&s)[k] = (uint8_t)next();
        s.flags = 0x202 | (next() & 0x8D5);   // IF and the bit-1 constant, random arithmetic flags
        generate();
        ((fn_t)code)(&s);
        s.flags &= 0x41;                       // CF and ZF: every emulated flag writer here defines both or neither
        uint64_t h = fnv(&s, sizeof s, 0xCBF29CE484222325ull);
        printf("seq %d %016llx\n", i, (unsigned long long)h);
        all = fnv(&h, sizeof h, all);
    }
    printf("all %016llx\n", (unsigned long long)all);
    return 0;
}

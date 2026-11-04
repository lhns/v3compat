// flags_kept: the arithmetic flags set before an expansion must survive it, also when the next
// instruction is one DynamoRIO's table says writes flags but that writes none (count 0, rep with rcx 0).
// Each case: set flags, run the instruction under test, run the writer, read the flags back.
#include <stdint.h>
#include <stdio.h>

#define ARITH 0x8D5ul

// uint64_t f(void *buf): rdi = buf (64 valid bytes), rcx = 0 for the writers.
#define CASE(kind, insn, wname, writer)                                                     \
    __asm__(".text\n.globl k_" #kind "_" #wname "\nk_" #kind "_" #wname ":\n"                \
            "push %rbx\nmov %rdi, %rsi\nxor %ecx, %ecx\nxor %r8d, %r8d\n"                    \
            "pushq $0x8D5\npopfq\n" insn "\n" writer "\npushfq\npop %rax\npop %rbx\nret\n")

#define WRITERS(kind, insn)                          \
    CASE(kind, insn, shl, "shl $0, %r8");            \
    CASE(kind, insn, shr, "shr %cl, %r8");           \
    CASE(kind, insn, rol, "rol %cl, %r8");           \
    CASE(kind, insn, cmps, "repz cmpsb")

WRITERS(inline, "vpaddq (%rdi), %ymm1, %ymm2");
WRITERS(inline_shift, "vpsllq $3, %ymm1, %ymm2");
WRITERS(frame, "vpmaxsd (%rdi), %ymm1, %ymm2");
WRITERS(frame_gpr, "vpmovmskb %ymm1, %eax");
WRITERS(frame_fma, "vfmadd231ps (%rdi), %ymm1, %ymm2");
WRITERS(subst, "vpblendd $5, %ymm1, %ymm2, %ymm3");
WRITERS(subst_mem, "vpmaskmovd (%rdi), %ymm1, %ymm2");
WRITERS(movbe_store, "movbe %rax, (%rdi)");
WRITERS(movbe_load, "movbe (%rdi), %rax");
WRITERS(scalar, "rorx $3, %r9, %r10");

#define DECL(kind) \
    extern uint64_t k_##kind##_shl(void *), k_##kind##_shr(void *), k_##kind##_rol(void *), k_##kind##_cmps(void *);
DECL(inline) DECL(inline_shift) DECL(frame) DECL(frame_gpr) DECL(frame_fma) DECL(subst) DECL(subst_mem)
DECL(movbe_store) DECL(movbe_load) DECL(scalar)

typedef uint64_t (*fn)(void *);
#define ROW(kind) {#kind, {k_##kind##_shl, k_##kind##_shr, k_##kind##_rol, k_##kind##_cmps}}
static const struct {
    const char *kind;
    fn f[4];
} rows[] = {ROW(inline), ROW(inline_shift), ROW(frame), ROW(frame_gpr), ROW(frame_fma), ROW(subst),
            ROW(subst_mem), ROW(movbe_store), ROW(movbe_load), ROW(scalar)};
static const char *const writers[4] = {"shl $0", "shr %cl", "rol %cl", "repz cmpsb"};

int main(void) {
    static uint8_t buf[64] __attribute__((aligned(64)));
    int bad = 0;
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++)
        for (int w = 0; w < 4; w++) {
            uint64_t fl = rows[r].f[w](buf);
            if ((fl & ARITH) != ARITH) {
                printf("FAIL %s then %s: flags %llx\n", rows[r].kind, writers[w], (unsigned long long)fl);
                bad = 1;
            }
        }
    puts(bad ? "FAIL" : "PASS");
    return bad;
}

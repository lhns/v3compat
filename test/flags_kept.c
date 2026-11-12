// flags_kept: the arithmetic flags set before an expansion must survive it, also when the next
// instruction is one DynamoRIO's table says writes flags but that writes none (count 0, rep with rcx 0).
// Each case: set flags, run the instruction under test, run the writer, read the flags back. The flags
// start all set and all clear, so an expansion that clobbers them cannot pass by chance. The "none"
// writer reads them straight after the instruction.
#include <stdint.h>
#include <stdio.h>

#define ARITH 0x8D5ul

// uint64_t f(void *buf, uint64_t flags): rdi = buf (64 valid bytes), rsi = the flags to start with,
// rcx = 0 for the writers.
#define CASE(kind, insn, wname, writer)                                                     \
    __asm__(".text\n.globl k_" #kind "_" #wname "\nk_" #kind "_" #wname ":\n"                \
            "push %rbx\npush %rsi\nmov %rdi, %rsi\nxor %ecx, %ecx\nxor %r8d, %r8d\n"         \
            "popfq\n" insn "\n" writer "\npushfq\npop %rax\npop %rbx\nret\n")

#define WRITERS(kind, insn)                          \
    CASE(kind, insn, shl, "shl $0, %r8");            \
    CASE(kind, insn, shr, "shr %cl, %r8");           \
    CASE(kind, insn, rol, "rol %cl, %r8");           \
    CASE(kind, insn, cmps, "repz cmpsb");            \
    CASE(kind, insn, none, "")

WRITERS(inline, "vpaddq (%rdi), %ymm1, %ymm2");
WRITERS(inline_shift, "vpsllq $3, %ymm1, %ymm2");
WRITERS(frame, "vpmaxsd (%rdi), %ymm1, %ymm2");
WRITERS(frame_gpr, "vpmovmskb %ymm1, %eax");
WRITERS(frame_fma, "vfmadd231ps (%rdi), %ymm1, %ymm2");
WRITERS(subst, "vpblendd $5, %ymm1, %ymm2, %ymm3");
WRITERS(subst_mem, "vpmaskmovd (%rdi), %ymm1, %ymm2");
WRITERS(movbe_store, "movbe %rax, (%rdi)");
WRITERS(movbe_load, "movbe (%rdi), %rax");
WRITERS(movbe_load16, "movbe (%rdi), %bx");
WRITERS(movbe_load32, "movbe (%rdi), %r9d");
WRITERS(scalar, "rorx $3, %r9, %r10");
WRITERS(rorx_mem, "rorx $29, (%rdi), %r10");
WRITERS(rorx32, "rorx $5, %r9d, %r10d");
WRITERS(mulx, "mulx %r9, %r10, %r11");
WRITERS(mulx_rax, "mulx %rax, %rax, %rdx");
WRITERS(mulx32_mem, "mulx (%rdi), %r10d, %r11d");
WRITERS(inline_cmp, "vpcmpeqd (%rdi), %ymm1, %ymm2");
WRITERS(inline_shift_d, "vpsrld $3, %ymm1, %ymm2");
WRITERS(widen_mem, "vpmovzxbq (%rdi), %ymm2");
WRITERS(widen_reg, "vpmovsxdq %xmm1, %ymm2");
WRITERS(bcast_mem, "vpbroadcastb (%rdi), %ymm2");
WRITERS(bcast_reg, "vpbroadcastq %xmm1, %ymm2");
WRITERS(abs_mem, "vpabsd (%rdi), %ymm2");
WRITERS(logic_mem, "vpand (%rdi), %ymm1, %ymm2");
WRITERS(shlx, "shlx %r9, %r10, %r11");
WRITERS(shrx_mem, "shrx %rcx, (%rdi), %r10");
WRITERS(sarx32, "sarx %eax, %r9d, %r10d");
WRITERS(shlx_rax, "shlx %rax, %rcx, %rax");

#define DECL(kind)                                                                               \
    extern uint64_t k_##kind##_shl(void *, uint64_t), k_##kind##_shr(void *, uint64_t),          \
        k_##kind##_rol(void *, uint64_t), k_##kind##_cmps(void *, uint64_t), k_##kind##_none(void *, uint64_t);
DECL(inline) DECL(inline_shift) DECL(frame) DECL(frame_gpr) DECL(frame_fma) DECL(subst) DECL(subst_mem)
DECL(movbe_store) DECL(movbe_load) DECL(movbe_load16) DECL(movbe_load32) DECL(scalar) DECL(rorx_mem)
DECL(rorx32) DECL(mulx) DECL(mulx_rax) DECL(mulx32_mem)
DECL(inline_cmp) DECL(inline_shift_d) DECL(widen_mem) DECL(widen_reg) DECL(bcast_mem) DECL(bcast_reg) DECL(abs_mem) DECL(logic_mem) DECL(shlx) DECL(shrx_mem) DECL(sarx32) DECL(shlx_rax)

typedef uint64_t (*fn)(void *, uint64_t);
#define ROW(kind) {#kind, {k_##kind##_shl, k_##kind##_shr, k_##kind##_rol, k_##kind##_cmps, k_##kind##_none}}
static const struct {
    const char *kind;
    fn f[5];
} rows[] = {ROW(inline), ROW(inline_shift), ROW(frame), ROW(frame_gpr), ROW(frame_fma), ROW(subst),
            ROW(subst_mem), ROW(movbe_store), ROW(movbe_load), ROW(movbe_load16), ROW(movbe_load32), ROW(scalar),
            ROW(rorx_mem), ROW(rorx32), ROW(mulx), ROW(mulx_rax), ROW(mulx32_mem),
            ROW(inline_cmp), ROW(inline_shift_d), ROW(widen_mem), ROW(widen_reg), ROW(bcast_mem), ROW(bcast_reg), ROW(abs_mem), ROW(logic_mem), ROW(shlx), ROW(shrx_mem), ROW(sarx32), ROW(shlx_rax)};
static const char *const writers[5] = {"shl $0", "shr %cl", "rol %cl", "repz cmpsb", "none"};

int main(void) {
    static uint8_t buf[64] __attribute__((aligned(64)));
    int bad = 0;
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++)
        for (int w = 0; w < 5; w++)
            for (int k = 0; k < 2; k++) {
                uint64_t init = k ? ARITH : 0, fl = rows[r].f[w](buf, init | 2);
                if ((fl & ARITH) != init) {
                    printf("FAIL %s then %s from %llx: flags %llx\n", rows[r].kind, writers[w],
                           (unsigned long long)init, (unsigned long long)fl);
                    bad = 1;
                }
            }
    puts(bad ? "FAIL" : "PASS");
    return bad;
}

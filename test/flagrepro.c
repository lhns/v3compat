// flagrepro: arithmetic flags and results DR must preserve across an indirect-branch entry.
// Every case is checked against what the architecture defines, so native must print PASS.
//   static   instructions that write flags only conditionally (count or rcx is 0)
//   fault    a faulting load before a flag writer: flags the handler sees in its ucontext
//   smc      code at one address rewritten from a flag writer to one that is not (DR sandboxes
//            the page after -ro2sandbox_threshold write faults)
//   selfwr   a write in the block overwrites a later flag writer of the same block
//   movbe    66 REX.W movbe with a rip-relative operand out of DR's reach (is 64-bit)
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

#define ARITH 0x8D5ul
#define ITERS 300

// run(target, flags, rcx, rdx): enter target by indirect jump with those flags; returns flags.
__asm__(".text\n"
        "run:\n"
        "  push %rbx\n"
        "  push %rsi\n"
        "  push %rdi\n"
        "  mov %rdi, run_tgt(%rip)\n"
        "  mov %rcx, %rax\n"
        "  mov %rdx, %rcx\n"
        "  mov %rax, %rdx\n"
        "  mov %rsp, run_rsp(%rip)\n"
        "  lea scratch_a(%rip), %rsi\n"
        "  lea scratch_b(%rip), %rdi\n"
        "  mov $0x1234, %rbx\n"
        "  push %r8\n"
        "  push %r8\n"
        "  pop %rax\n"
        "  push %rax\n"
        "  popfq\n"
        "  lea 8(%rsp), %rsp\n"
        "  jmp *run_tgt(%rip)\n"
        ".globl run_after\n"
        "run_after:\n"
        "  pushfq\n"
        "  pop %rax\n"
        "  mov run_rsp(%rip), %rsp\n"
        "  pop %rdi\n"
        "  pop %rsi\n"
        "  pop %rbx\n"
        "  ret\n"
        ".macro CASE name, body:vararg\n"
        "  .globl \\name\n"
        "\\name:\n"
        "  \\body\n"
        "  jmp *run_after_p(%rip)\n"
        ".endm\n"
        "CASE c_control, mov %rbx, %rbx\n"
        "CASE c_shl, shl %cl, %rbx\n"
        "CASE c_shr, shr %cl, %rbx\n"
        "CASE c_sar, sar %cl, %rbx\n"
        "CASE c_rol, rol %cl, %rbx\n"
        "CASE c_ror, ror %cl, %rbx\n"
        "CASE c_rcl, rcl %cl, %rbx\n"
        "CASE c_rcr, rcr %cl, %rbx\n"
        "CASE c_shld, shld %cl, %rax, %rbx\n"
        "CASE c_shrd, shrd %cl, %rax, %rbx\n"
        "CASE c_shl0, .byte 0x48, 0xc1, 0xe3, 0x00\n"
        "CASE c_rol0, .byte 0x48, 0xc1, 0xc3, 0x00\n"
        "CASE c_cmps, repe cmpsb\n"
        "CASE c_scas, repne scasb\n"
        ".globl c_fault\n"
        ".globl c_fault_ok\n"
        "c_fault:\n"
        "  mov (%rdx), %rbx\n"
        "c_fault_ok:\n"
        "  add %rbx, %rbx\n"
        "  jmp *run_after_p(%rip)\n"
        ".data\n"
        "run_tgt: .quad 0\n"
        "run_rsp: .quad 0\n"
        "run_after_p: .quad run_after\n"
        "scratch_a: .quad 1\n"
        "scratch_b: .quad 2\n"
        ".text\n");
extern uint64_t run(void *tgt, uint64_t flags, uint64_t rcx, uint64_t rdx, uint64_t flags2);
extern char run_after[], c_control[], c_shl[], c_shr[], c_sar[], c_rol[], c_ror[], c_rcl[], c_rcr[],
    c_shld[], c_shrd[], c_shl0[], c_rol0[], c_cmps[], c_scas[], c_fault[];

static const uint64_t INPUTS[] = {0x202, 0xA02, 0x8D5 | 0x202, 0x246, 0x203, 0xA96};
#define NIN (sizeof INPUTS / sizeof INPUTS[0])
static int failed;

static void report(const char *name, int fails, int total, uint64_t in, uint64_t out) {
    printf("%-22s fails=%d/%d", name, fails, total);
    if (fails)
        printf(" first in=%llx out=%llx xor=%llx", (unsigned long long)in, (unsigned long long)out,
               (unsigned long long)((in ^ out) & ARITH));
    printf("\n");
    if (fails)
        failed = 1;
}

static uint64_t enter(void *tgt, uint64_t flags, uint64_t rcx, uint64_t rdx) {
    return run(tgt, flags, rcx, rdx, flags);
}

static void static_case(const char *name, void *tgt) {
    int fails = 0, total = 0;
    uint64_t fi = 0, fo = 0;
    for (int it = 0; it < ITERS; it++)
        for (unsigned i = 0; i < NIN; i++) {
            uint64_t out = enter(tgt, INPUTS[i], 0, 0);
            total++;
            if ((out ^ INPUTS[i]) & ARITH) {
                if (!fails) {
                    fi = INPUTS[i];
                    fo = out;
                }
                fails++;
            }
        }
    report(name, fails, total, fi, fo);
}

static sigjmp_buf jb;
static uint64_t fault_flags;
static void on_segv(int s, siginfo_t *si, void *ucv) {
    (void)s;
    (void)si;
    fault_flags = ((ucontext_t *)ucv)->uc_mcontext.gregs[REG_EFL];
    siglongjmp(jb, 1);
}
static void fault_case(void) {
    int fails = 0, total = 0;
    uint64_t fi = 0, fo = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0);
    for (int it = 0; it < ITERS; it++)
        for (unsigned i = 0; i < NIN; i++) {
            if (!sigsetjmp(jb, 1))
                enter(c_fault, INPUTS[i], 0, 0);
            total++;
            if ((fault_flags ^ INPUTS[i]) & ARITH) {
                if (!fails) {
                    fi = INPUTS[i];
                    fo = fault_flags;
                }
                fails++;
            }
        }
    signal(SIGSEGV, SIG_DFL);
    report("fault-ucontext", fails, total, fi, fo);
}

static uint8_t *page;
// jmp *0(%rip) back to run_after, after len bytes of code.
static void emit(const uint8_t *code, size_t len) {
    uintptr_t after = (uintptr_t)run_after;
    memcpy(page, code, len);
    page[len] = 0xFF;
    page[len + 1] = 0x25;
    memset(page + len + 2, 0, 4);
    memcpy(page + len + 6, &after, 8);
}

// Runs writer then keeper at one address: flags after the keeper must equal the input.
static void smc_case(const char *name, const uint8_t *wr, size_t wlen) {
    static const uint8_t keeper[] = {0x48, 0x89, 0xdb}; // mov %rbx,%rbx
    int fails = 0, total = 0;
    uint64_t fi = 0, fo = 0;
    for (int it = 0; it < ITERS; it++)
        for (unsigned i = 0; i < NIN; i++) {
            emit(wr, wlen);
            enter(page, INPUTS[i], 0, 0);
            emit(keeper, sizeof keeper);
            uint64_t out = enter(page, INPUTS[i], 0, 0);
            total++;
            if ((out ^ INPUTS[i]) & ARITH) {
                if (!fails) {
                    fi = INPUTS[i];
                    fo = out;
                }
                fails++;
            }
        }
    report(name, fails, total, fi, fo);
}

// movl %eax,(%rdx) overwrites the add that follows it with nops; eax is set via rcx.
static void selfwrite_case(void) {
    // mov %ecx,(%rdx); add %rbx,%rbx; nop
    static const uint8_t body[] = {0x89, 0x0a, 0x48, 0x01, 0xdb, 0x90};
    int fails = 0, total = 0;
    uint64_t fi = 0, fo = 0;
    for (int it = 0; it < ITERS; it++)
        for (unsigned i = 0; i < NIN; i++) {
            emit(body, sizeof body);
            uint64_t out = enter(page, INPUTS[i], 0x90909090, (uint64_t)(page + 2));
            total++;
            if ((out ^ INPUTS[i]) & ARITH) {
                if (!fails) {
                    fi = INPUTS[i];
                    fo = out;
                }
                fails++;
            }
        }
    report("selfwrite-next-instr", fails, total, fi, fo);
}

// Forms run from a low page so the rip-relative target is out of reach of DR's code cache.
static void movbe_case(void) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!((c >> 22) & 1)) {
        printf("movbe                  skipped (no MOVBE)\n");
        return;
    }
    // movbe 0x1000(%rip),%rax; mov %rax,0x1010(%rip): data on another page than the code.
    static const uint8_t code[] = {0x66, 0x48, 0x0f, 0x38, 0xf0, 0x05, 0x00, 0x10, 0x00, 0x00,
                                   0x48, 0x89, 0x05, 0x10, 0x10, 0x00, 0x00};
    uint64_t *src = (uint64_t *)(page + 10 + 0x1000), *dst = (uint64_t *)(page + 17 + 0x1010);
    const uint64_t want = 0x8877665544332211ull;
    uint64_t got = 0;
    int fails = 0, total = 0;
    emit(code, sizeof code);
    for (int it = 0; it < ITERS; it++) {
        *src = 0x1122334455667788ull;
        *dst = 0xdeadbeefdeadbeefull;
        enter(page, 0x202, 0, 0);
        total++;
        if (*dst != want) {
            if (!fails)
                got = *dst;
            fails++;
        }
    }
    printf("movbe-66-rexw-rip      fails=%d/%d", fails, total);
    if (fails) {
        printf(" got=%llx want=%llx", (unsigned long long)got, (unsigned long long)want);
        failed = 1;
    }
    printf("\n");
}

int main(void) {
    page = mmap((void *)0x30000000, 4 * 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (page == MAP_FAILED)
        return 2;
    static_case("control-mov", c_control);
    static_case("shl-cl0", c_shl);
    static_case("shr-cl0", c_shr);
    static_case("sar-cl0", c_sar);
    static_case("rol-cl0", c_rol);
    static_case("ror-cl0", c_ror);
    static_case("rcl-cl0", c_rcl);
    static_case("rcr-cl0", c_rcr);
    static_case("shld-cl0", c_shld);
    static_case("shrd-cl0", c_shrd);
    static_case("shl-imm0", c_shl0);
    static_case("rol-imm0", c_rol0);
    static_case("repe-cmpsb-rcx0", c_cmps);
    static_case("repne-scasb-rcx0", c_scas);
    fault_case();
    static const uint8_t add6[] = {0x48, 0x01, 0xdb};              // add %rbx,%rbx: writes all 6
    static const uint8_t ror1[] = {0x48, 0xc1, 0xcb, 0x01};        // ror $1,%rbx: writes CF, OF
    static const uint8_t tz[] = {0xf3, 0x48, 0x0f, 0xbc, 0xdb};    // tzcnt %rbx,%rbx
    smc_case("smc-add-to-mov", add6, sizeof add6);
    smc_case("smc-ror-to-mov", ror1, sizeof ror1);
    smc_case("smc-tzcnt-to-mov", tz, sizeof tz);
    selfwrite_case();
    movbe_case();
    printf("%s\n", failed ? "FAIL" : "PASS");
    return failed;
}

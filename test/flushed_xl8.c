/* flushed_xl8 MODE SECONDS: a fault in a block that was flushed while the faulting thread was still inside it, where
 * rebuilding the block from the application's code no longer gives the code that was emitted.
 *
 * Each round, a fresh block runs a long `rep stosb`; while it does, a writer changes something, and after the store
 * the block loads from an unreadable page. The handler checks the pc of that fault.
 *   rewrite  the writer overwrites the block's first two instructions, already run, with a 5-byte jmp (as a JIT
 *            patches a method it retires): the block is flushed, and rebuilding it decodes the jmp.
 *   span     the block starts on page A (r-x) and falls through into page B (rwx), where it faults. The writer
 *            writes B and runs code there until B passes -ro2sandbox_threshold and comes back sandboxed: a rebuild
 *            from A then stops at the page boundary.
 *   module   as rewrite, in the executable's own text (a module image, where the client does not store translations
 *            and the block is rebuilt): one page per round, between pages that stay r-x so that no page passes the
 *            threshold. A rebuild that cannot translate the fault stops the process instead of resuming at pc 0.
 *   rewrite-same, span-below, module-same  controls: the same bytes written back, or B written fewer times than the
 *            threshold.
 * A round counts as checked when the writer finished before the store did. Prints "ok" and exits 0 when every fault
 * was at the right pc and some rounds were checked. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

enum { LEN = 128 << 20, POLL = 1 << 16, PG = 4096, SPAN_ROUNDS = 14, BELOW_ROUNDS = 3, MOD_PAGES = 64 };

/* module: MOD_PAGES copies of the rewrite block, each on its own page followed by a page never made writable. */
__asm__(".pushsection .text.flushed_xl8_module,\"ax\",@progbits\n.balign 4096\nmodule_pages:\n.rept 64\n"
        ".byte 0x48,0x83,0xc1,0x00,0x90,0x48,0x89,0xc8,0x48,0x89,0xf1,0xf3,0xaa\n"
        ".rept 4\n.byte 0x48,0x83,0xc0,0x01\n.endr\n.byte 0x8b,0x02,0xc3\n"
        ".balign 64,0xcc\n.byte 0xc3\n.balign 4096,0xcc\n.skip 4096,0xcc\n.endr\n.popsection");
extern unsigned char module_pages[];

typedef void (*block_fn)(unsigned char *buf, long len, void *bad, long v);

static unsigned char *buf, *bad_page;
static unsigned char *volatile expect_pc;
static unsigned char *code;   /* rewrite: the page; span: page B */
static atomic_long go, done;
static atomic_ulong faults;
static int mode_span, mode_module, control;

static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    ucontext_t *uc = ctx;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    if (pc != (uintptr_t)expect_pc || si->si_addr != bad_page) {
        char m[160];
        int n = snprintf(m, sizeof m, "FAIL: fault at pc %#lx addr %p, expected pc %p after %lu faults\n",
                         (unsigned long)pc, si->si_addr, (void *)expect_pc, atomic_load(&faults));
        write(1, m, n);
        _exit(1);
    }
    uc->uc_mcontext.gregs[REG_RIP] = pc + 2;   /* skip the load */
    atomic_fetch_add(&faults, 1);
}

static unsigned char *put(unsigned char *p, const char *bytes, size_t n) { memcpy(p, bytes, n); return p + n; }

/* rewrite: add rcx,0; nop; mov rax,rcx; mov rcx,rsi; rep stosb; 4x add rax,1; mov eax,[rdx]; ret. A ret at 64 is
 * the patched jmp's target. Returns the entry. */
static unsigned char *make_rewrite(void) {
    unsigned char *p = code = mmap(0, PG, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) exit(2);
    p = put(p, "\x48\x83\xc1\x00\x90", 5);
    p = put(p, "\x48\x89\xc8\x48\x89\xf1\xf3\xaa", 8);
    for (int i = 0; i < 4; i++) p = put(p, "\x48\x83\xc0\x01", 4);
    expect_pc = p;
    p = put(p, "\x8b\x02\xc3", 3);
    code[64] = 0xc3;
    return code;
}

/* module: round R's page of module_pages, made writable; the body is make_rewrite's. */
static unsigned char *make_module(long r) {
    code = module_pages + (r - 1) * 2 * PG;
    if (mprotect(code, PG, PROT_READ | PROT_WRITE | PROT_EXEC)) { perror("mprotect"); exit(2); }
    expect_pc = code + 29;
    return code;
}

/* span: the same body from 16 bytes before the end of A, the load 8 bytes into B; a ret at B+2048 to run B, data
 * at B+3072. A is r-x, so only B is written and only B can switch to sandboxing. */
static unsigned char *make_span(void) {
    unsigned char *a = mmap(0, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED) exit(2);
    unsigned char *b = code = a + PG, *p = b - 16;
    p = put(p, "\x48\x89\xc8\x48\x89\xf1\xf3\xaa", 8);
    for (int i = 0; i < 4; i++) p = put(p, "\x48\x83\xc0\x01", 4);
    expect_pc = p;
    p = put(p, "\x8b\x02\xc3", 3);
    b[2048] = 0xc3;
    if (mprotect(a, PG, PROT_READ | PROT_EXEC) || mprotect(b, PG, PROT_READ | PROT_WRITE | PROT_EXEC)) exit(2);
    return b - 16;
}

static void *writer(void *arg) {
    unsigned long *checked = arg;
    for (long r = 1;; r++) {
        while (atomic_load(&go) < r) if (atomic_load(&go) < 0) return 0;
        unsigned char v = (unsigned char)(r % 255 + 1);
        while (((volatile unsigned char *)buf)[POLL] != v) {}
        if (!mode_span) {   /* jmp +59 to the ret at 64, or the original bytes */
            memcpy(code, control ? "\x48\x83\xc1\x00\x90" : "\xe9\x3b\x00\x00\x00", 5);
        } else {
            for (int i = 0; i < (control ? BELOW_ROUNDS : SPAN_ROUNDS); i++) {
                ((volatile unsigned char *)code)[3072]++;
                ((void (*)(void))(code + 2048))();
            }
        }
        if (((volatile unsigned char *)buf)[LEN - 1] != v) (*checked)++;
        atomic_store(&done, r);
    }
}

int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "";
    int secs = argc > 2 ? atoi(argv[2]) : 5;
    if (!strcmp(m, "rewrite") || !strcmp(m, "rewrite-same")) mode_span = 0;
    else if (!strcmp(m, "span") || !strcmp(m, "span-below")) mode_span = 1;
    else if (!strcmp(m, "module") || !strcmp(m, "module-same")) mode_module = 1;
    else {
        fprintf(stderr, "usage: flushed_xl8 rewrite|rewrite-same|span|span-below|module|module-same [SECONDS]\n");
        return 2;
    }
    control = strchr(m, '-') != NULL;
    buf = mmap(0, LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    bad_page = mmap(0, PG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED || bad_page == MAP_FAILED) return 2;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    unsigned long checked = 0;
    pthread_t w;
    pthread_create(&w, 0, writer, &checked);
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long r = 0;
    do {
        r++;
        block_fn f = (block_fn)(mode_module ? make_module(r) : mode_span ? make_span() : make_rewrite());
        atomic_store(&go, r);
        f(buf, LEN, bad_page, r % 255 + 1);
        while (atomic_load(&done) < r) {}
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while (t.tv_sec - t0.tv_sec < secs && !(mode_module && r == MOD_PAGES));
    atomic_store(&go, -1);
    pthread_join(w, 0);
    /* A late writer lets the block finish first, or a rewrite land before the block was built: no fault then. */
    if (!checked) {
        printf("FAIL: nothing checked, rounds=%ld faults=%lu\n", r, atomic_load(&faults));
        return 1;
    }
    printf("ok rounds=%ld checked=%lu faults=%lu\n", r, checked, atomic_load(&faults));
    return 0;
}

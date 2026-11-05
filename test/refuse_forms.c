// refuse_forms NAME: runs one instruction form by its bytes. Assemblers would refuse some, or pick another encoding.
// Off the audited list a form must exit 86 under the client, however it behaves on this CPU; listed ones must run.
#include <cpuid.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static uint64_t mem[64] __attribute__((aligned(64)));

#define FORM(name, ...) {name, (const unsigned char[]){__VA_ARGS__}, sizeof((const unsigned char[]){__VA_ARGS__})}
static const struct form { const char *name; const unsigned char *b; size_t n; } forms[] = {
    /* Refused: [rbx] is mem, rcx and rax hold plain values. */
    FORM("xop-bextr-imm", 0x8f, 0xea, 0x78, 0x10, 0xc3, 0x04, 0x08, 0x00, 0x00), /* TBM: must not run with control 0 */
    FORM("vaesenc-ymm", 0xc4, 0xe2, 0x75, 0xdc, 0xc2),
    FORM("vpclmulqdq-ymm", 0xc4, 0xe3, 0x75, 0x44, 0xc2, 0x00),
    FORM("rdgsbase", 0xf3, 0x48, 0x0f, 0xae, 0xc8),
    FORM("wrgsbase", 0xf3, 0x48, 0x0f, 0xae, 0xd8),
    FORM("rdfsbase", 0xf3, 0x48, 0x0f, 0xae, 0xc0),
    FORM("tpause", 0x66, 0x0f, 0xae, 0xf1),
    FORM("umonitor", 0xf3, 0x0f, 0xae, 0xf1),
    FORM("adcx", 0x66, 0x48, 0x0f, 0x38, 0xf6, 0xc1),
    FORM("adox", 0xf3, 0x48, 0x0f, 0x38, 0xf6, 0xc1),
    FORM("sha1rnds4", 0x0f, 0x3a, 0xcc, 0xc1, 0x00),
    FORM("clflushopt", 0x66, 0x0f, 0xae, 0x3b),
    FORM("clwb", 0x66, 0x0f, 0xae, 0x33),
    FORM("rdpid", 0xf3, 0x0f, 0xc7, 0xf9),
    FORM("rdseed", 0x48, 0x0f, 0xc7, 0xf9),
    FORM("xsavec", 0x0f, 0xc7, 0x23),
    FORM("movdiri", 0x0f, 0x38, 0xf9, 0x0b),
    FORM("gf2p8mulb", 0x66, 0x0f, 0x38, 0xcf, 0xc1),
    FORM("kmovw", 0xc5, 0xf8, 0x90, 0xc1),
    FORM("vzeroupper-w1", 0xc4, 0xe1, 0xf8, 0x77),  /* #UD on Ivy Bridge, though XED accepts it */
    FORM("vmmcall", 0x0f, 0x01, 0xd9),              /* runs on a hypervisor's Ivy Bridge instead of faulting */
    FORM("cmpccxadd", 0xc4, 0xe2, 0x71, 0xe0, 0x0b), /* DR has no decoding: the client sees only the SIGILL */
    FORM("jz-66", 0x66, 0x74, 0x00),                /* Intel ignores the 66, AMD truncates RIP to 16 bits */
    FORM("pause-rexb", 0xf3, 0x41, 0x90),           /* CPUs disagree whether it is xchg r8, rax */
    /* Run natively, listed. */
    FORM("pause", 0xf3, 0x90),
    FORM("rep-ret-next", 0xf3, 0xc3),
    FORM("endbr64", 0xf3, 0x0f, 0x1e, 0xfa),
    FORM("prefetchw", 0x0f, 0x0d, 0x0b),
    FORM("popcnt", 0xf3, 0x48, 0x0f, 0xb8, 0xc1),
    FORM("crc32", 0xf2, 0x48, 0x0f, 0x38, 0xf1, 0xc1),
    FORM("aesenc", 0x66, 0x0f, 0x38, 0xdc, 0xc1),
    FORM("vcvtph2ps", 0xc4, 0xe2, 0x79, 0x13, 0xc1),
    FORM("rdrand", 0x48, 0x0f, 0xc7, 0xf1),
};

/* Called, so that every form returns through the ret after it, and `rep ret` through itself. */
__attribute__((noinline)) static void run(const struct form *f) {
    static unsigned char page[4096] __attribute__((aligned(4096)));
    memcpy(page, f->b, f->n);
    page[f->n] = 0xc3;
    mprotect(page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    uint64_t rax = 1, rcx = 2;
    __asm__ volatile("call *%2" : "+a"(rax), "+c"(rcx) : "r"(page), "b"(mem) : "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "cc", "memory");
}

int main(int argc, char **argv) {
    const char *n = argc > 1 ? argv[1] : "";
    if (!strcmp(n, "cpuid-fsgsbase")) {
        unsigned a, b, c, d;
        __cpuid_count(7, 0, a, b, c, d);
        printf("fsgsbase=%u\n", b & 1);
        return b & 1;
    }
    if (!strcmp(n, "cpuid-xsavec")) {
        unsigned a, b, c, d;
        __cpuid_count(0xd, 1, a, b, c, d);
        printf("xsave-extras=%u\n", (a >> 1) & 7);
        return (a >> 1) & 7;
    }
    if (!strcmp(n, "not-executed")) {
        /* A refused form behind a branch always taken: the program must not notice it. */
        int z = 1;
        __asm__ volatile("test %0, %0\njnz 1f\n.byte 0x66, 0x0f, 0xae, 0xf1\n1:\n" : : "r"(z));
        puts("PASS");
        return 0;
    }
    for (size_t i = 0; i < sizeof forms / sizeof *forms; i++)
        if (!strcmp(n, forms[i].name)) {
            run(&forms[i]);
            puts("PASS");
            return 0;
        }
    return 2;
}

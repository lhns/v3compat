/*
 * probe.c: runs every row of the sweep on this CPU from fixed seed states and prints, per row,
 * which seeds faulted and a digest of all registers, flags and memory afterwards. No XED, no DR:
 * this is what runs on each machine being compared. Usage: probe [-r | -s N | -f] < rows.tsv > results.tsv
 * Rows run in order, reversed (-r), shuffled by N (-s) or each in its own process (-f); the output is in row
 * order whichever: nothing a row leaves may reach the next, and `gen.py sweep` holds it to that.
 * `probe -v HEX [UNDEFMASK]` prints the state per seed, to look into a mismatch.
 */
#define _GNU_SOURCE
#include <cpuid.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
#include "layout.h"

#define SEEDS 5
#define XS_SIZE 4096
#define XS_ALIGN __attribute__((aligned(64)))
extern void run_slot(void), p_capture(void), p_recover(void);
uint64_t p_regs[16], p_seed_regs[16], p_flags, p_seed_flags, p_host_rsp, p_code = P_CODE;
XS_ALIGN uint8_t p_xs[XS_SIZE];
XS_ALIGN static uint8_t seed_xs[SEEDS][XS_SIZE], host_xs[XS_SIZE];
uint8_t *p_seed_xs, *p_host_xs = host_xs;
static uint8_t seed_mem[SEEDS][P_DATASZ];
static uint64_t seed_regs[SEEDS][16], seed_flags[SEEDS];
static volatile int got_sig, got_code;

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI };
enum { CF = 1, PF = 4, AF = 0x10, ZF = 0x40, SF = 0x80, OF = 0x800 };

static void on_fault(int s, siginfo_t *i, void *u) {
    ucontext_t *c = u;
    got_sig = s;
    got_code = i->si_code;
    c->uc_mcontext.gregs[REG_RIP] = (greg_t)p_recover;
    c->uc_mcontext.gregs[REG_EFL] = 0x202;
}

static uint64_t rng_s = 0x9e3779b97f4a7c15;
static uint64_t rng(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return rng_s; }

/* Seeds: zeros; ones with all flags; FP edge values with x87 loaded; pointers to the landing pad,
 * so indirect branches arrive; random. */
static void build_seeds(void) {
    __asm__ volatile("xsave %0" : "=m"(host_xs) : "a"(7), "d"(0) : "memory");
    static const uint32_t fl[8] = {0x3fc00000, 0xc0100000, 0x000116c2, 0x7f800000, 0x7fc00000, 0x7f800001, 0, 0x80000000};
    static const uint64_t db[4] = {0x3ff8000000000000, 0x7ff8000000000000, 0x7ff0000000000001, 0x0000000000000001};
    for (int s = 0; s < SEEDS; s++) {
        memcpy(seed_xs[s], host_xs, XS_SIZE);
        uint8_t *x = seed_xs[s];
        memset(x + 512, 0, 64);
        x[512] = 7;                 /* XSTATE_BV: x87, SSE, AVX */
        memset(x, 0, 160);
        x[0] = 0x7f; x[1] = 0x03;   /* fcw */
        x[24] = 0x80; x[25] = 0x1f; /* mxcsr */
        uint64_t *r = seed_regs[s];
        memset(r, 0, sizeof seed_regs[s]);
        seed_flags[s] = 0x202;
        for (int v = 0; v < 16; v++) { /* xmm at 160, ymm upper halves at 576 */
            uint64_t q[4] = {0, 0, 0, 0};
            uint32_t d[8];
            switch (s) {
            case 0: break;
            case 1: q[0] = q[1] = q[2] = q[3] = ~0ULL; break;
            case 2:
                if (v & 1) for (int k = 0; k < 4; k++) q[k] = db[(k + v / 2) & 3];
                else {
                    for (int k = 0; k < 8; k++) d[k] = fl[(k + v / 2) & 7];
                    memcpy(q, d, 32);
                }
                break;
            case 3: q[0] = P_LAND; q[1] = 0x8000000000000000; q[2] = 1; q[3] = 0x7fffffff; break;
            default: for (int k = 0; k < 4; k++) q[k] = rng(); break;
            }
            memcpy(x + 160 + v * 16, q, 16);
            memcpy(x + 576 + v * 16, q + 2, 16);
        }
        if (s == 2) {                  /* st0-7 = 1..8, all tags valid */
            x[4] = 0xff;
            for (int k = 0; k < 8; k++) {
                int e = (k >= 1) + (k >= 3) + (k >= 7);
                uint64_t mant = (uint64_t)(k + 1) << (63 - e);
                uint16_t ex = (uint16_t)(16383 + e);
                memcpy(x + 32 + 16 * k, &mant, 8);
                memcpy(x + 40 + 16 * k, &ex, 2);
            }
        }
        for (size_t o = 0; o < P_DATASZ; o += 8) {
            uint64_t m = s == 0 ? 0 : s == 1 ? ~0ULL : s == 2 ? db[(o / 8) & 3] : s == 3 ? P_LAND : rng();
            memcpy(seed_mem[s] + o, &m, 8);
        }
        switch (s) {
        case 1:
            for (int i = 0; i < 16; i++) r[i] = ~0ULL;
            r[RCX] = 0x21;
            seed_flags[s] |= CF | PF | AF | ZF | SF | OF;
            break;
        case 2:
            for (int i = 0; i < 16; i++) r[i] = 0x1234 + 0x101 * i;
            r[RAX] = 0x123456789abcdef0; r[RCX] = 5; r[RDX] = 0;
            seed_flags[s] |= ZF;
            break;
        case 3:
            for (int i = 0; i < 16; i++) r[i] = P_LAND;
            break;
        case 4:
            for (int i = 0; i < 16; i++) r[i] = rng();
            r[RCX] = 3; r[RDX] = 1;
            seed_flags[s] |= CF | OF;
            break;
        default: break;
        }
        if (s == 0) r[RDX] = 0x100; /* dh is the divisor of a modrm with no REX */
        r[RBX] = P_RBX;
        r[11] = P_RBX + 0x800; /* the same modrm with REX.B */
        r[RBP] = P_DATA + 0x2800;
        r[RDI] = P_DATA + 0x2000;
        r[RSP] = P_DATA + 0x3800;
        if (s != 3) r[RSI] = P_DATA + 0x1807; /* nonzero as a byte, word and dword: a divisor */
    }
}

static uint64_t mix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x100000001b3ULL; }
    for (; n; n--, b++) h = (h ^ *b) * 0x100000001b3ULL;
    return h;
}

/* One row, one seed: the state's digest, or the fault's identity. */
static uint64_t run_one(const uint8_t *code, int len, uint32_t undef, int s, int *sig, int verbose) {
    memcpy((void *)P_DATA, seed_mem[s], P_DATASZ);
    /* The whole page, every time: seed 3's pointers let a row write here (movdir64b does) and let later ones read it. */
    memset((void *)P_CODE, 0, P_CODESZ);
    memset((void *)P_CODE, 0x90, 64);
    memcpy((void *)P_CODE, code, len);
    /* jmp *disp(%rip), and the address far off: it is the build's, so it must not reach a result. */
    uint8_t *land = (uint8_t *)P_LAND, *slot = land + 6 + 0x800;
    uint32_t disp = 0x800;
    uint64_t cap = (uint64_t)p_capture;
    land[0] = 0xff; land[1] = 0x25;
    memcpy(land + 2, &disp, 4);
    memcpy(slot, &cap, 8);
    memcpy(p_seed_regs, seed_regs[s], sizeof p_seed_regs);
    p_seed_flags = seed_flags[s];
    p_seed_xs = seed_xs[s];
    memset(p_xs, 0, sizeof p_xs);
    got_sig = 0;
    struct itimerval t = {{0, 0}, {1, 0}}, off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &t, NULL);
    run_slot();
    setitimer(ITIMER_REAL, &off, NULL);
    *sig = got_sig;
    if (got_sig == SIGALRM) {
        fprintf(stderr, "timeout seed %d:", s);
        for (int i = 0; i < len; i++) fprintf(stderr, " %02x", code[i]);
        fprintf(stderr, "%c", 10);
    }
    if (got_sig) return (uint64_t)got_sig << 16 | (uint32_t)(got_code & 0xffff);
    /* FOP, FIP, FDP and MXCSR_MASK vary by CPU and say nothing of the result; so does the header. */
    memset(p_xs + 6, 0, 18);
    memset(p_xs + 28, 0, 4);
    memset(p_xs + 464, 0, 112);
    uint64_t h = 0xcbf29ce484222325ULL, f = p_flags & ~(uint64_t)undef & 0xcd5;
    h = mix(h, p_regs, sizeof p_regs);
    h = mix(h, &f, 8);
    h = mix(h, p_xs, 576 + 256);
    h = mix(h, (void *)P_DATA, P_DATASZ);
    memset(slot, 0, 8);
    h = mix(h, (void *)P_CODE, P_CODESZ);
    if (verbose) {
        printf("seed %d flags=%lx masked=%lx\n", s, p_flags, f);
        for (int i = 0; i < 16; i++) printf(" r%d=%lx", i, p_regs[i]);
        printf("\n xmm0=%lx,%lx xmm1=%lx st0=%lx mxcsr=%x fsw=%x mem[rbx]=%lx\n", *(uint64_t *)(p_xs + 160),
               *(uint64_t *)(p_xs + 168), *(uint64_t *)(p_xs + 176), *(uint64_t *)(p_xs + 32), *(uint32_t *)(p_xs + 24),
               *(uint16_t *)(p_xs + 2), *(uint64_t *)P_RBX);
        printf(" digest=%016lx\n", h);
    }
    return h;
}

static int parse_hex(const char *s, uint8_t *out) {
    int n = 0;
    for (; s[0] && s[1] && s[0] != '\t' && s[0] != '\n' && n < 15; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

struct row { uint8_t code[16]; int len; uint32_t undef; int run; };
struct res { uint64_t h; uint32_t sigmask; int done; };

static void setup(void) {
    if (mmap((void *)P_DATA, P_DATASZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED ||
        mmap((void *)P_CODE, P_CODESZ, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
    static char alt[65536];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    struct sigaction a;
    memset(&a, 0, sizeof a);
    a.sa_sigaction = on_fault;
    a.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    int sigs[] = {SIGILL, SIGSEGV, SIGBUS, SIGFPE, SIGTRAP, SIGALRM, SIGSYS};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &a, NULL);
    build_seeds();
}

static void run_row(const struct row *r, struct res *o) {
    uint64_t h = 0xcbf29ce484222325ULL;
    uint32_t m = 0;
    for (int s = 0; s < SEEDS; s++) {
        int sig = 0;
        uint64_t d = run_one(r->code, r->len, r->undef, s, &sig, 0);
        if (sig) m |= 1u << s;
        h = mix(h, &d, 8);
    }
    o->h = h;
    o->sigmask = m;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOFBF, 1 << 20);
    int rev = 0, each = 0;
    uint64_t shuf = 0;
    if (argc >= 2 && !strcmp(argv[1], "-r")) rev = 1;
    if (argc >= 2 && !strcmp(argv[1], "-f")) each = 1;
    if (argc >= 3 && !strcmp(argv[1], "-s")) shuf = strtoull(argv[2], NULL, 0) * 2 + 1;
    if (argc >= 3 && !strcmp(argv[1], "-v")) {
        struct row r = {0};
        r.len = parse_hex(argv[2], r.code);
        r.undef = argc > 3 ? strtoul(argv[3], NULL, 16) : 0;
        setup();
        for (int s = 0; s < SEEDS; s++) {
            int sig = 0;
            uint64_t d = run_one(r.code, r.len, r.undef, s, &sig, 1);
            if (sig) printf("seed %d fault sig=%d code=%d\n", s, sig, (int)(d & 0xffff));
        }
        return 0;
    }
    size_t n = 0, cap = 1 << 16;
    struct row *rows = malloc(cap * sizeof *rows);
    static char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        if (n == cap) rows = realloc(rows, (cap *= 2) * sizeof *rows);
        struct row *r = &rows[n++];
        r->len = parse_hex(line, r->code);
        r->undef = strtoul(t1 + 1, NULL, 16);
        r->run = strrchr(line, '\t')[1] == '-';
    }
    struct res *res = mmap(NULL, (n + 1) * sizeof *res, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    size_t *ord = malloc((n + 1) * sizeof *ord);
    for (size_t i = 0; i < n; i++) ord[i] = rev ? n - 1 - i : i;
    for (size_t i = n; shuf && i > 1; i--) { /* its own generator: rng() makes the seeds, which no order may change */
        shuf ^= shuf << 13; shuf ^= shuf >> 7; shuf ^= shuf << 17;
        size_t j = shuf % i, t = ord[i - 1];
        ord[i - 1] = ord[j];
        ord[j] = t;
    }
    /* A row can end its process however it likes; that costs the row only ("ff": killed its worker). */
    for (size_t at = 0; at < n;) {
        pid_t c = fork();
        if (c == 0) {
            setup();
            for (size_t end = each ? at + 1 : n; at < end; at++) {
                struct row *r = &rows[ord[at]];
                if (r->run) run_row(r, &res[ord[at]]);
                res[ord[at]].done = 1;
            }
            _exit(0);
        }
        int st;
        waitpid(c, &st, 0);
        while (at < n && res[ord[at]].done) at++;
        if (at < n && !(each && WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
            res[ord[at]].sigmask = 0xff;
            res[ord[at]].done = 1;
            at++;
        }
    }
    unsigned a, b, cc, d, name[4] = {0};
    __cpuid(0, a, name[0], name[2], name[1]);
    __cpuid(1, a, b, cc, d);
    printf("#cpu %s family=%u model=%u\n", (char *)name, ((a >> 8) & 0xf) + ((a >> 20) & 0xff),
           ((a >> 4) & 0xf) | ((a >> 12) & 0xf0));
    for (size_t i = 0; i < n; i++) {
        if (!rows[i].run) puts("-\t-");
        else printf("%x\t%016lx\n", res[i].sigmask, res[i].h);
    }
    return 0;
}

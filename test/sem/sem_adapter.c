/*
 * Adapter over lhns/avxemu's test/spec/sem_test.c: runs its instruction-form cases on the real
 * CPU only (no avxemu emulator) and checks their digests against the reference recorded on AVX2
 * silicon. Built to run under the v3compat client. sem_test.c is included, not modified.
 *
 *   semhw MANIFEST REF [smoke|full]     env SEM_ONLY=glob restricts the forms
 *
 * Exit: 0 match, 1 mismatch, 3 emulation missing (SIGILL, or a silent lzcnt/tzcnt-style
 * difference on a form the host lacks), 2 usage/setup.
 */
#define main sem_orig_main
#include "sem_test.c"
#undef main
#include <sys/wait.h>
#include <unistd.h>

/* sem_orig_main (unused here) references the emulator; the cases we run never call these. */
int decode(const uint8_t *p, decoded *d) { (void)p; (void)d; return 0; }
int avxemu_emulate(const decoded *d, avxemu_regfile *rf) { (void)d; (void)rf; return 0; }
size_t avxemu_page_size(void) { return PAGE; }

enum { EX_OK = 0, EX_MISMATCH = 1, EX_USAGE = 2, EX_MISSING = 3 };
static char *g_form;     /* shared with the parent: form being run when the child died */

static int host_lacks(const specform *f) {
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    int avx2 = (b >> 5) & 1, bmi1 = (b >> 3) & 1, bmi2 = (b >> 8) & 1;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    int fma = (c >> 12) & 1, movbe = (c >> 22) & 1, f16c = (c >> 29) & 1;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001), "c"(0));
    int abm = (c >> 5) & 1;
    const char *i = f->isa;
    if (!strncmp(i, "AVX2", 4)) return !avx2;
    if (!strcmp(i, "FMA")) return !fma;
    if (!strcmp(i, "BMI1")) return !bmi1;
    if (!strcmp(i, "BMI2")) return !bmi2;
    if (!strcmp(i, "LZCNT")) return !abm;
    if (!strcmp(i, "MOVBE")) return !movbe;
    if (!strcmp(i, "F16C")) return !f16c;
    return 0;
}

static int run_hw_suite(const char *manifest, const char *refpath, int full) {
    specform *forms;
    int nf = spec_load(manifest, &forms);
    if (nf < 0) return EX_USAGE;
    if (map_arena() < 0) { perror("arena"); return EX_USAGE; }
    install_handlers();
    if (ref_begin(REF_CHECK, "semantics", refpath) != 0) { fprintf(stderr, "cannot read reference %s\n", refpath); return EX_USAGE; }
    long nforms = 0, ncases = 0; int bad = 0;
    for (int i = 0; i < nf; i++) {
        const specform *f = &forms[i];
        if (!strcmp(f->status, "refused")) continue;
        if (getenv("SEM_ONLY") && fnmatch(getenv("SEM_ONLY"), f->name, 0)) continue;
        snprintf(g_form, 128, "%s", f->name);
        nforms++;
        FLAGS_MASK = flags_mask(f);
        uint64_t dh_s = REF_FNV_OFFSET, dh_f = REF_FNV_OFFSET; unsigned long cs = 0, cf = 0;
        int total = full_cases(f);
        for (int k = 0; k < total; k++) {
            int smoke = is_smoke(k);
            if (!full && !smoke) continue;
            sem_case c; sem_out ho; uint8_t ph[1024];
            gen_case(f, k, &c);
            run_hw(&c, &ho);
            size_t nh = pack(&ho, ph);
            dh_f = fnv(ph, nh, dh_f); cf++;
            if (smoke) { dh_s = fnv(ph, nh, dh_s); cs++; }
            ncases++;
        }
        for (int pass = 0; pass < 2; pass++) {
            if (pass && !full) continue;
            char nm[64]; unsigned long rc; uint64_t rd;
            snprintf(nm, sizeof nm, "%s:%c", f->name, pass ? 'f' : 's');
            uint64_t mine = pass ? dh_f : dh_s; unsigned long mc = pass ? cf : cs;
            if (!ref_get(nm, &rc, &rd) || mc != rc || mine != rd) {
                if (host_lacks(f)) {
                    printf("EMULATION MISSING: %s runs without faulting but gives a different result (silent)\n", f->name);
                    return EX_MISSING;
                }
                printf("MISMATCH: %s digest %016llx (%lu cases) != recorded %016llx (%lu)\n", nm,
                       (unsigned long long)mine, mc, (unsigned long long)rd, rc);
                bad = 1;
            }
        }
    }
    printf("semantics on the real CPU (%s): %ld forms, %ld cases, %s\n", full ? "full" : "smoke", nforms, ncases, bad ? "MISMATCH" : "match");
    return bad ? EX_MISMATCH : EX_OK;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: semhw MANIFEST REF [smoke|full]\n"); return EX_USAGE; }
    int full = argc > 3 && !strcmp(argv[3], "full");
    g_form = mmap(0, 128, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_form == MAP_FAILED) return EX_USAGE;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) { int rc = run_hw_suite(argv[1], argv[2], full); fflush(NULL); _exit(rc); }
    int st;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGILL) {
        printf("EMULATION MISSING: SIGILL in form '%s' (first missing instruction); nothing emulates it yet\n", g_form);
        return EX_MISSING;
    }
    if (WIFSIGNALED(st)) { printf("CRASH: signal %d in form '%s'\n", WTERMSIG(st), g_form); return EX_MISMATCH; }
    return WEXITSTATUS(st);
}

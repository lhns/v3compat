/*
 * Adapter over lhns/avxemu's test/spec/sem_test.c: runs its instruction-form cases on the CPU, under
 * the client, and checks their digests against the reference recorded on AVX2 silicon. sem_test.c
 * is included, not modified. Each form runs in its own process, as a refused form ends it.
 *
 *   semhw MANIFEST REF [smoke|full] [REFUSED]   env SEM_ONLY=glob restricts the forms, SEM_DUMP=1 dumps cases
 *   REFUSED: file of the forms the client still refuses; the list has to be exact.
 *
 * Exit: 0 all as expected, 1 mismatch or a refusal list that is out of date, 2 usage/setup.
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

enum { EX_OK = 0, EX_MISMATCH = 1, EX_USAGE = 2, EX_REFUSED = 86 /* V3COMPAT_REFUSED */ };

static int listed(const char *path, const char *form) {
    char line[128];
    FILE *k = path ? fopen(path, "r") : NULL;
    int hit = 0;
    while (k && fgets(line, sizeof line, k)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && line[0] != '#' && !strcmp(line, form)) hit = 1;
    }
    if (k) fclose(k);
    return hit;
}

static int run_form(const specform *f, int full) {
    FLAGS_MASK = flags_mask(f);
    uint64_t dh_s = REF_FNV_OFFSET, dh_f = REF_FNV_OFFSET; unsigned long cs = 0, cf = 0;
    int total = full_cases(f), bad = 0;
    for (int k = 0; k < total; k++) {
        int smoke = is_smoke(k);
        if (!full && !smoke) continue;
        sem_case c; sem_out ho; uint8_t ph[1024];
        gen_case(f, k, &c);
        run_hw(&c, &ho);
        size_t nh = pack(&ho, ph);
        if (getenv("SEM_DUMP")) {     /* per-case line, to diff a native run against a client run */
            char hx[80] = "";
            for (int b = 0; b < c.len; b++) sprintf(hx + 2 * b, "%02x", c.code[b]);
            printf("case %s k=%d place=%d code=%s kind=%d fault=%llx h=%016llx\n", f->name, k, c.place, hx, ho.kind,
                   (unsigned long long)ho.fault_page, (unsigned long long)fnv(ph, nh, REF_FNV_OFFSET));
            printf("  gpr");
            for (int r = 0; r < 16; r++) printf(" %llx", (unsigned long long)ho.st.gpr[r]);
            printf(" fl=%llx mx=%x mem=%llx ymm=%llx\n", (unsigned long long)ho.st.rflags, ho.st.mxcsr,
                   (unsigned long long)ho.mem_hash, (unsigned long long)fnv(ho.st.ymm, sizeof ho.st.ymm, REF_FNV_OFFSET));
        }
        dh_f = fnv(ph, nh, dh_f); cf++;
        if (smoke) { dh_s = fnv(ph, nh, dh_s); cs++; }
    }
    for (int pass = 0; pass < 2; pass++) {
        if (pass && !full) continue;
        char nm[64]; unsigned long rc; uint64_t rd;
        snprintf(nm, sizeof nm, "%s:%c", f->name, pass ? 'f' : 's');
        uint64_t mine = pass ? dh_f : dh_s; unsigned long mc = pass ? cf : cs;
        if (!ref_get(nm, &rc, &rd) || mc != rc || mine != rd) {
            printf("MISMATCH: %s digest %016llx (%lu cases) != recorded %016llx (%lu)\n", nm,
                   (unsigned long long)mine, mc, (unsigned long long)rd, rc);
            bad = 1;
        }
    }
    return bad ? EX_MISMATCH : EX_OK;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: semhw MANIFEST REF [smoke|full] [REFUSED]\n"); return EX_USAGE; }
    int full = argc > 3 && !strcmp(argv[3], "full");
    const char *refused = argc > 4 ? argv[4] : NULL;
    specform *forms;
    int nf = spec_load(argv[1], &forms);
    if (nf < 0) return EX_USAGE;
    if (map_arena() < 0) { perror("arena"); return EX_USAGE; }
    install_handlers();
    if (ref_begin(REF_CHECK, "semantics", argv[2]) != 0) { fprintf(stderr, "cannot read reference %s\n", argv[2]); return EX_USAGE; }
    int bad = 0, nforms = 0, nrefused = 0;
    for (int i = 0; i < nf; i++) {
        const specform *f = &forms[i];
        if (!strcmp(f->status, "refused")) continue;
        if (getenv("SEM_ONLY") && fnmatch(getenv("SEM_ONLY"), f->name, 0)) continue;
        nforms++;
        fflush(NULL);
        pid_t pid = fork();
        if (pid == 0) { int rc = run_form(f, full); fflush(NULL); _exit(rc); }
        int st;
        waitpid(pid, &st, 0);
        int was_refused = WIFEXITED(st) && WEXITSTATUS(st) == EX_REFUSED, expected = listed(refused, f->name);
        if (was_refused) nrefused++;
        if (was_refused != expected) {
            printf("REFUSAL LIST: %s %s\n", f->name, was_refused ? "refused, not listed" : "listed, not refused");
            bad = 1;
        } else if (!was_refused && WIFSIGNALED(st)) {
            printf("CRASH: signal %d in form %s\n", WTERMSIG(st), f->name);
            bad = 1;
        } else if (!was_refused && WEXITSTATUS(st) != EX_OK) {
            bad = 1;
        }
    }
    printf("semantics (%s): %d forms, %d refused, %s\n", full ? "full" : "smoke", nforms, nrefused, bad ? "FAIL" : "ok");
    return bad ? EX_MISMATCH : EX_OK;
}

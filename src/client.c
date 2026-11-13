/* v3compat DynamoRIO client: finds what the CPU lacks and has it replaced (expand.c). */
#include "dr_api.h"
#include "drmgr.h"
#include "allow.h"
#include "expand.h"
#include "execpolicy.h"
#include "str.h"

static const char *const cls_name[C_N] = {"avx2", "fma", "bmi", "lzcnt", "tzcnt", "movbe", "instrs"};
static uint64 counts[C_N];

enum { SIGILL_NUM = 4 }; /* no libc headers in the client */
static bool stats_on, audit_on;

/* Executed counts: a block's static class totals are added by a clean call at its entry. */
static void add_count(int c, int n) { __atomic_add_fetch(&counts[c], (uint64)n, __ATOMIC_RELAXED); }
/* Executed count per DR opcode of the classes the CPU lacks. */
static uint64 op_counts[OP_LAST];
static bool store_xl;
static void add_op(int op) { __atomic_add_fetch(&op_counts[op], 1, __ATOMIC_RELAXED); }

static dr_emit_flags_t event_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace,
                                     bool translating) {
    /* Runs when translating too: DR maps PCs through the list, so it must come out the same. */
    (void)tag; (void)for_trace; (void)translating;
    int n[C_N] = {0};
    instr_t *next;
    for (instr_t *in = instrlist_first_app(bb); in != NULL; in = next) {
        next = instr_get_next_app(in);
        int c = classify(drcontext, in);
        /* cpuid is translated whatever its shape. Off the list means refused when executed, not now:
         * a form on a path never taken must not stop the program. */
        char text[ALLOW_TEXT_MAX];
        if (instr_get_opcode(in) != OP_cpuid && !allow_listed(allow_key(drcontext, in, text), c != C_N)) {
            if (audit_on) {
                dr_fprintf(STDERR, "v3compat: audit %s at %p\n", text, instr_get_app_pc(in));
                if (c == C_N) continue;
            } else {
                expand_refuse(drcontext, bb, in);
                continue;
            }
        }
        if (stats_on) {
            if (c != C_N) {
                n[c]++;
                dr_insert_clean_call(drcontext, bb, in, (void *)add_op, false, 1,
                                     OPND_CREATE_INT32(instr_get_opcode(in)));
            }
            n[C_INSTRS]++;
        }
        expand_instr(drcontext, bb, in, c != C_N);
    }
    for (int c = 0; c < C_N; c++)
        if (n[c] != 0)
            dr_insert_clean_call(drcontext, bb, instrlist_first(bb), (void *)add_count, false, 2,
                                 OPND_CREATE_INT32(c), OPND_CREATE_INT32(n[c]));
    return store_xl ? DR_EMIT_STORE_TRANSLATIONS : DR_EMIT_DEFAULT;
}

/* DR stops a block at bytes it cannot decode and raises SIGILL there: the list cannot see them, and
 * the program would die of a signal, so this is where they are refused. A decoded form's SIGILL (ud2)
 * is the program's own. */
static dr_signal_action_t event_signal(void *drcontext, dr_siginfo_t *info) {
    {   /* LAB ONLY: what translation changed between the raw and the application state at a signal */
        static int n;
        if (info->raw_mcontext_valid && info->mcontext != NULL && n < 400) {
            dr_mcontext_t *a = info->mcontext, *r = info->raw_mcontext;
            int diff = 0;
            for (int i = 0; i < 16; i++)
                if (__builtin_memcmp(&a->simd[i], &r->simd[i], 32) != 0) diff |= 1 << i;
            int gd = (a->xax != r->xax) | (a->xbx != r->xbx) << 1 | (a->xcx != r->xcx) << 2 | (a->xdx != r->xdx) << 3 |
                (a->xsi != r->xsi) << 4 | (a->xdi != r->xdi) << 5 | (a->xbp != r->xbp) << 6 | (a->xsp != r->xsp) << 7 |
                (a->r8 != r->r8) << 8 | (a->r9 != r->r9) << 9 | (a->r10 != r->r10) << 10 | (a->r11 != r->r11) << 11 |
                (a->r12 != r->r12) << 12 | (a->r13 != r->r13) << 13 | (a->r14 != r->r14) << 14 | (a->r15 != r->r15) << 15;
            n++;
            dr_fprintf(STDERR, "V3S sig=%d raw=%p app=%p ymmdiff=%x gprdiff=%x flagsdiff=%d tid=%d\n", info->sig, r->pc, a->pc,
                       diff, gd, a->xflags != r->xflags, (int)dr_get_thread_id(drcontext));
        }
    }
    if (info->sig != SIGILL_NUM || info->mcontext == NULL) return DR_SIGNAL_DELIVER;
    instr_t in;
    instr_init(drcontext, &in);
    bool known = decode(drcontext, info->mcontext->pc, &in) != NULL && instr_valid(&in);
    instr_free(drcontext, &in);
    if (known) return DR_SIGNAL_DELIVER;
    dr_fprintf(STDERR, "v3compat: refusing undecodable instruction at %p\n", info->mcontext->pc);
    dr_exit_process(V3COMPAT_REFUSED);
    return DR_SIGNAL_DELIVER;
}

static void event_exit(void) {
    if (stats_on) {
        dr_fprintf(STDERR, "v3compat: stats pid=%d", (int)dr_get_process_id());
        for (int i = 0; i < C_N; i++) dr_fprintf(STDERR, " %s=%llu", cls_name[i], (unsigned long long)counts[i]);
        dr_fprintf(STDERR, "\n");
        for (int op = 0; op < OP_LAST; op++)
            if (op_counts[op] != 0)
                dr_fprintf(STDERR, "v3compat: op %s %llu\n", decode_opcode_name(op),
                           (unsigned long long)op_counts[op]);
    }
    expand_exit();
    drmgr_exit();
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    (void)id;
    dr_set_client_name("v3compat", "https://github.com/lhns/v3compat");
    if (!drmgr_init()) return;
    /* The launcher turns V3COMPAT_STATS=1 and V3COMPAT_HELPERS_ONLY=1 into these options. */
    bool helpers_only = false;
    for (int i = 1; i < argc; i++) {
        if (starts(argv[i], "-stats")) stats_on = true;
        if (starts(argv[i], "-helpers-only")) helpers_only = true;
        if (starts(argv[i], "-audit")) audit_on = true;
        if (starts(argv[i], "-store-xl")) store_xl = true;   /* LAB ONLY */
    }
    if (!expand_init(helpers_only)) dr_abort_with_code(V3COMPAT_REFUSED);
    drmgr_register_bb_app2app_event(event_app2app, NULL);
    drmgr_register_signal_event(event_signal);
    exec_policy_init();
    drmgr_register_exit_event(event_exit);
}

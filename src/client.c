/* v3compat DynamoRIO client: finds what the CPU lacks and has it replaced (expand.c). */
#include "dr_api.h"
#include "drmgr.h"
#include "allow.h"
#include "expand.h"
#include "execpolicy.h"

static const char *const cls_name[C_N] = {"avx2", "fma", "bmi", "lzcnt", "tzcnt", "movbe", "instrs"};
static uint64 counts[C_N];

enum { SIGILL_NUM = 4 }; /* no libc headers in the client */
static bool stats_on, audit_on;
/* DR exports no libc string functions to clients (calling one dies at first use). */
static bool starts(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return false;
    return true;
}

/* Executed counts: a block's static class totals are added by a clean call at its entry. */
static void add_count(int c, int n) { __atomic_add_fetch(&counts[c], (uint64)n, __ATOMIC_RELAXED); }
/* Executed count per DR opcode of the classes the CPU lacks. */
static uint64 op_counts[OP_LAST];
static void add_op(int op) { __atomic_add_fetch(&op_counts[op], 1, __ATOMIC_RELAXED); }

/* Decisions by instruction bytes: DR decodes the same bytes to the same instruction (RIP-relative
 * displacements and branch targets are operands "m" and "p" in the key, not values), so repeats in
 * the many blocks of a startup skip classify and the key. Only passes are kept, so audit and
 * refusal output are unchanged. A lock that is busy means no cache, never a wait. */
enum { MEMO_BITS = 14, MEMO_MAX = 15 };
typedef struct { byte bytes[MEMO_MAX]; byte meta; } memo_t;   /* meta: length | class << 4 */
static memo_t memo[1 << MEMO_BITS];
static void *memo_lock;

static uint memo_slot(const byte *p, uint len) {
    uint64 h = 0xcbf29ce484222325ULL;
    for (uint i = 0; i < len; i++) h = (h ^ p[i]) * 0x100000001b3ULL;
    return (uint)(h >> (64 - MEMO_BITS));
}

static dr_emit_flags_t event_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace,
                                     bool translating) {
    /* Runs when translating too: DR maps PCs through the list, so it must come out the same. */
    (void)tag; (void)for_trace; (void)translating;
    int n[C_N] = {0};
    instr_t *next;
    for (instr_t *in = instrlist_first_app(bb); in != NULL; in = next) {
        next = instr_get_next_app(in);
        int c = C_N;
        const byte *raw = NULL;
        uint len = 0, slot = 0;
        bool hit = false;
        if (!audit_on && instr_raw_bits_valid(in) && instr_length(drcontext, in) <= MEMO_MAX) {
            raw = instr_get_raw_bits(in);
            len = instr_length(drcontext, in);
            slot = memo_slot(raw, len);
            if (dr_mutex_trylock(memo_lock)) {
                const memo_t *m = &memo[slot];
                hit = (m->meta & 15) == len && len != 0;
                for (uint i = 0; hit && i < len; i++) hit = m->bytes[i] == raw[i];
                c = m->meta >> 4;
                dr_mutex_unlock(memo_lock);
            }
        }
        if (!hit) c = classify(drcontext, in);
        /* cpuid is translated whatever its shape. Off the list means refused when executed, not now:
         * a form on a path never taken must not stop the program. */
        char text[ALLOW_TEXT_MAX];
        if (!hit && instr_get_opcode(in) != OP_cpuid && !allow_listed(allow_key(drcontext, in, text), c != C_N)) {
            if (audit_on) {
                dr_fprintf(STDERR, "v3compat: audit %s at %p\n", text, instr_get_app_pc(in));
                if (c == C_N) continue;
            } else {
                expand_refuse(drcontext, bb, in);
                continue;
            }
        } else if (!hit && raw != NULL && instr_get_opcode(in) != OP_cpuid && dr_mutex_trylock(memo_lock)) {
            memo_t *m = &memo[slot];
            for (uint i = 0; i < len; i++) m->bytes[i] = raw[i];
            m->meta = (byte)(len | c << 4);
            dr_mutex_unlock(memo_lock);
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
    return DR_EMIT_DEFAULT;
}

/* DR stops a block at bytes it cannot decode and raises SIGILL there: the list cannot see them, and
 * the program would die of a signal, so this is where they are refused. A decoded form's SIGILL (ud2)
 * is the program's own. */
static dr_signal_action_t event_signal(void *drcontext, dr_siginfo_t *info) {
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
    }
    if (!expand_init(helpers_only)) dr_abort_with_code(V3COMPAT_REFUSED);
    memo_lock = dr_mutex_create();
    drmgr_register_bb_app2app_event(event_app2app, NULL);
    drmgr_register_signal_event(event_signal);
    exec_policy_init();
    dr_register_exit_event(event_exit);
}

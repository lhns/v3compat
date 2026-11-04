/* v3compat DynamoRIO client: finds what the CPU lacks and has it replaced (expand.c). */
#include "dr_api.h"
#include "drmgr.h"
#include "expand.h"

/* DR exports no libc string functions to clients (calling one dies at first use). */
static bool starts(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return false;
    return true;
}
static bool contains(const char *s, const char *p) {
    for (; *s; s++) if (starts(s, p)) return true;
    return false;
}

enum cls { C_AVX2, C_FMA, C_BMI, C_LZCNT, C_TZCNT, C_MOVBE, C_INSTRS, C_N };
static const char *const cls_name[C_N] = {"avx2", "fma", "bmi", "lzcnt", "tzcnt", "movbe", "instrs"};
static uint64 counts[C_N];

static bool stats_on;

/* Executed counts: a block's static class totals are added by a clean call at its entry. */
static void add_count(int c, int n) { __atomic_add_fetch(&counts[c], (uint64)n, __ATOMIC_RELAXED); }
/* Executed count per DR opcode of the classes the CPU lacks. */
static uint64 op_counts[OP_LAST];
static void add_op(int op) { __atomic_add_fetch(&op_counts[op], 1, __ATOMIC_RELAXED); }

static bool has_ymm(instr_t *in) {
    for (int i = 0; i < instr_num_srcs(in); i++) {
        opnd_t o = instr_get_src(in, i);
        if (opnd_is_reg(o) && reg_is_ymm(opnd_get_reg(o))) return true;
    }
    for (int i = 0; i < instr_num_dsts(in); i++) {
        opnd_t o = instr_get_dst(in, i);
        if (opnd_is_reg(o) && reg_is_ymm(opnd_get_reg(o))) return true;
    }
    return false;
}

/* Classes the CPU lacks on AVX-only parts; BMI/AVX2 opcodes are matched by DR opcode or mnemonic. */
static int classify(void *dc, instr_t *in) {
    int op = instr_get_opcode(in);
    if (instr_is_evex_encoded(dc, in)) return C_AVX2;
    switch (op) {
    case OP_lzcnt: return C_LZCNT;
    case OP_tzcnt: return C_TZCNT;
    case OP_movbe: return C_MOVBE;
    case OP_andn: case OP_bextr: case OP_blsi: case OP_blsmsk: case OP_blsr: case OP_bzhi:
    case OP_mulx: case OP_pdep: case OP_pext: case OP_rorx: case OP_sarx: case OP_shlx:
    case OP_shrx:
        return C_BMI;
    /* Always AVX2, whatever the vector width. */
    case OP_vbroadcasti128: case OP_vextracti128: case OP_vinserti128: case OP_vperm2i128:
    case OP_vpblendd: case OP_vpbroadcastb: case OP_vpbroadcastw: case OP_vpbroadcastd:
    case OP_vpbroadcastq: case OP_vpmaskmovd: case OP_vpmaskmovq: case OP_vpsllvd:
    case OP_vpsllvq: case OP_vpsrlvd: case OP_vpsrlvq: case OP_vpsravd: case OP_vgatherdpd:
    case OP_vgatherqpd: case OP_vgatherdps: case OP_vgatherqps: case OP_vpgatherdd:
    case OP_vpgatherqd: case OP_vpgatherdq: case OP_vpgatherqq:
        return C_AVX2;
    case OP_vbroadcastss: case OP_vbroadcastsd:   /* the register-source form is AVX2 */
        return opnd_is_reg(instr_get_src(in, 0)) ? C_AVX2 : C_N;
    case OP_vmovntdqa:   /* the ymm load is AVX2 */
        return has_ymm(in) ? C_AVX2 : C_N;
    default: break;
    }
    const char *n = decode_opcode_name(op);
    if (n == NULL) return C_N;
    if (n[0] == 'v' && n[1] == 'f' && (contains(n, "madd") || contains(n, "msub"))) return C_FMA;
    /* 256-bit integer VEX ops are AVX2; the 256-bit FP-domain vp* ones below are AVX. */
    if (n[0] == 'v' && n[1] == 'p' && has_ymm(in) && !starts(n, "vptest") && !starts(n, "vpermil") &&
        !starts(n, "vperm2f128"))
        return C_AVX2;
    return C_N;
}

static dr_emit_flags_t event_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace,
                                     bool translating) {
    /* Runs when translating too: DR maps PCs through the list, so it must come out the same. */
    (void)tag; (void)for_trace; (void)translating;
    int n[C_N] = {0};
    instr_t *next;
    for (instr_t *in = instrlist_first_app(bb); in != NULL; in = next) {
        next = instr_get_next_app(in);
        int c = classify(drcontext, in);
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
    }
    if (!expand_init(helpers_only)) dr_abort_with_code(V3COMPAT_REFUSED);
    drmgr_register_bb_app2app_event(event_app2app, NULL);
    dr_register_exit_event(event_exit);
}

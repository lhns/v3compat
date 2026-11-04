/* v3compat DynamoRIO client. No emulation yet: the app2app hook is where it lands. */
#include "dr_api.h"
#include "drmgr.h"

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

static void count(enum cls c) { __atomic_add_fetch(&counts[c], 1, __ATOMIC_RELAXED); }

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
static void classify(instr_t *in) {
    int op = instr_get_opcode(in);
    count(C_INSTRS);
    switch (op) {
    case OP_lzcnt: count(C_LZCNT); return;
    case OP_tzcnt: count(C_TZCNT); return;
    case OP_movbe: count(C_MOVBE); return;
    case OP_andn: case OP_bextr: case OP_blsi: case OP_blsmsk: case OP_blsr: case OP_bzhi:
    case OP_mulx: case OP_pdep: case OP_pext: case OP_rorx: case OP_sarx: case OP_shlx:
    case OP_shrx:
        count(C_BMI); return;
    /* Always AVX2, whatever the vector width. */
    case OP_vbroadcasti128: case OP_vextracti128: case OP_vinserti128: case OP_vperm2i128:
    case OP_vpblendd: case OP_vpbroadcastb: case OP_vpbroadcastw: case OP_vpbroadcastd:
    case OP_vpbroadcastq: case OP_vpmaskmovd: case OP_vpmaskmovq: case OP_vpsllvd:
    case OP_vpsllvq: case OP_vpsrlvd: case OP_vpsrlvq: case OP_vpsravd: case OP_vgatherdpd:
    case OP_vgatherqpd: case OP_vgatherdps: case OP_vgatherqps: case OP_vpgatherdd:
    case OP_vpgatherqd: case OP_vpgatherdq: case OP_vpgatherqq:
        count(C_AVX2); return;
    case OP_vbroadcastss: case OP_vbroadcastsd:   /* the register-source form is AVX2 */
        if (opnd_is_reg(instr_get_src(in, 0))) count(C_AVX2);
        return;
    default: break;
    }
    const char *n = decode_opcode_name(op);
    if (n == NULL) return;
    if (n[0] == 'v' && n[1] == 'f' && (contains(n, "madd") || contains(n, "msub"))) { count(C_FMA); return; }
    /* 256-bit integer VEX ops are AVX2; the 256-bit FP-domain vp* ones below are AVX. */
    if (n[0] == 'v' && n[1] == 'p' && has_ymm(in) && !starts(n, "vptest") &&
        !starts(n, "vpermil"))
        count(C_AVX2);
}

static dr_emit_flags_t event_app2app(void *drcontext, void *tag, instrlist_t *bb, bool for_trace,
                                     bool translating) {
    (void)drcontext; (void)tag; (void)for_trace;
    if (translating) return DR_EMIT_DEFAULT;       /* already seen when first built */
    if (stats_on)
        for (instr_t *in = instrlist_first_app(bb); in != NULL; in = instr_get_next_app(in))
            classify(in);
    return DR_EMIT_DEFAULT;
}

static void event_exit(void) {
    if (stats_on) {
        dr_fprintf(STDERR, "v3compat: stats pid=%d", (int)dr_get_process_id());
        for (int i = 0; i < C_N; i++) dr_fprintf(STDERR, " %s=%llu", cls_name[i], (unsigned long long)counts[i]);
        dr_fprintf(STDERR, "\n");
    }
    drmgr_exit();
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    (void)id;
    dr_set_client_name("v3compat", "https://github.com/lhns/v3compat");
    if (!drmgr_init()) return;
    /* The launcher turns V3COMPAT_STATS=1 into this option. */
    for (int i = 1; i < argc; i++)
        if (starts(argv[i], "-stats")) stats_on = true;
    drmgr_register_bb_app2app_event(event_app2app, NULL);
    dr_register_exit_event(event_exit);
}

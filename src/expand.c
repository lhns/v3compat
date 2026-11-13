/*
 * Every expansion is rewindable and commits last (a gather commits per element, see below): until one final write, the only application
 * state it touches is scratch saved in raw TLS, which event_restore puts back. A memory operand
 * is read by one real load with translation P, so a fault is the real fault at P. Inline code
 * leaves flags alone, except where the instruction writes them (andn, bzhi, tzcnt, lzcnt) or the
 * sequence clobbers them and saves them first (rorx, mulx, shlx, shrx, sarx; never conditional on
 * flag liveness); the mcontext path writes them in its atomic commit.
 */
#include "allow.h"
#include "expand.h"
#include "drmgr.h"
#include "avxemu_helpers.h"
#include <stddef.h>

/* Raw TLS (limit: 64 slots). Addressed through gs, so operands copy without a GPR scratch. Must be
 * 32-byte aligned: a ymm save split across a cache line triples the cost of an inline form. */
typedef struct {
    ymm256 save[3];
    ymm256 a, b, c, out;
    uint64 pending;   /* unmasked FP flags a call left; outside both ranges event_restore reads */
    uint64 gsave[6];  /* gsave[5]: the application's rsp while a frame helper runs on `stack` */
    uint64 fl;        /* the application's flags as lahf and seto leave ax, while a sequence has clobbered them */
    uint64 self;
    uint64 stack;     /* top of this thread's helper stack */
    uint64 thunk;     /* lean_thunk, called through here so the code cache needs no reach to it */
    uint64 desc;      /* the frame helper's descriptor (vec_call) */
    uint64 pad[4];
    ymm256 vacc;      /* LAB ONLY: OR of (emulated ^ native) over every verified inline op */
    ymm256 vref;      /* LAB ONLY: the native result of the op being emulated */
} tls_t;
#define TLS_SLOTS (sizeof(tls_t) / sizeof(void *))

static reg_id_t tls_seg;
static uint tls_offs;
static int tls_idx;
static bool helpers_only;
bool lab_verify;   /* LAB ONLY: set by the client's -verify */

static opnd_t tls_opnd(size_t off, opnd_size_t sz) {
    return opnd_create_far_base_disp(tls_seg, DR_REG_NULL, DR_REG_NULL, 0, (int)(tls_offs + off), sz);
}
#define SAVE_Y(i) tls_opnd(offsetof(tls_t, save) + (i) * sizeof(ymm256), OPSZ_32)
#define SAVE_G(i) tls_opnd(offsetof(tls_t, gsave) + (i) * sizeof(uint64), OPSZ_8)
#define SAVE_SP SAVE_G(5)
#define FRAME(f) tls_opnd(offsetof(tls_t, f), OPSZ_32)
#define FRAME_X(f) tls_opnd(offsetof(tls_t, f), OPSZ_16)

static inline tls_t *my_tls(void) {
    tls_t *t;
    __asm__ volatile("mov %%gs:(%1), %0" : "=r"(t) : "r"((uint64)(tls_offs + offsetof(tls_t, self))));
    return t;
}

static reg_id_t xmm_of(reg_id_t y) { return (reg_id_t)(y - DR_REG_YMM0 + DR_REG_XMM0); }
static reg_id_t ymm_of(reg_id_t r) { return reg_is_ymm(r) ? r : (reg_id_t)(r - DR_REG_XMM0 + DR_REG_YMM0); }
static opnd_t reg(reg_id_t r) { return opnd_create_reg(r); }

/* Emission state for one application instruction. */
typedef struct {
    void *dc;
    instrlist_t *bb;
    instr_t *where;
    app_pc pc;
    reg_id_t scratch[3];
} emit_t;

static instr_t *put(emit_t *e, instr_t *i) {
    instrlist_meta_preinsert(e->bb, e->where, i);
    return i;
}

static void pick_scratch(emit_t *e, instr_t *in) {
    int n = 0;
    for (reg_id_t y = DR_REG_YMM15; n < 3; y--)
        if (!instr_uses_reg(in, y) && !instr_uses_reg(in, xmm_of(y))) e->scratch[n++] = y;
}

/* A meta instruction's fs: and gs: reach DR's segments, not the application's, so those operands are
 * rewritten by unseg. Refused: a 32-bit address (0x67, whose wrap the rewrite would not reproduce) and
 * rip-relative with a segment (the address is out of disp32 range once the base is added). */
static bool uses_seg(opnd_t m) { return opnd_get_segment(m) == DR_SEG_FS || opnd_get_segment(m) == DR_SEG_GS; }
static bool mem_supported(opnd_t m) {
    if (opnd_is_far_rel_addr(m)) return false;
    if (uses_seg(m) && opnd_is_base_disp(m) &&
        ((opnd_get_base(m) != DR_REG_NULL && !reg_is_64bit(opnd_get_base(m))) ||
         (opnd_get_index(m) != DR_REG_NULL && reg_is_gpr(opnd_get_index(m)) && !reg_is_64bit(opnd_get_index(m)))))
        return false;
    return true;
}
static bool needs_abs(opnd_t m) { return (opnd_is_rel_addr(m) || opnd_is_abs_addr(m)) && !uses_seg(m); }

/* Returns m without the override, the segment base plus base in `r` (which the caller has saved). */
static opnd_t unseg(emit_t *e, opnd_t m, reg_id_t r) {
    dr_insert_get_seg_base(e->dc, e->bb, e->where, opnd_get_segment(m), r);
    if (opnd_is_far_abs_addr(m))   /* no base or index: disp32, sign-extended */
        return opnd_create_base_disp(r, DR_REG_NULL, 0, (int)(ptr_int_t)opnd_get_addr(m), opnd_get_size(m));
    reg_id_t b = opnd_get_base(m);
    if (b != DR_REG_NULL) put(e, INSTR_CREATE_lea(e->dc, reg(r), opnd_create_base_disp(b, r, 1, 0, OPSZ_lea)));
    return opnd_create_base_disp(r, opnd_get_index(m), opnd_get_scale(m), opnd_get_disp(m), opnd_get_size(m));
}

/* The one instruction that may fault, and `t` is written only if the load succeeds. */
static void load_mem(emit_t *e, opnd_t m, reg_id_t t) {
    uint size = opnd_size_in_bytes(opnd_get_size(m));
    /* The application's address may be out of rip reach from the code cache. */
    bool abs = needs_abs(m), seg = uses_seg(m);
    reg_id_t sr = DR_REG_RAX;
    while (seg && opnd_uses_reg(m, sr)) sr++;   /* rax, rcx, rdx: an operand names two registers at most */
    if (abs || seg) put(e, INSTR_CREATE_mov_st(e->dc, SAVE_G(0), reg(sr)));
    if (abs) {
        instrlist_insert_mov_immed_ptrsz(e->dc, (ptr_int_t)opnd_get_addr(m), reg(sr), e->bb, e->where, NULL, NULL);
        m = opnd_create_base_disp(sr, DR_REG_NULL, 0, 0, opnd_get_size(m));
    } else if (seg) {
        m = unseg(e, m, sr);
    }
    reg_id_t x = xmm_of(t);
    instr_t *i;
    switch (size) {
    case 32: i = INSTR_CREATE_vmovdqu(e->dc, reg(t), m); break;
    case 16: i = INSTR_CREATE_vmovdqu(e->dc, reg(x), m); break;
    case 8: i = INSTR_CREATE_vmovq(e->dc, reg(x), m); break;
    case 4: i = INSTR_CREATE_vmovd(e->dc, reg(x), m); break;
    case 2: i = INSTR_CREATE_vpinsrw(e->dc, reg(x), reg(x), m, OPND_CREATE_INT8(0)); break;
    default: i = INSTR_CREATE_vpinsrb(e->dc, reg(x), reg(x), m, OPND_CREATE_INT8(0)); break;
    }
    put(e, i);
    if (abs || seg) put(e, INSTR_CREATE_mov_ld(e->dc, reg(sr), SAVE_G(0)));
}

static void save_y(emit_t *e, int i) { put(e, INSTR_CREATE_vmovdqu(e->dc, SAVE_Y(i), reg(e->scratch[i]))); }
static void reload_y(emit_t *e, int i) { put(e, INSTR_CREATE_vmovdqu(e->dc, reg(e->scratch[i]), SAVE_Y(i))); }

/* DR lists an immediate shift count first, and every other second source after the first. */
static void vec_srcs(instr_t *in, opnd_t *a, opnd_t *b) {
    bool imm_first = opnd_is_immed_int(instr_get_src(in, 0));
    *a = instr_get_src(in, imm_first ? 1 : 0);
    *b = instr_get_src(in, imm_first ? 0 : 1);
}

static void op_128(emit_t *e, int opc, reg_id_t d, opnd_t a, opnd_t b) {
    instr_t *i = instr_build(e->dc, opc, 1, 2);
    instr_set_dst(i, 0, reg(d));
    bool imm = opnd_is_immed_int(b);
    instr_set_src(i, 0, imm ? b : a);
    instr_set_src(i, 1, imm ? a : b);
    put(e, i);
}

/*
 * Inline, in two 128-bit halves: the ops that dominate measured programs (plan, "Phase 1 results").
 * The second source is ymm or memory; for shifts it is the count: xmm, memory or an immediate.
 */
static bool inline_form(int opc) {
    switch (opc) {
    case OP_vpaddb: case OP_vpaddw: case OP_vpaddd: case OP_vpaddq:
    case OP_vpsubb: case OP_vpsubw: case OP_vpsubd: case OP_vpsubq:
    case OP_vpcmpeqb: case OP_vpcmpeqw: case OP_vpcmpeqd: case OP_vpcmpeqq:
    case OP_vpcmpgtb: case OP_vpcmpgtw: case OP_vpcmpgtd: case OP_vpcmpgtq:
    case OP_vpminub: case OP_vpminuw: case OP_vpminud: case OP_vpminsb: case OP_vpminsw: case OP_vpminsd:
    case OP_vpmaxub: case OP_vpmaxuw: case OP_vpmaxud: case OP_vpmaxsb: case OP_vpmaxsw: case OP_vpmaxsd:
    case OP_vpmullw: case OP_vpmulld: case OP_vpmuludq: case OP_vpshufb:
    case OP_vpsllw: case OP_vpslld: case OP_vpsllq: case OP_vpsrlw: case OP_vpsrld: case OP_vpsrlq:
    case OP_vpsraw: case OP_vpsrad:
        return true;
    }
    return false;
}

static void expand_inline(emit_t *e, instr_t *in) {
    int opc = instr_get_opcode(in);
    reg_id_t d = opnd_get_reg(instr_get_dst(in, 0));
    opnd_t ao, b;
    vec_srcs(in, &ao, &b);
    reg_id_t a = opnd_get_reg(ao);
    bool shift = opc == OP_vpsllw || opc == OP_vpslld || opc == OP_vpsllq || opc == OP_vpsrlw ||
                 opc == OP_vpsrld || opc == OP_vpsrlq || opc == OP_vpsraw || opc == OP_vpsrad;
    bool mem = opnd_is_memory_reference(b);
    /* DR gives a shift count the vector's width: it is xmm or m128 for ymm as well. */
    if (shift && mem) opnd_set_size(&b, OPSZ_16);
    if (shift && opnd_is_reg(b)) b = reg(xmm_of(ymm_of(opnd_get_reg(b))));
    reg_id_t t0 = e->scratch[0], t1 = e->scratch[1], t2 = e->scratch[2];
    reg_id_t t0x = xmm_of(t0), t1x = xmm_of(t1), t2x = xmm_of(t2);

    if (lab_verify && !shift && !uses_seg(b) && !(mem && needs_abs(b))) {   /* LAB ONLY: the CPU's own result */
        instr_t *nat = instr_build(e->dc, opc, 1, 2);
        instr_set_dst(nat, 0, reg(t2));
        instr_set_src(nat, 0, reg(a));
        instr_set_src(nat, 1, b);
        save_y(e, 2);
        put(e, nat);
        put(e, INSTR_CREATE_vmovdqu(e->dc, tls_opnd(offsetof(tls_t, vref), OPSZ_32), reg(t2)));
        reload_y(e, 2);
    }
    save_y(e, 0);
    save_y(e, 1);
    if (mem) {
        save_y(e, 2);
        load_mem(e, b, t2);
    }
    put(e, INSTR_CREATE_vextractf128(e->dc, reg(t0x), reg(a), OPND_CREATE_INT8(1)));
    opnd_t hi_b, lo_b;
    if (shift) {
        hi_b = lo_b = mem ? reg(t2x) : b;   /* both halves shift by the same count */
    } else {
        reg_id_t src = mem ? t2 : opnd_get_reg(b);
        put(e, INSTR_CREATE_vextractf128(e->dc, reg(t1x), reg(src), OPND_CREATE_INT8(1)));
        hi_b = reg(t1x);
        lo_b = reg(xmm_of(src));
    }
    op_128(e, opc, t0x, reg(t0x), hi_b);
    op_128(e, opc, t1x, reg(xmm_of(a)), lo_b);
    put(e, INSTR_CREATE_vinsertf128(e->dc, reg(t1), reg(t1), reg(t0x), OPND_CREATE_INT8(1)));
    if (mem) reload_y(e, 2);
    reload_y(e, 0);
    put(e, INSTR_CREATE_vmovdqu(e->dc, reg(d), reg(t1)));   /* commit */
    instrlist_set_translation_target(e->bb, decode_next_pc(e->dc, e->pc));
    reload_y(e, 1);
    if (lab_verify && !shift && !uses_seg(b) && !(mem && needs_abs(b))) {
        save_y(e, 2);
        put(e, INSTR_CREATE_vxorps(e->dc, reg(t2), reg(d), tls_opnd(offsetof(tls_t, vref), OPSZ_32)));
        put(e, INSTR_CREATE_vorps(e->dc, reg(t2), reg(t2), tls_opnd(offsetof(tls_t, vacc), OPSZ_32)));
        put(e, INSTR_CREATE_vmovdqu(e->dc, tls_opnd(offsetof(tls_t, vacc), OPSZ_32), reg(t2)));
        reload_y(e, 2);
    }
}

/* One-source op on an xmm: dst = op(src). */
static void op_1(emit_t *e, int opc, reg_id_t d, opnd_t a) {
    instr_t *i = instr_build(e->dc, opc, 1, 1);
    instr_set_dst(i, 0, reg(d));
    instr_set_src(i, 0, a);
    put(e, i);
}

/* Width-changing and one-source forms with a ymm destination, from two 128-bit halves, and broadcasts to xmm. The source (xmm or
 * memory) is read once, by one real load. Scratch registers are saved and restored as in expand_inline. */
typedef struct { int opc; uint8_t src_bytes, hi_shift; } wide_t;   /* hi_shift: bytes to drop for the high half */
static const wide_t wide_forms[] = {
    {OP_vpmovzxbw, 16, 8}, {OP_vpmovzxbd, 8, 4}, {OP_vpmovzxbq, 4, 2},
    {OP_vpmovzxwd, 16, 8}, {OP_vpmovzxwq, 8, 4}, {OP_vpmovzxdq, 16, 8},
    {OP_vpmovsxbw, 16, 8}, {OP_vpmovsxbd, 8, 4}, {OP_vpmovsxbq, 4, 2},
    {OP_vpmovsxwd, 16, 8}, {OP_vpmovsxwq, 8, 4}, {OP_vpmovsxdq, 16, 8},
};
static const wide_t *wide_form(int opc) {
    for (size_t i = 0; i < sizeof wide_forms / sizeof wide_forms[0]; i++)
        if (wide_forms[i].opc == opc) return &wide_forms[i];
    return NULL;
}
static bool bcast_form(int opc) {
    return opc == OP_vpbroadcastb || opc == OP_vpbroadcastw || opc == OP_vpbroadcastd || opc == OP_vpbroadcastq;
}
static bool abs_form(int opc) { return opc == OP_vpabsb || opc == OP_vpabsw || opc == OP_vpabsd; }

static bool oneop_ok(instr_t *in) {
    if (instr_num_dsts(in) != 1 || instr_num_srcs(in) != 1) return false;
    opnd_t d = instr_get_dst(in, 0), s = instr_get_src(in, 0);
    int opc = instr_get_opcode(in);
    /* AVX1 has the xmm forms of the others; a broadcast to xmm is AVX2. */
    if (!opnd_is_reg(d) || !(reg_is_ymm(opnd_get_reg(d)) || (bcast_form(opc) && reg_is_xmm(opnd_get_reg(d)))))
        return false;
    if (abs_form(opc)) return opnd_is_memory_reference(s) || (opnd_is_reg(s) && reg_is_ymm(opnd_get_reg(s)));
    return opnd_is_memory_reference(s) || (opnd_is_reg(s) && (reg_is_xmm(opnd_get_reg(s)) || reg_is_ymm(opnd_get_reg(s))));
}

static void expand_oneop(emit_t *e, instr_t *in) {
    int opc = instr_get_opcode(in);
    reg_id_t d = opnd_get_reg(instr_get_dst(in, 0));
    opnd_t s = instr_get_src(in, 0);
    const wide_t *w = wide_form(opc);
    bool mem = opnd_is_memory_reference(s), bc = bcast_form(opc), ab = abs_form(opc);
    uint bytes = w ? w->src_bytes : opc == OP_vpbroadcastb ? 1 : opc == OP_vpbroadcastw ? 2 : opc == OP_vpbroadcastd ? 4 : 8;
    reg_id_t t0 = e->scratch[0], t1 = e->scratch[1], t2 = e->scratch[2];
    reg_id_t t0x = xmm_of(t0), t1x = xmm_of(t1);
    save_y(e, 0);
    save_y(e, 1);
    if (mem) {
        save_y(e, 2);
        if (!ab) opnd_set_size(&s, bytes == 1 ? OPSZ_1 : bytes == 2 ? OPSZ_2 : bytes == 4 ? OPSZ_4 : bytes == 8 ? OPSZ_8 : OPSZ_16);
        load_mem(e, s, t2);
    }
    reg_id_t sv = mem ? t2 : ymm_of(opnd_get_reg(s)), sx = xmm_of(sv);
    /* The result is built in t1: t0 and t2 are reloaded before the commit, t1 after it. */
    if (bc) {
        /* Element 0 into every element of the low half. */
        if (opc == OP_vpbroadcastb) {
            put(e, INSTR_CREATE_vpxor(e->dc, reg(t0x), reg(t0x), reg(t0x)));
            put(e, INSTR_CREATE_vpshufb(e->dc, reg(t1x), reg(sx), reg(t0x)));
        } else if (opc == OP_vpbroadcastw) {
            put(e, INSTR_CREATE_vpshuflw(e->dc, reg(t1x), reg(sx), OPND_CREATE_INT8(0)));
            put(e, INSTR_CREATE_vpshufd(e->dc, reg(t1x), reg(t1x), OPND_CREATE_INT8(0)));
        } else if (opc == OP_vpbroadcastd) {
            put(e, INSTR_CREATE_vpshufd(e->dc, reg(t1x), reg(sx), OPND_CREATE_INT8(0)));
        } else {
            put(e, INSTR_CREATE_vmovddup(e->dc, reg(t1x), reg(sx)));
        }
    } else {
        if (ab) put(e, INSTR_CREATE_vextractf128(e->dc, reg(t0x), reg(sv), OPND_CREATE_INT8(1)));
        else op_128(e, OP_vpsrldq, t0x, reg(sx), OPND_CREATE_INT8(w->hi_shift));   /* DR lists the immediate first */
        op_1(e, opc, t0x, reg(t0x));
        op_1(e, opc, t1x, reg(sx));
    }
    bool ydst = reg_is_ymm(d);
    if (ydst) put(e, INSTR_CREATE_vinsertf128(e->dc, reg(t1), reg(t1), reg(bc ? t1x : t0x), OPND_CREATE_INT8(1)));
    if (mem) reload_y(e, 2);
    reload_y(e, 0);
    /* commit; a VEX.128 write zeroes the destination's upper half */
    put(e, INSTR_CREATE_vmovdqu(e->dc, reg(d), reg(ydst ? t1 : t1x)));
    instrlist_set_translation_target(e->bb, decode_next_pc(e->dc, e->pc));
    reload_y(e, 1);
}

static void refuse_call(app_pc pc, int opc) {
    void *dc = dr_get_current_drcontext();
    char text[ALLOW_TEXT_MAX] = "?";
    instr_t in;
    instr_init(dc, &in);
    if (decode(dc, pc, &in) != NULL && instr_valid(&in)) allow_key(dc, &in, text);
    instr_free(dc, &in);
    dr_fprintf(STDERR, "v3compat: refusing %s at %p: not on the audited list or not emulated (%s)\n",
               decode_opcode_name(opc), pc, text);
    dr_exit_process(V3COMPAT_REFUSED);
}

/* ---- Vector helpers, through the raw-TLS frame ---- */

/* How a form's DR operands (immediate set aside, the rest in order r0 r1 r2) map to vec_exec's a, b, c. */
enum {
    K_3,     /* a=r0 b=r1; plus an imm for the 3-operand imm forms (palignr, blend, insert) */
    K_SH,    /* a=r0, count b=r1, or the immediate as the count */
    K_BI,    /* b=r0, imm (pshufd, permq) */
    K_AI,    /* a=r0, imm (pslldq) */
    K_A,     /* a=r0 (pabs) */
    K_B,     /* b=r0 (broadcast, pmovzx/sx) */
    K_4,     /* a=r0 b=r1 c=r2 (pblendvb) */
    K_FMA,   /* a=r0 b=r1 c=old destination; MXCSR and pending exceptions */
    K_MSK,   /* b=r0, result to a GPR */
};
typedef struct { int opc; vex_op op; uint8_t kind, type; } form_t;

#define F3(n, op) {OP_##n, op, K_3, 0}
#define FMA2(n, op) {OP_##n##ps, op, K_FMA, FT_PS}, {OP_##n##pd, op, K_FMA, FT_PD}
#define FMA4(n, op) {OP_##n##ps, op, K_FMA, FT_PS}, {OP_##n##pd, op, K_FMA, FT_PD}, \
                    {OP_##n##ss, op, K_FMA, FT_SS}, {OP_##n##sd, op, K_FMA, FT_SD}
/* The ymm and AVX2-only forms; vec_exec's operand convention is in vexops.h. */
static const form_t vec_forms[] = {
    F3(vpaddb, VPADDB), F3(vpaddw, VPADDW), F3(vpaddd, VPADDD), F3(vpaddq, VPADDQ),
    F3(vpsubb, VPSUBB), F3(vpsubw, VPSUBW), F3(vpsubd, VPSUBD), F3(vpsubq, VPSUBQ),
    F3(vpaddsb, VPADDSB), F3(vpaddsw, VPADDSW), F3(vpaddusb, VPADDUSB), F3(vpaddusw, VPADDUSW),
    F3(vpsubsb, VPSUBSB), F3(vpsubsw, VPSUBSW), F3(vpsubusb, VPSUBUSB), F3(vpsubusw, VPSUBUSW),
    F3(vpand, VPAND), F3(vpandn, VPANDN), F3(vpor, VPOR), F3(vpxor, VPXOR),
    F3(vpcmpeqb, VPCMPEQB), F3(vpcmpeqw, VPCMPEQW), F3(vpcmpeqd, VPCMPEQD), F3(vpcmpeqq, VPCMPEQQ),
    F3(vpcmpgtb, VPCMPGTB), F3(vpcmpgtw, VPCMPGTW), F3(vpcmpgtd, VPCMPGTD), F3(vpcmpgtq, VPCMPGTQ),
    F3(vpminub, VPMINUB), F3(vpminuw, VPMINUW), F3(vpminud, VPMINUD),
    F3(vpminsb, VPMINSB), F3(vpminsw, VPMINSW), F3(vpminsd, VPMINSD),
    F3(vpmaxub, VPMAXUB), F3(vpmaxuw, VPMAXUW), F3(vpmaxud, VPMAXUD),
    F3(vpmaxsb, VPMAXSB), F3(vpmaxsw, VPMAXSW), F3(vpmaxsd, VPMAXSD),
    F3(vpmullw, VPMULLW), F3(vpmulld, VPMULLD), F3(vpmulhw, VPMULHW), F3(vpmulhuw, VPMULHUW),
    F3(vpmulhrsw, VPMULHRSW), F3(vpmuldq, VPMULDQ), F3(vpmuludq, VPMULUDQ),
    F3(vpmaddwd, VPMADDWD), F3(vpmaddubsw, VPMADDUBSW),
    F3(vpavgb, VPAVGB), F3(vpavgw, VPAVGW), F3(vpsadbw, VPSADBW),
    F3(vpsignb, VPSIGNB), F3(vpsignw, VPSIGNW), F3(vpsignd, VPSIGND), F3(vphaddw, VPHADDW), F3(vphaddd, VPHADDD), F3(vphaddsw, VPHADDSW),
    F3(vphsubw, VPHSUBW), F3(vphsubd, VPHSUBD), F3(vphsubsw, VPHSUBSW), F3(vmpsadbw, VMPSADBW),
    F3(vpshufb, VPSHUFB), F3(vpalignr, VPALIGNR), F3(vpblendw, VPBLENDW),
    F3(vpacksswb, VPACKSSWB), F3(vpackssdw, VPACKSSDW), F3(vpackuswb, VPACKUSWB), F3(vpackusdw, VPACKUSDW),
    F3(vpunpcklbw, VPUNPCKLBW), F3(vpunpckhbw, VPUNPCKHBW), F3(vpunpcklwd, VPUNPCKLWD),
    F3(vpunpckhwd, VPUNPCKHWD), F3(vpunpckldq, VPUNPCKLDQ), F3(vpunpckhdq, VPUNPCKHDQ),
    F3(vpunpcklqdq, VPUNPCKLQDQ), F3(vpunpckhqdq, VPUNPCKHQDQ),
    F3(vpermd, VPERMD), F3(vpermps, VPERMPS),
    F3(vpsllvd, VPSLLVD), F3(vpsllvq, VPSLLVQ), F3(vpsrlvd, VPSRLVD), F3(vpsrlvq, VPSRLVQ),
    F3(vpsravd, VPSRAVD),
    {OP_vpsllw, VPSLLW, K_SH, 0}, {OP_vpslld, VPSLLD, K_SH, 0}, {OP_vpsllq, VPSLLQ, K_SH, 0},
    {OP_vpsrlw, VPSRLW, K_SH, 0}, {OP_vpsrld, VPSRLD, K_SH, 0}, {OP_vpsrlq, VPSRLQ, K_SH, 0},
    {OP_vpsraw, VPSRAW, K_SH, 0}, {OP_vpsrad, VPSRAD, K_SH, 0},
    {OP_vpslldq, VPSLLDQ, K_AI, 0}, {OP_vpsrldq, VPSRLDQ, K_AI, 0},
    {OP_vpshufd, VPSHUFD, K_BI, 0}, {OP_vpshuflw, VPSHUFLW, K_BI, 0}, {OP_vpshufhw, VPSHUFHW, K_BI, 0},
    {OP_vpermq, VPERMQ, K_BI, 0}, {OP_vpermpd, VPERMPD, K_BI, 0},
    {OP_vpabsb, VPABSB, K_A, 0}, {OP_vpabsw, VPABSW, K_A, 0}, {OP_vpabsd, VPABSD, K_A, 0},
    {OP_vpbroadcastb, VPBROADCASTB, K_B, 0}, {OP_vpbroadcastw, VPBROADCASTW, K_B, 0},
    {OP_vpbroadcastd, VPBROADCASTD, K_B, 0}, {OP_vpbroadcastq, VPBROADCASTQ, K_B, 0},
    /* The register-source vbroadcastss/sd are AVX2 and broadcast like vpbroadcastd/q. */
    {OP_vbroadcastss, VPBROADCASTD, K_B, 0}, {OP_vbroadcastsd, VPBROADCASTQ, K_B, 0},
    {OP_vpmovzxbw, VPMOVZXBW, K_B, 0}, {OP_vpmovzxbd, VPMOVZXBD, K_B, 0}, {OP_vpmovzxbq, VPMOVZXBQ, K_B, 0},
    {OP_vpmovzxwd, VPMOVZXWD, K_B, 0}, {OP_vpmovzxwq, VPMOVZXWQ, K_B, 0}, {OP_vpmovzxdq, VPMOVZXDQ, K_B, 0},
    {OP_vpmovsxbw, VPMOVSXBW, K_B, 0}, {OP_vpmovsxbd, VPMOVSXBD, K_B, 0}, {OP_vpmovsxbq, VPMOVSXBQ, K_B, 0},
    {OP_vpmovsxwd, VPMOVSXWD, K_B, 0}, {OP_vpmovsxwq, VPMOVSXWQ, K_B, 0}, {OP_vpmovsxdq, VPMOVSXDQ, K_B, 0},
    {OP_vpblendvb, VPBLENDVB, K_4, 0},
    {OP_vpmovmskb, VPMOVMSKB, K_MSK, 0},
    FMA4(vfmadd132, VFMADD132), FMA4(vfmadd213, VFMADD213), FMA4(vfmadd231, VFMADD231),
    FMA4(vfmsub132, VFMSUB132), FMA4(vfmsub213, VFMSUB213), FMA4(vfmsub231, VFMSUB231),
    FMA4(vfnmadd132, VFNMADD132), FMA4(vfnmadd213, VFNMADD213), FMA4(vfnmadd231, VFNMADD231),
    FMA4(vfnmsub132, VFNMSUB132), FMA4(vfnmsub213, VFNMSUB213), FMA4(vfnmsub231, VFNMSUB231),
    FMA2(vfmaddsub132, VFMADDSUB132), FMA2(vfmaddsub213, VFMADDSUB213), FMA2(vfmaddsub231, VFMADDSUB231),
    FMA2(vfmsubadd132, VFMSUBADD132), FMA2(vfmsubadd213, VFMSUBADD213), FMA2(vfmsubadd231, VFMSUBADD231),
};

/* Scalars, through the mcontext: they are rare, and flags and registers commit atomically. */
static const form_t bmi_forms[] = {
    {OP_andn, BMI_ANDN}, {OP_blsi, BMI_BLSI}, {OP_blsr, BMI_BLSR}, {OP_blsmsk, BMI_BLSMSK},
    {OP_bzhi, BMI_BZHI}, {OP_bextr, BMI_BEXTR}, {OP_mulx, BMI_MULX}, {OP_pdep, BMI_PDEP},
    {OP_pext, BMI_PEXT}, {OP_rorx, BMI_RORX}, {OP_sarx, BMI_SARX}, {OP_shlx, BMI_SHLX},
    {OP_shrx, BMI_SHRX}, {OP_tzcnt, BMI_TZCNT}, {OP_lzcnt, BMI_LZCNT}, {OP_movbe, BMI_MOVBE},
};

static const form_t *find(const form_t *t, size_t n, int opc) {
    for (size_t i = 0; i < n; i++)
        if (t[i].opc == opc) return &t[i];
    return NULL;
}
#define FIND(t, opc) find((t), sizeof(t) / sizeof((t)[0]), (opc))

/* vec_call descriptor, 32 bits: op | type<<8 | imm<<16 | flags<<24 */
enum {
    D_MXCSR = 1,    /* FP form: run with the application's masks, OR the raised flags back */
    D_NARROW = 2,   /* VEX.128: stale upper lanes must not compute or raise flags; a mask has 16 bits */
    D_COUNT = 4,    /* the immediate is a shift count, in b */
    D_GPR = 8,      /* the result goes to out's first 8 bytes */
};

static uint32_t fbits(float f) {
    uint32_t u;
    __builtin_memcpy(&u, &f, 4);
    return u;
}

/* Operands for one vdivss that raises the highest-priority of the unmasked flags `ex` (as fp_fault in
 * avxemu's emulate.c): the application then gets the SIGFPE of that kind. */
static void pending_operands(tls_t *t, uint32_t ex) {
    uint32_t x, y;
    if (ex & 1) { x = fbits(0.0f); y = fbits(0.0f); }                  /* IE */
    else if (ex & 2) { x = 1; y = fbits(1.0f); }                       /* DE: a denormal */
    else if (ex & 4) { x = fbits(1.0f); y = fbits(0.0f); }             /* ZE */
    else if (ex & 8) { x = fbits(3e38f); y = fbits(1e-30f); }          /* OE */
    else if (ex & 16) { x = fbits(1e-30f); y = fbits(3e38f); }         /* UE */
    else { x = fbits(1.0f); y = fbits(3.0f); }                         /* PE */
    for (int i = 0; i < 32; i++) t->a.b[i] = t->b.b[i] = 0;
    for (int i = 0; i < 4; i++) {
        t->a.b[i] = (uint8_t)(x >> (8 * i));
        t->b.b[i] = (uint8_t)(y >> (8 * i));
    }
}

static void vec_call(uint64 d) {
    tls_t *t = my_tls();
    uint fl = (uint)(d >> 24);
    uint8_t imm = (uint8_t)(d >> 16);
    if ((fl & D_NARROW) && !(fl & D_GPR))
        for (int i = 16; i < 32; i++) t->a.b[i] = t->b.b[i] = t->c.b[i] = 0;
    if (fl & D_COUNT) {
        for (int i = 0; i < 32; i++) t->b.b[i] = 0;
        t->b.b[0] = imm;
    }
    uint32_t csr = 0, mx = 0x1F80;
    if (fl & D_MXCSR) {
        csr = __builtin_ia32_stmxcsr();
        mx = csr & ~0x3Fu;
        t->pending = 0;
    }
    uint64_t gpr = 0;
    int r = vec_exec((vex_op)(d & 0xff), (int)((d >> 8) & 3), &t->a, &t->b, &t->c, imm, &t->out, &gpr, &mx);
    uint32_t raised = mx & 0x3Fu;
    if ((fl & D_MXCSR) && raised) __builtin_ia32_ldmxcsr(csr | raised);
    if (r == AVXEMU_FP_PENDING) {
        uint32_t ex = raised & ~(csr >> 7) & 0x3Fu;
        if (ex == 0) dr_exit_process(V3COMPAT_REFUSED);
        pending_operands(t, ex);
        t->pending = ex;
        return;
    }
    if (r != 1) dr_exit_process(V3COMPAT_REFUSED);
    if (fl & D_GPR) {
        if (fl & D_NARROW) gpr &= 0xFFFFu;
        for (int i = 0; i < 8; i++) t->out.b[i] = (uint8_t)(gpr >> (8 * i));
    }
}

/* The lean call's callee: the descriptor comes through TLS, as the thunk passes nothing. */
__attribute__((visibility("hidden"), used)) void lean_entry(void) { vec_call(my_tls()->desc); }

/*
 * Frame helpers' call (ADR 0007): the fragment stores rsp in SAVE_SP, switches to this thread's helper stack
 * and calls here. Saved: what the SysV ABI lets lean_entry clobber (rax rcx rdx rsi rdi r8-r11, every ymm, as
 * a VEX.128 write zeroes the upper half) and the flags; DF is cleared for memcpy. MXCSR is not: vec_call
 * writes the raised flags to it on purpose. A pc here is outside the cache, so DynamoRIO defers signals and
 * does not take the thread as a safe spot, as for its own clean callee.
 */
__asm__(".text\n"
        ".p2align 4\n"
        ".type lean_thunk, @function\n"
        "lean_thunk:\n"
        "pushfq\n cld\n"
        "push %rax\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n"
        "push %r8\n push %r9\n push %r10\n push %r11\n"
        /* stack top 32-aligned: 8 (return) + 80 (pushes) + 520 is a multiple of 32 */
        "sub $520, %rsp\n"
        "vmovdqa %ymm0, 0(%rsp)\n vmovdqa %ymm1, 32(%rsp)\n vmovdqa %ymm2, 64(%rsp)\n vmovdqa %ymm3, 96(%rsp)\n"
        "vmovdqa %ymm4, 128(%rsp)\n vmovdqa %ymm5, 160(%rsp)\n vmovdqa %ymm6, 192(%rsp)\n vmovdqa %ymm7, 224(%rsp)\n"
        "vmovdqa %ymm8, 256(%rsp)\n vmovdqa %ymm9, 288(%rsp)\n vmovdqa %ymm10, 320(%rsp)\n vmovdqa %ymm11, 352(%rsp)\n"
        "vmovdqa %ymm12, 384(%rsp)\n vmovdqa %ymm13, 416(%rsp)\n vmovdqa %ymm14, 448(%rsp)\n vmovdqa %ymm15, 480(%rsp)\n"
        "vzeroupper\n"   /* every ymm is saved: the helper's legacy-SSE code then runs without transition penalties */
        "call lean_entry\n"
        "vmovdqa 0(%rsp), %ymm0\n vmovdqa 32(%rsp), %ymm1\n vmovdqa 64(%rsp), %ymm2\n vmovdqa 96(%rsp), %ymm3\n"
        "vmovdqa 128(%rsp), %ymm4\n vmovdqa 160(%rsp), %ymm5\n vmovdqa 192(%rsp), %ymm6\n vmovdqa 224(%rsp), %ymm7\n"
        "vmovdqa 256(%rsp), %ymm8\n vmovdqa 288(%rsp), %ymm9\n vmovdqa 320(%rsp), %ymm10\n vmovdqa 352(%rsp), %ymm11\n"
        "vmovdqa 384(%rsp), %ymm12\n vmovdqa 416(%rsp), %ymm13\n vmovdqa 448(%rsp), %ymm14\n vmovdqa 480(%rsp), %ymm15\n"
        "add $520, %rsp\n"
        "pop %r11\n pop %r10\n pop %r9\n pop %r8\n"
        "pop %rdi\n pop %rsi\n pop %rdx\n pop %rcx\n pop %rax\n"
        "popfq\n ret\n"
        ".size lean_thunk, .-lean_thunk\n");
extern char lean_thunk[] __attribute__((visibility("hidden")));

/* rsp is the only application state that differs inside this sequence; event_restore has it from SAVE_SP. */
static void insert_lean_call(emit_t *e, uint32_t desc) {
    put(e, INSTR_CREATE_mov_st(e->dc, SAVE_SP, reg(DR_REG_RSP)));
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(DR_REG_RSP), tls_opnd(offsetof(tls_t, stack), OPSZ_8)));
    put(e, INSTR_CREATE_mov_st(e->dc, tls_opnd(offsetof(tls_t, desc), OPSZ_4), OPND_CREATE_INT32((int)desc)));
    put(e, INSTR_CREATE_call_ind(e->dc, tls_opnd(offsetof(tls_t, thunk), OPSZ_8)));
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(DR_REG_RSP), SAVE_SP));
}

static void refuse_pending(void) {
    dr_fprintf(STDERR, "v3compat: the pending FP exception did not fault\n");
    dr_exit_process(V3COMPAT_REFUSED);
}

/* A vec_exec operand into the frame: a register's whole ymm (as emulate.c copies it), or a real load. */
static void stage(emit_t *e, opnd_t o, size_t off, bool count) {
    opnd_t slot = tls_opnd(off, OPSZ_32);
    if (opnd_is_memory_reference(o)) {
        if (count) opnd_set_size(&o, OPSZ_16);   /* a shift count is xmm/m128 even for ymm */
        reg_id_t t = e->scratch[2];
        save_y(e, 2);
        load_mem(e, o, t);
        put(e, INSTR_CREATE_vmovdqu(e->dc, slot, reg(t)));
        reload_y(e, 2);
    } else {
        put(e, INSTR_CREATE_vmovdqu(e->dc, slot, reg(ymm_of(opnd_get_reg(o)))));
    }
}

static bool vec_operand(opnd_t o) {
    return opnd_is_memory_reference(o) || (opnd_is_reg(o) && (reg_is_ymm(opnd_get_reg(o)) || reg_is_xmm(opnd_get_reg(o))));
}

/* After the call: if it left an unmasked exception, nothing is committed and the application gets
 * the SIGFPE at P from a vdivss that raises the same kind (pending_operands). */
static void insert_pending_check(emit_t *e) {
    instr_t *ok = INSTR_CREATE_label(e->dc);
    reg_id_t t = e->scratch[0], tx = xmm_of(t);
    put(e, INSTR_CREATE_mov_st(e->dc, SAVE_G(1), reg(DR_REG_RCX)));
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(DR_REG_RCX), tls_opnd(offsetof(tls_t, pending), OPSZ_8)));
    /* jrcxz reaches 127 bytes only, less than the block with its out-of-line call. */
    instr_t *skip = INSTR_CREATE_label(e->dc), *block = INSTR_CREATE_label(e->dc);
    put(e, INSTR_CREATE_jecxz(e->dc, opnd_create_instr(skip)));
    put(e, INSTR_CREATE_jmp(e->dc, opnd_create_instr(block)));
    put(e, skip);
    put(e, INSTR_CREATE_jmp(e->dc, opnd_create_instr(ok)));
    put(e, block);
    put(e, INSTR_CREATE_mov_st(e->dc, tls_opnd(offsetof(tls_t, pending), OPSZ_8), OPND_CREATE_INT32(0)));
    save_y(e, 0);
    put(e, INSTR_CREATE_vmovd(e->dc, reg(tx), tls_opnd(offsetof(tls_t, a), OPSZ_4)));
    put(e, INSTR_CREATE_vdivss(e->dc, reg(tx), reg(tx), tls_opnd(offsetof(tls_t, b), OPSZ_4)));
    dr_insert_clean_call_ex(e->dc, e->bb, e->where, (void *)refuse_pending, DR_CLEANCALL_ALWAYS_OUT_OF_LINE, 0);
    reload_y(e, 0);
    put(e, ok);
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(DR_REG_RCX), SAVE_G(1)));
}

/* Returns false if the operands are not the shape the form expects. */
static bool expand_vec(emit_t *e, instr_t *in, const form_t *f) {
    opnd_t r[3], dst = instr_get_dst(in, 0);
    int n = 0;
    bool has_imm = false;
    uint imm = 0;
    for (int i = 0; i < instr_num_srcs(in); i++) {
        opnd_t o = instr_get_src(in, i);
        if (opnd_is_immed_int(o)) {
            has_imm = true;
            imm = (uint)opnd_get_immed_int(o) & 0xff;
        } else if (n < 3 && vec_operand(o)) {
            r[n++] = o;
        } else {
            return false;
        }
    }
    uint fl = 0;
    int ia = -1, ib = -1, ic = -1;   /* which r feeds a, b, c */
    bool count = false;
    switch (f->kind) {
    case K_3: case K_FMA: ia = 0; ib = 1; break;
    case K_SH:
        ia = 0;
        if (has_imm) fl |= D_COUNT;
        else { ib = 1; count = true; }
        break;
    case K_BI: case K_B: ib = 0; break;
    case K_AI: case K_A: ia = 0; break;
    case K_4: ia = 0; ib = 1; ic = 2; break;
    case K_MSK: ib = 0; fl |= D_GPR; break;
    }
    int need = ic >= 0 ? 3 : ib >= 0 && ia >= 0 ? 2 : 1;
    if (n < need || !opnd_is_reg(dst)) return false;
    reg_id_t d = opnd_get_reg(dst);
    bool ydst = reg_is_ymm(d);
    if (f->kind == K_MSK ? !reg_is_gpr(d) : !(ydst || reg_is_xmm(d))) return false;
    if (f->kind == K_FMA) {
        fl |= D_MXCSR;
        if (!ydst) fl |= D_NARROW;
    }
    if (f->kind == K_MSK && !opnd_is_reg(r[0])) return false;
    if (f->kind == K_MSK && !reg_is_ymm(opnd_get_reg(r[0]))) fl |= D_NARROW;

    if (ia >= 0) stage(e, r[ia], offsetof(tls_t, a), false);
    if (ib >= 0) stage(e, r[ib], offsetof(tls_t, b), count);
    if (ic >= 0) stage(e, r[ic], offsetof(tls_t, c), false);
    if (f->kind == K_FMA) put(e, INSTR_CREATE_vmovdqu(e->dc, FRAME(c), reg(ymm_of(d))));
    uint32_t desc = (uint32_t)f->op | ((uint32_t)f->type << 8) | (imm << 16) | (fl << 24);
    insert_lean_call(e, desc);
    if (fl & D_MXCSR) insert_pending_check(e);
    /* Commit: one write. A VEX.128 load zeroes the destination's upper half. */
    if (f->kind == K_MSK)
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(reg_resize_to_opsz(d, OPSZ_4)), tls_opnd(offsetof(tls_t, out), OPSZ_4)));
    else if (ydst)
        put(e, INSTR_CREATE_vmovdqu(e->dc, reg(d), FRAME(out)));
    else
        put(e, INSTR_CREATE_vmovdqu(e->dc, reg(d), FRAME_X(out)));
    return true;
}

/* Source codes in a descriptor: a pointer-sized DR GPR id, or one of these. */
enum { S_NONE = 0xff, S_MEM = 0xfe, S_IMM = 0xfd, S_RDX = 0xfc };

static int src_code(opnd_t o) {
    if (opnd_is_memory_reference(o)) return S_MEM;
    if (opnd_is_immed_int(o)) return S_IMM;
    return (int)reg_to_pointer_sized(opnd_get_reg(o));
}

static uint64 src_value(int code, uint imm, const tls_t *t, dr_mcontext_t *mc) {
    switch (code) {
    case S_NONE: return 0;
    case S_MEM: return *(const uint64 *)t->b.b;
    case S_IMM: return imm;
    case S_RDX: return mc->xdx;
    default: return reg_get_value((reg_id_t)code, mc);
    }
}

/* A 16-bit op writes only the low word; 32-bit writes zero-extend (avxemu_helpers.h). */
static void write_gpr(reg_id_t r, int opsize, uint64 v, dr_mcontext_t *mc) {
    if (opsize == 16) v = (reg_get_value(r, mc) & ~0xFFFFull) | (v & 0xFFFF);
    reg_set_value(r, mc, v);
}

/* d: op | opsize<<8 | s1<<16 | s2<<24 | dst<<32 | dst2<<40 | imm<<48 */
static void bmi_call(uint64 d) {
    void *dc = dr_get_current_drcontext();
    dr_mcontext_t mc;
    mc.size = sizeof mc;
    mc.flags = DR_MC_ALL;
    dr_get_mcontext(dc, &mc);
    tls_t *t = my_tls();
    int opsize = (d >> 8) & 0xff;
    uint imm = (d >> 48) & 0xff;
    uint64 s1 = src_value((d >> 16) & 0xff, imm, t, &mc), s2 = src_value((d >> 24) & 0xff, imm, t, &mc);
    uint64_t dst = 0, dst2 = 0, flags = mc.xflags;
    if (bmi_exec((vex_op)(d & 0xff), opsize, s1, s2, &dst, &dst2, &flags) != 1)
        dr_exit_process(V3COMPAT_REFUSED);
    write_gpr((reg_id_t)((d >> 32) & 0xff), opsize, dst, &mc);
    if (((d >> 40) & 0xff) != S_NONE) write_gpr((reg_id_t)((d >> 40) & 0xff), opsize, dst2, &mc);
    mc.xflags = flags;
    dr_set_mcontext(dc, &mc);
}

/* movbe to memory: the store is the commit, and if it faults event_restore has the scratch. */
static void expand_movbe_store(emit_t *e, instr_t *in) {
    opnd_t m = instr_get_dst(in, 0);
    reg_id_t src = reg_to_pointer_sized(opnd_get_reg(instr_get_src(in, 0)));
    uint size = opnd_size_in_bytes(opnd_get_size(m));
    bool abs = needs_abs(m);
    reg_id_t g = DR_REG_NULL, addr = DR_REG_NULL;
    /* rax..rbx: the ones with an addressable high byte, for the 16-bit swap. */
    for (reg_id_t c = DR_REG_RAX; c <= DR_REG_RBX; c++) {
        if (opnd_uses_reg(m, c) || c == src) continue;
        if (g == DR_REG_NULL) g = c;
        else if (addr == DR_REG_NULL) addr = c;
    }
    bool seg = uses_seg(m);
    if (abs || seg) put(e, INSTR_CREATE_mov_st(e->dc, SAVE_G(1), reg(addr)));
    if (abs) {
        instrlist_insert_mov_immed_ptrsz(e->dc, (ptr_int_t)opnd_get_addr(m), reg(addr), e->bb, e->where, NULL, NULL);
        m = opnd_create_base_disp(addr, DR_REG_NULL, 0, 0, opnd_get_size(m));
    } else if (seg) {
        m = unseg(e, m, addr);
    }
    put(e, INSTR_CREATE_mov_st(e->dc, SAVE_G(0), reg(g)));
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(g), reg(src)));
    if (size == 2) {
        /* xchg of the byte registers leaves flags alone, unlike a 16-bit rotate. */
        reg_id_t lo = reg_resize_to_opsz(g, OPSZ_1);
        put(e, INSTR_CREATE_xchg(e->dc, reg((reg_id_t)(lo - DR_REG_AL + DR_REG_AH)), reg(lo)));
    } else {
        put(e, INSTR_CREATE_bswap(e->dc, reg(size == 8 ? g : reg_resize_to_opsz(g, OPSZ_4))));
    }
    put(e, INSTR_CREATE_mov_st(e->dc, m, reg(reg_resize_to_opsz(g, opnd_get_size(m)))));
    instrlist_set_translation_target(e->bb, decode_next_pc(e->dc, e->pc));
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(g), SAVE_G(0)));
    if (abs || seg) put(e, INSTR_CREATE_mov_ld(e->dc, reg(addr), SAVE_G(1)));
}

static void expand_bmi(emit_t *e, instr_t *in, const form_t *f) {
    int opc = instr_get_opcode(in);
    if (opc == OP_movbe && opnd_is_memory_reference(instr_get_dst(in, 0))) {
        expand_movbe_store(e, in);
        return;
    }
    reg_id_t dst = opnd_get_reg(instr_get_dst(in, 0));
    int opsize = (int)opnd_size_in_bytes(reg_get_size(dst)) * 8;
    int d1 = (int)reg_to_pointer_sized(dst), d2 = S_NONE, s1, s2 = S_NONE;
    uint imm = 0;
    if (opc == OP_mulx) {
        /* DR: dst0 low (vvvv), dst1 high, src0 rm. bmi_exec: *dst low, *dst2 high, s1 rdx. */
        d2 = (int)reg_to_pointer_sized(opnd_get_reg(instr_get_dst(in, 1)));
        s1 = S_RDX;
        s2 = src_code(instr_get_src(in, 0));
    } else {
        s1 = src_code(instr_get_src(in, 0));
        /* DR lists pdep/pext as (rm, vvvv); bmi_exec wants (vvvv, rm). */
        if (opc == OP_pdep || opc == OP_pext) {
            s1 = src_code(instr_get_src(in, 1));
            s2 = src_code(instr_get_src(in, 0));
        } else if (opc != OP_blsi && opc != OP_blsr && opc != OP_blsmsk && opc != OP_tzcnt && opc != OP_lzcnt &&
            opc != OP_movbe)
            s2 = src_code(instr_get_src(in, 1));
    }
    if (opc == OP_rorx) imm = (uint)opnd_get_immed_int(instr_get_src(in, 1)) & 0xff;
    for (int i = 0; i < instr_num_srcs(in); i++) {
        opnd_t o = instr_get_src(in, i);
        if (opnd_is_memory_reference(o)) {
            save_y(e, 2);
            load_mem(e, o, e->scratch[2]);
            put(e, INSTR_CREATE_vmovdqu(e->dc, FRAME_X(b), reg(xmm_of(e->scratch[2]))));
            reload_y(e, 2);
            break;
        }
    }
    uint64 d = (uint64)f->op | ((uint64)opsize << 8) | ((uint64)s1 << 16) | ((uint64)s2 << 24) |
        ((uint64)d1 << 32) | ((uint64)d2 << 40) | ((uint64)imm << 48);
    dr_insert_clean_call_ex(e->dc, e->bb, e->where, (void *)bmi_call,
                            DR_CLEANCALL_ALWAYS_OUT_OF_LINE | DR_CLEANCALL_READS_APP_CONTEXT |
                                DR_CLEANCALL_WRITES_APP_CONTEXT,
                            1, OPND_CREATE_INT64(d));
}

/* ---- Scalar BMI inline: GPR scratch saved in raw TLS, one faulting load first, one commit ---- */

static const reg_id_t gp_cand[] = {DR_REG_RCX, DR_REG_RBX, DR_REG_RSI, DR_REG_RDI, DR_REG_R8, DR_REG_R9,
                                   DR_REG_R10, DR_REG_R11, DR_REG_R12, DR_REG_R13, DR_REG_R14, DR_REG_R15};
/* The registers whose low word has an addressable high byte, for the 16-bit swap. */
static const reg_id_t gp_legacy[] = {DR_REG_RAX, DR_REG_RCX, DR_REG_RDX, DR_REG_RBX};

static bool reg_in_instr(instr_t *in, reg_id_t r) {
    for (int i = 0; i < instr_num_srcs(in); i++)
        if (opnd_uses_reg(instr_get_src(in, i), r)) return true;
    for (int i = 0; i < instr_num_dsts(in); i++)
        if (opnd_uses_reg(instr_get_dst(in, i), r)) return true;
    return false;
}

static reg_id_t pick_gp(instr_t *in, const reg_id_t *c, int n, reg_id_t skip) {
    for (int i = 0; i < n; i++)
        if (c[i] != skip && !reg_in_instr(in, c[i])) return c[i];
    return DR_REG_NULL;   /* an instruction names five registers at most */
}
#define PICK(in, skip) pick_gp((in), gp_cand, sizeof gp_cand / sizeof gp_cand[0], (skip))

static reg_id_t gsz(reg_id_t r, uint bytes) { return reg_resize_to_opsz(r, bytes == 8 ? OPSZ_8 : OPSZ_4); }
static void save_g(emit_t *e, int i, reg_id_t r) { put(e, INSTR_CREATE_mov_st(e->dc, SAVE_G(i), reg(r))); }
static void load_g(emit_t *e, reg_id_t r, int i) { put(e, INSTR_CREATE_mov_ld(e->dc, reg(r), SAVE_G(i))); }
static void to_next_pc(emit_t *e) { instrlist_set_translation_target(e->bb, decode_next_pc(e->dc, e->pc)); }

/* One real load of m, `bytes` wide (a word zero-extends), into t: the only instruction that may fault. */
static void load_gpr(emit_t *e, opnd_t m, reg_id_t t, uint bytes) {
    if (needs_abs(m)) {   /* the application's address may be out of rip reach from the code cache */
        instrlist_insert_mov_immed_ptrsz(e->dc, (ptr_int_t)opnd_get_addr(m), reg(t), e->bb, e->where, NULL, NULL);
        m = opnd_create_base_disp(t, DR_REG_NULL, 0, 0, opnd_get_size(m));
    } else if (uses_seg(m)) {
        m = unseg(e, m, t);
    }
    if (bytes == 2) put(e, INSTR_CREATE_movzx(e->dc, reg(gsz(t, 4)), m));
    else put(e, INSTR_CREATE_mov_ld(e->dc, reg(gsz(t, bytes)), m));
}
/* An r/m source into t. */
static void fetch(emit_t *e, opnd_t s, reg_id_t t, uint bytes) {
    if (opnd_is_memory_reference(s)) load_gpr(e, s, t, bytes);
    else put(e, INSTR_CREATE_mov_ld(e->dc, reg(gsz(t, bytes)), s));
}

/*
 * The arithmetic flags around a sequence that clobbers them (rorx, mulx). Always saved, never
 * elided by liveness: DynamoRIO's flag table says shl $0, a variable shift or rep with rcx 0 write
 * flags that stay as they were, so a following writer would make a restore look dead. rax holds
 * the image only between lahf and the store, and event_restore puts the flags back from `fl`.
 */
static void flags_save(emit_t *e) {
    put(e, INSTR_CREATE_lahf(e->dc));
    put(e, INSTR_CREATE_setcc(e->dc, OP_seto, reg(DR_REG_AL)));
    put(e, INSTR_CREATE_mov_st(e->dc, tls_opnd(offsetof(tls_t, fl), OPSZ_8), reg(DR_REG_RAX)));
}
static void flags_restore(emit_t *e) {
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(DR_REG_RAX), tls_opnd(offsetof(tls_t, fl), OPSZ_8)));
    put(e, INSTR_CREATE_add(e->dc, reg(DR_REG_AL), OPND_CREATE_INT8(0x7f)));   /* OF from al */
    put(e, INSTR_CREATE_sahf(e->dc));
}

static void expand_rorx(emit_t *e, instr_t *in) {
    reg_id_t d = opnd_get_reg(instr_get_dst(in, 0));
    uint bytes = opnd_size_in_bytes(reg_get_size(d));
    uint cnt = (uint)opnd_get_immed_int(instr_get_src(in, 1)) & (bytes * 8 - 1);
    reg_id_t t = PICK(in, DR_REG_NULL), tt = gsz(t, bytes);
    save_g(e, 0, DR_REG_RAX);
    save_g(e, 1, t);
    fetch(e, instr_get_src(in, 0), t, bytes);
    if (cnt != 0) {
        flags_save(e);
        put(e, INSTR_CREATE_ror(e->dc, reg(tt), OPND_CREATE_INT8((int)cnt)));
        flags_restore(e);
    }
    load_g(e, DR_REG_RAX, 0);
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(d), reg(tt)));   /* commit */
    to_next_pc(e);
    load_g(e, t, 1);
}

/* andn writes SF and ZF from the result and clears CF and OF, which and does; AF and PF are undefined. */
static void expand_andn(emit_t *e, instr_t *in) {
    reg_id_t d = opnd_get_reg(instr_get_dst(in, 0));
    uint bytes = opnd_size_in_bytes(reg_get_size(d));
    opnd_t s1 = instr_get_src(in, 0), s2 = instr_get_src(in, 1);
    reg_id_t t = PICK(in, DR_REG_NULL), tt = gsz(t, bytes), t2 = DR_REG_NULL;
    save_g(e, 0, t);
    if (opnd_is_memory_reference(s2)) {
        t2 = PICK(in, t);
        reg_id_t tt2 = gsz(t2, bytes);
        save_g(e, 1, t2);
        load_gpr(e, s2, t, bytes);
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(tt2), s1));
        put(e, INSTR_CREATE_not(e->dc, reg(tt2)));
        put(e, INSTR_CREATE_and(e->dc, reg(tt), reg(tt2)));
    } else {
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(tt), s1));
        put(e, INSTR_CREATE_not(e->dc, reg(tt)));
        put(e, INSTR_CREATE_and(e->dc, reg(tt), s2));
    }
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(d), reg(tt)));   /* commit */
    to_next_pc(e);
    load_g(e, t, 0);
    if (t2 != DR_REG_NULL) load_g(e, t2, 1);
}

/*
 * mulx lo, hi, src: rdx * src, through mul (rdx:rax), so rax, rdx, flags and two scratch are saved.
 * The source is copied before rax or rdx change, so it may be either. A 32-bit form multiplies the
 * zero-extended halves in 64 bits: the product is in rax. With lo == hi the high half is written.
 */
static void expand_mulx(emit_t *e, instr_t *in) {
    reg_id_t lo = opnd_get_reg(instr_get_dst(in, 0)), hi = opnd_get_reg(instr_get_dst(in, 1));
    opnd_t src = instr_get_src(in, 0);
    uint bytes = opnd_size_in_bytes(reg_get_size(lo));
    reg_id_t t = PICK(in, DR_REG_NULL), t2 = PICK(in, t);
    save_g(e, 0, DR_REG_RAX);
    save_g(e, 1, DR_REG_RDX);
    save_g(e, 2, t);
    save_g(e, 3, t2);
    fetch(e, src, t, bytes);
    flags_save(e);
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(gsz(DR_REG_RAX, bytes)), reg(gsz(DR_REG_RDX, bytes))));
    /* DR's mul macros take no 64-bit form: rdx:rax = rax * t. */
    put(e, instr_create_2dst_2src(e->dc, OP_mul, reg(DR_REG_RDX), reg(DR_REG_RAX), reg(t), reg(DR_REG_RAX)));
    if (bytes == 8) {
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(t), reg(DR_REG_RAX)));
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(t2), reg(DR_REG_RDX)));
    } else {
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(t2), reg(DR_REG_RAX)));
        put(e, INSTR_CREATE_shr(e->dc, reg(t2), OPND_CREATE_INT8(32)));
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(gsz(t, 4)), reg(DR_REG_EAX)));
    }
    flags_restore(e);
    load_g(e, DR_REG_RAX, 0);
    load_g(e, DR_REG_RDX, 1);
    reg_id_t lop = reg_to_pointer_sized(lo), hip = reg_to_pointer_sized(hi);
    if (lop == hip) {
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(hi), reg(gsz(t2, bytes))));   /* commit */
    } else {
        /* A write that clobbers an input goes last, so the first leaves the instruction re-executable. */
        bool lo_in = lop == DR_REG_RDX || opnd_uses_reg(src, lop);
        bool hi_in = hip == DR_REG_RDX || opnd_uses_reg(src, hip);
        bool hi_first = lo_in && !hi_in;
        reg_id_t fr = hi_first ? hi : lo, sr = hi_first ? lo : hi;
        reg_id_t fv = hi_first ? t2 : t, sv = hi_first ? t : t2;
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(fr), reg(gsz(fv, bytes))));
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(sr), reg(gsz(sv, bytes))));   /* commit */
    }
    to_next_pc(e);
    load_g(e, t, 2);
    load_g(e, t2, 3);
}

/* movbe from memory: swapped in a scratch, so the destination is written once and a fault writes nothing. */
static void expand_movbe_load(emit_t *e, instr_t *in) {
    reg_id_t d = opnd_get_reg(instr_get_dst(in, 0));
    uint bytes = opnd_size_in_bytes(reg_get_size(d));
    reg_id_t t = bytes == 2 ? pick_gp(in, gp_legacy, 4, DR_REG_NULL) : PICK(in, DR_REG_NULL);
    reg_id_t tt = bytes == 2 ? reg_resize_to_opsz(t, OPSZ_2) : gsz(t, bytes);
    save_g(e, 0, t);
    load_gpr(e, instr_get_src(in, 0), t, bytes);
    if (bytes == 2) {
        /* xchg of the byte registers leaves flags alone, unlike a 16-bit rotate. */
        reg_id_t lo = reg_resize_to_opsz(t, OPSZ_1);
        put(e, INSTR_CREATE_xchg(e->dc, reg((reg_id_t)(lo - DR_REG_AL + DR_REG_AH)), reg(lo)));
    } else {
        put(e, INSTR_CREATE_bswap(e->dc, reg(tt)));
    }
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(d), reg(tt)));   /* commit; a word merges into the destination */
    to_next_pc(e);
    load_g(e, t, 0);
}

/* A register that is neither a, b nor named by the instruction. */
static reg_id_t pick_not(instr_t *in, reg_id_t a, reg_id_t b) {
    for (size_t i = 0; i < sizeof gp_cand / sizeof gp_cand[0]; i++)
        if (gp_cand[i] != a && gp_cand[i] != b && !reg_in_instr(in, gp_cand[i])) return gp_cand[i];
    return DR_REG_NULL;
}

/*
 * shlx, shrx, sarx: a shift by cl, which masks the count as the instruction does. Flags are saved around it
 * as in rorx. The source is fetched (the one load) before rcx changes, the count is copied before rax
 * (flags) changes, and rax and rcx are restored before the commit, as the destination may be either.
 */
static void expand_shx(emit_t *e, instr_t *in) {
    int opc = instr_get_opcode(in);
    reg_id_t d = opnd_get_reg(instr_get_dst(in, 0));
    uint bytes = opnd_size_in_bytes(reg_get_size(d));
    reg_id_t cnt = reg_to_pointer_sized(opnd_get_reg(instr_get_src(in, 1)));
    reg_id_t t = pick_not(in, DR_REG_RCX, DR_REG_NULL), tt = gsz(t, bytes);
    save_g(e, 0, DR_REG_RAX);
    save_g(e, 1, DR_REG_RCX);
    save_g(e, 2, t);
    fetch(e, instr_get_src(in, 0), t, bytes);
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(DR_REG_ECX), reg(gsz(cnt, 4))));
    flags_save(e);
    opnd_t cl = reg(DR_REG_CL);
    put(e, opc == OP_shlx ? INSTR_CREATE_shl(e->dc, reg(tt), cl)
           : opc == OP_shrx ? INSTR_CREATE_shr(e->dc, reg(tt), cl) : INSTR_CREATE_sar(e->dc, reg(tt), cl));
    flags_restore(e);
    load_g(e, DR_REG_RAX, 0);
    load_g(e, DR_REG_RCX, 1);
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(d), reg(tt)));   /* commit */
    to_next_pc(e);
    load_g(e, t, 2);
}

/*
 * tzcnt, lzcnt: bsf/bsr plus a branch for a zero source (CF = source is zero, ZF = result is zero; the other
 * flags are undefined). The flags are written before the commit but by no instruction that reads them,
 * and the only load is first, so an interrupted expansion is re-executed from the start.
 */
static void expand_cnt(emit_t *e, instr_t *in) {
    bool lz = instr_get_opcode(in) == OP_lzcnt;
    reg_id_t d = opnd_get_reg(instr_get_dst(in, 0));
    uint bytes = opnd_size_in_bytes(reg_get_size(d));
    reg_id_t t = PICK(in, DR_REG_NULL), tt = gsz(t, bytes);
    instr_t *zero = INSTR_CREATE_label(e->dc), *done = INSTR_CREATE_label(e->dc);
    save_g(e, 0, t);
    fetch(e, instr_get_src(in, 0), t, bytes);
    put(e, lz ? INSTR_CREATE_bsr(e->dc, reg(tt), reg(tt)) : INSTR_CREATE_bsf(e->dc, reg(tt), reg(tt)));
    put(e, INSTR_CREATE_jcc(e->dc, OP_jz, opnd_create_instr(zero)));
    if (lz) put(e, INSTR_CREATE_xor(e->dc, reg(tt), OPND_CREATE_INT8((int)bytes * 8 - 1)));
    put(e, INSTR_CREATE_test(e->dc, reg(tt), reg(tt)));
    put(e, INSTR_CREATE_jmp(e->dc, opnd_create_instr(done)));
    put(e, zero);
    put(e, INSTR_CREATE_mov_imm(e->dc, reg(gsz(t, 4)), OPND_CREATE_INT32((int)bytes * 8)));
    put(e, INSTR_CREATE_test(e->dc, reg(tt), reg(tt)));
    put(e, INSTR_CREATE_stc(e->dc));
    put(e, done);
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(d), reg(tt)));   /* commit */
    to_next_pc(e);
    load_g(e, t, 0);
}

/*
 * bzhi: below the operand width, src & ((1 << idx) - 1) (ZF, SF from it, CF and OF clear); otherwise src
 * unchanged with CF set. Branch on the width as tzcnt does.
 */
static void expand_bzhi(emit_t *e, instr_t *in) {
    reg_id_t d = opnd_get_reg(instr_get_dst(in, 0));
    uint bytes = opnd_size_in_bytes(reg_get_size(d));
    reg_id_t idx = reg_to_pointer_sized(opnd_get_reg(instr_get_src(in, 1)));
    reg_id_t t = pick_not(in, DR_REG_RCX, DR_REG_NULL), t2 = pick_not(in, DR_REG_RCX, t);
    reg_id_t tt = gsz(t, bytes), tt2 = gsz(t2, bytes);
    instr_t *big = INSTR_CREATE_label(e->dc), *done = INSTR_CREATE_label(e->dc);
    save_g(e, 0, t);
    save_g(e, 1, t2);
    save_g(e, 2, DR_REG_RCX);
    fetch(e, instr_get_src(in, 0), t, bytes);
    put(e, INSTR_CREATE_movzx(e->dc, reg(DR_REG_ECX), reg(reg_resize_to_opsz(idx, OPSZ_1))));
    put(e, INSTR_CREATE_cmp(e->dc, reg(DR_REG_ECX), OPND_CREATE_INT8((int)bytes * 8)));
    put(e, INSTR_CREATE_jcc(e->dc, OP_jae, opnd_create_instr(big)));
    put(e, INSTR_CREATE_mov_imm(e->dc, reg(gsz(t2, 4)), OPND_CREATE_INT32(1)));
    put(e, INSTR_CREATE_shl(e->dc, reg(tt2), reg(DR_REG_CL)));
    put(e, INSTR_CREATE_dec(e->dc, reg(tt2)));
    put(e, INSTR_CREATE_and(e->dc, reg(tt), reg(tt2)));
    put(e, INSTR_CREATE_jmp(e->dc, opnd_create_instr(done)));
    put(e, big);
    put(e, INSTR_CREATE_test(e->dc, reg(tt), reg(tt)));
    put(e, INSTR_CREATE_stc(e->dc));
    put(e, done);
    load_g(e, DR_REG_RCX, 2);
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(d), reg(tt)));   /* commit */
    to_next_pc(e);
    load_g(e, t, 0);
    load_g(e, t2, 1);
}

static bool gpr_opnd(opnd_t o) { return opnd_is_reg(o) && reg_is_gpr(opnd_get_reg(o)); }
static bool rm_opnd(opnd_t o) { return gpr_opnd(o) || opnd_is_memory_reference(o); }

/* The forms with an inline sequence; anything else takes the helper. */
static bool bmi_inline_ok(instr_t *in) {
    int ns = instr_num_srcs(in), nd = instr_num_dsts(in);
    if (nd < 1 || !gpr_opnd(instr_get_dst(in, 0))) return false;
    uint bytes = opnd_size_in_bytes(reg_get_size(opnd_get_reg(instr_get_dst(in, 0))));
    switch (instr_get_opcode(in)) {
    case OP_rorx:
        return nd == 1 && ns == 2 && (bytes == 4 || bytes == 8) && rm_opnd(instr_get_src(in, 0)) &&
               opnd_is_immed_int(instr_get_src(in, 1));
    case OP_andn:
        return nd == 1 && ns == 2 && (bytes == 4 || bytes == 8) && gpr_opnd(instr_get_src(in, 0)) &&
               rm_opnd(instr_get_src(in, 1));
    case OP_mulx:
        return nd == 2 && ns >= 1 && (bytes == 4 || bytes == 8) && gpr_opnd(instr_get_dst(in, 1)) &&
               rm_opnd(instr_get_src(in, 0));
    case OP_movbe:
        return nd == 1 && ns == 1 && opnd_is_memory_reference(instr_get_src(in, 0));
    case OP_shlx: case OP_shrx: case OP_sarx: case OP_bzhi:
        return nd == 1 && ns == 2 && (bytes == 4 || bytes == 8) && rm_opnd(instr_get_src(in, 0)) &&
               gpr_opnd(instr_get_src(in, 1));
    case OP_tzcnt: case OP_lzcnt:
        return nd == 1 && ns == 1 && (bytes == 4 || bytes == 8) && rm_opnd(instr_get_src(in, 0));
    }
    return false;
}

/*
 * Lane-crossing forms and vpmovmskb, inline from AVX1 operations. A memory source is read once, by one real
 * load; scratch is saved and restored as in expand_inline, and the destination is written last.
 *   vpermq/vpermpd imm: both lanes of [L,L] and [H,H] permuted within the lane by the selector's low bits
 *     (vpermilpd), then blended by its high bits (vblendpd); the immediates are worked out here.
 *   vpermd/vpermps: the same with the index register as vpermilps's control (bits 1:0) and vblendvps on
 *     index bit 2, moved to the sign bit by a shift of each half.
 *   vpshufd/vpshufhw/vpshuflw: the xmm form on each half.
 *   vpmovmskb: each half's 16 bits through a GPR scratch, joined in an xmm by vpinsrw: no flags change.
 */
static bool cross_form(int opc) {
    switch (opc) {
    case OP_vpermd: case OP_vpermps: case OP_vpermq: case OP_vpermpd:
    case OP_vpshufd: case OP_vpshufhw: case OP_vpshuflw: case OP_vpmovmskb: return true;
    }
    return false;
}

static bool ymm_or_mem(opnd_t o) {
    return opnd_is_reg(o) ? reg_is_ymm(opnd_get_reg(o)) : opnd_is_memory_reference(o);
}

/* DR's operands: vpermd (index, source); the immediate forms (source, imm); vpmovmskb (ymm source). */
static bool cross_ok(instr_t *in) {
    int opc = instr_get_opcode(in);
    if (instr_num_dsts(in) != 1 || !opnd_is_reg(instr_get_dst(in, 0))) return false;
    reg_id_t d = opnd_get_reg(instr_get_dst(in, 0));
    if (opc == OP_vpmovmskb)
        return instr_num_srcs(in) == 1 && reg_is_gpr(d) && opnd_is_reg(instr_get_src(in, 0)) &&
            reg_is_ymm(opnd_get_reg(instr_get_src(in, 0)));
    if (!reg_is_ymm(d) || instr_num_srcs(in) != 2) return false;
    opnd_t a = instr_get_src(in, 0), b = instr_get_src(in, 1);
    if (opc == OP_vpermd || opc == OP_vpermps)
        return opnd_is_reg(a) && reg_is_ymm(opnd_get_reg(a)) && ymm_or_mem(b);
    return ymm_or_mem(a) && opnd_is_immed_int(b);
}

static void op_imm(emit_t *e, int opc, reg_id_t d, reg_id_t s, uint imm) {
    instr_t *i = instr_build(e->dc, opc, 1, 2);
    instr_set_dst(i, 0, reg(d));
    instr_set_src(i, 0, reg(s));
    instr_set_src(i, 1, OPND_CREATE_INT8((char)imm));
    put(e, i);
}

static void expand_msk(emit_t *e, instr_t *in) {
    reg_id_t d = reg_resize_to_opsz(opnd_get_reg(instr_get_dst(in, 0)), OPSZ_4);
    reg_id_t s = opnd_get_reg(instr_get_src(in, 0));
    reg_id_t g = PICK(in, DR_REG_NULL), g4 = reg_resize_to_opsz(g, OPSZ_4);
    reg_id_t t0x = xmm_of(e->scratch[0]), t1x = xmm_of(e->scratch[1]);
    save_y(e, 0);
    save_y(e, 1);
    put(e, INSTR_CREATE_mov_st(e->dc, SAVE_G(0), reg(g)));
    put(e, INSTR_CREATE_vpmovmskb(e->dc, reg(g4), reg(xmm_of(s))));
    put(e, INSTR_CREATE_vmovd(e->dc, reg(t1x), reg(g4)));
    put(e, INSTR_CREATE_vextractf128(e->dc, reg(t0x), reg(s), OPND_CREATE_INT8(1)));
    put(e, INSTR_CREATE_vpmovmskb(e->dc, reg(g4), reg(t0x)));
    put(e, INSTR_CREATE_vpinsrw(e->dc, reg(t1x), reg(t1x), reg(g4), OPND_CREATE_INT8(1)));
    put(e, INSTR_CREATE_mov_ld(e->dc, reg(g), SAVE_G(0)));
    reload_y(e, 0);
    put(e, INSTR_CREATE_vmovd(e->dc, reg(d), reg(t1x)));   /* commit; a 32-bit write zero-extends */
    instrlist_set_translation_target(e->bb, decode_next_pc(e->dc, e->pc));
    reload_y(e, 1);
}

static void expand_cross(emit_t *e, instr_t *in) {
    int opc = instr_get_opcode(in);
    if (opc == OP_vpmovmskb) {
        expand_msk(e, in);
        return;
    }
    reg_id_t d = opnd_get_reg(instr_get_dst(in, 0));
    reg_id_t t0 = e->scratch[0], t1 = e->scratch[1], t2 = e->scratch[2];
    reg_id_t t0x = xmm_of(t0), t1x = xmm_of(t1), t2x = xmm_of(t2);
    bool perm_d = opc == OP_vpermd || opc == OP_vpermps;
    opnd_t s = instr_get_src(in, perm_d ? 1 : 0);
    bool mem = opnd_is_memory_reference(s);
    save_y(e, 0);
    save_y(e, 1);
    if (perm_d || mem) save_y(e, 2);
    reg_id_t sv = mem ? (perm_d ? t0 : t2) : opnd_get_reg(s);
    if (perm_d) {
        /* index bit 2 to each dword's sign bit, in t2; t0 holds the high half meanwhile */
        reg_id_t idx = opnd_get_reg(instr_get_src(in, 0));
        op_128(e, OP_vpslld, t2x, reg(xmm_of(idx)), OPND_CREATE_INT8(29));
        put(e, INSTR_CREATE_vextractf128(e->dc, reg(t0x), reg(idx), OPND_CREATE_INT8(1)));
        op_128(e, OP_vpslld, t0x, reg(t0x), OPND_CREATE_INT8(29));
        put(e, INSTR_CREATE_vinsertf128(e->dc, reg(t2), reg(t2), reg(t0x), OPND_CREATE_INT8(1)));
        if (mem) load_mem(e, s, sv);
        put(e, INSTR_CREATE_vperm2f128(e->dc, reg(t1), reg(sv), reg(sv), OPND_CREATE_INT8(0x11)));
        put(e, INSTR_CREATE_vperm2f128(e->dc, reg(t0), reg(sv), reg(sv), OPND_CREATE_INT8(0x00)));
        put(e, INSTR_CREATE_vpermilps(e->dc, reg(t0), reg(t0), reg(idx)));
        put(e, INSTR_CREATE_vpermilps(e->dc, reg(t1), reg(t1), reg(idx)));
        put(e, INSTR_CREATE_vblendvps(e->dc, reg(t1), reg(t0), reg(t1), reg(t2)));
    } else {
        uint imm = (uint)opnd_get_immed_int(instr_get_src(in, 1)) & 0xff;
        if (mem) load_mem(e, s, sv);
        if (opc == OP_vpermq || opc == OP_vpermpd) {
            uint lo = 0, hi = 0;
            for (int i = 0; i < 4; i++) {
                lo |= ((imm >> (2 * i)) & 1) << i;
                hi |= ((imm >> (2 * i + 1)) & 1) << i;
            }
            put(e, INSTR_CREATE_vperm2f128(e->dc, reg(t0), reg(sv), reg(sv), OPND_CREATE_INT8(0x00)));
            put(e, INSTR_CREATE_vperm2f128(e->dc, reg(t1), reg(sv), reg(sv), OPND_CREATE_INT8(0x11)));
            op_imm(e, OP_vpermilpd, t0, t0, lo);
            op_imm(e, OP_vpermilpd, t1, t1, lo);
            put(e, INSTR_CREATE_vblendpd(e->dc, reg(t1), reg(t0), reg(t1), OPND_CREATE_INT8((char)hi)));
        } else {
            put(e, INSTR_CREATE_vextractf128(e->dc, reg(t0x), reg(sv), OPND_CREATE_INT8(1)));
            op_imm(e, opc, t0x, t0x, imm);
            op_imm(e, opc, t1x, xmm_of(sv), imm);
            put(e, INSTR_CREATE_vinsertf128(e->dc, reg(t1), reg(t1), reg(t0x), OPND_CREATE_INT8(1)));
        }
    }
    if (perm_d || mem) reload_y(e, 2);
    reload_y(e, 0);
    put(e, INSTR_CREATE_vmovdqu(e->dc, reg(d), reg(t1)));   /* commit */
    instrlist_set_translation_target(e->bb, decode_next_pc(e->dc, e->pc));
    reload_y(e, 1);
}

static void expand_bmi_inline(emit_t *e, instr_t *in) {
    switch (instr_get_opcode(in)) {
    case OP_rorx: expand_rorx(e, in); break;
    case OP_andn: expand_andn(e, in); break;
    case OP_mulx: expand_mulx(e, in); break;
    case OP_shlx: case OP_shrx: case OP_sarx: expand_shx(e, in); break;
    case OP_bzhi: expand_bzhi(e, in); break;
    case OP_tzcnt: case OP_lzcnt: expand_cnt(e, in); break;
    default: expand_movbe_load(e, in); break;
    }
}

/*
 * AVX2 gathers: avxemu's archive never touches memory, so these are expanded here. Unrolled, one block per
 * element, so the translation scan in event_restore stays a straight read. The operands are copied to the
 * frame (a = index, b = mask, c = destination). Each active element: one real load (the only instruction
 * that may fault), then the element is stored to c, its mask element zeroed in b, and c and b are written back
 * to the registers. A fault at element k has therefore left k elements done and visible, and the gather
 * restarts from the registers as a signal handler's return leaves them. A masked-off element is not read, but its mask element is cleared.
 * Completion zeroes what the form leaves unused, as VEX does. Flags are saved and restored around the
 * tests (never elided), the scratch GPRs are saved in gsave. The same register twice among dst, index and
 * mask is #UD, as on hardware.
 */
static bool gather_shape(int opc, uint *esz, uint *isz) {
    switch (opc) {
    case OP_vpgatherdd: case OP_vgatherdps: *esz = 4; *isz = 4; return true;
    case OP_vpgatherdq: case OP_vgatherdpd: *esz = 8; *isz = 4; return true;
    case OP_vpgatherqd: case OP_vgatherqps: *esz = 4; *isz = 8; return true;
    case OP_vpgatherqq: case OP_vgatherqpd: *esz = 8; *isz = 8; return true;
    }
    return false;
}
static bool vreg(reg_id_t r) { return reg_is_xmm(r) || reg_is_ymm(r); }
static int vnum(reg_id_t r) { return reg_is_ymm(r) ? r - DR_REG_YMM0 : r - DR_REG_XMM0; }
static uint vbytes(reg_id_t r) { return reg_is_ymm(r) ? 32 : 16; }

/* A 0x67 prefix: the address wraps at 32 bits. DR's operand does not say so when there is no base. */
static bool gather_addr32(instr_t *in) {
    const byte *p = instr_get_raw_bits(in);
    for (int i = 0; i < 4 && p[i] != 0xc4 && p[i] != 0xc5; i++)
        if (p[i] == 0x67) return true;
    return false;
}

static bool gather_ok(instr_t *in) {
    uint es, is;
    if (!gather_shape(instr_get_opcode(in), &es, &is) || instr_num_dsts(in) != 2 || instr_num_srcs(in) != 2 ||
        !instr_raw_bits_valid(in))
        return false;
    opnd_t d = instr_get_dst(in, 0), m = instr_get_dst(in, 1), mem = instr_get_src(in, 0), ms = instr_get_src(in, 1);
    return opnd_is_reg(d) && opnd_is_reg(m) && opnd_is_reg(ms) && opnd_get_reg(m) == opnd_get_reg(ms) &&
           vreg(opnd_get_reg(d)) && vreg(opnd_get_reg(m)) && opnd_is_vsib(mem) && vreg(opnd_get_index(mem)) &&
           (opnd_get_base(mem) == DR_REG_NULL || reg_is_gpr(opnd_get_base(mem)));
}

static void expand_gather(emit_t *e, instr_t *in) {
    uint es, is;
    gather_shape(instr_get_opcode(in), &es, &is);
    reg_id_t dr = opnd_get_reg(instr_get_dst(in, 0)), mr = opnd_get_reg(instr_get_dst(in, 1));
    opnd_t mem = instr_get_src(in, 0);
    reg_id_t ir = opnd_get_index(mem), base = opnd_get_base(mem);
    if (vnum(dr) == vnum(mr) || vnum(dr) == vnum(ir) || vnum(mr) == vnum(ir)) {
        put(e, INSTR_CREATE_ud2a(e->dc));
        return;
    }
    uint count = vbytes(dr) / es < vbytes(ir) / is ? vbytes(dr) / es : vbytes(ir) / is;
    bool a32 = gather_addr32(in);
    reg_id_t t[4];
    int n = 0;
    for (size_t i = 0; i < sizeof gp_cand / sizeof gp_cand[0] && n < 4; i++)
        if (!reg_in_instr(in, gp_cand[i])) t[n++] = gp_cand[i];
    reg_id_t tm = t[0], ta = t[1], td = t[2], tb = t[3];   /* mask, address, data, copy of the base */
    save_g(e, 0, DR_REG_RAX);
    for (int i = 0; i < 4; i++) save_g(e, i + 1, t[i]);
    if (base != DR_REG_NULL) put(e, INSTR_CREATE_mov_ld(e->dc, reg(tb), reg(reg_to_pointer_sized(base))));
    flags_save(e);
    put(e, INSTR_CREATE_vmovdqu(e->dc, FRAME(a), reg(ymm_of(ir))));
    put(e, INSTR_CREATE_vmovdqu(e->dc, FRAME(b), reg(ymm_of(mr))));
    put(e, INSTR_CREATE_vmovdqu(e->dc, FRAME(c), reg(ymm_of(dr))));
    opnd_size_t esz = es == 8 ? OPSZ_8 : OPSZ_4;
    for (uint k = 0; k < count; k++) {
        opnd_t mk = tls_opnd(offsetof(tls_t, b) + k * es, esz), el = tls_opnd(offsetof(tls_t, c) + k * es, esz);
        opnd_t ix = tls_opnd(offsetof(tls_t, a) + k * is, is == 8 ? OPSZ_8 : OPSZ_4);
        instr_t *skip = INSTR_CREATE_label(e->dc);
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(gsz(tm, es)), mk));
        put(e, INSTR_CREATE_test(e->dc, reg(gsz(tm, es)), reg(gsz(tm, es))));
        put(e, INSTR_CREATE_jcc(e->dc, OP_jns, opnd_create_instr(skip)));
        if (is == 8) put(e, INSTR_CREATE_mov_ld(e->dc, reg(ta), ix));
        else put(e, INSTR_CREATE_movsxd(e->dc, reg(ta), ix));
        put(e, INSTR_CREATE_lea(e->dc, reg(ta),
                                opnd_create_base_disp(base == DR_REG_NULL ? DR_REG_NULL : tb, ta,
                                                      opnd_get_scale(mem), opnd_get_disp(mem), OPSZ_lea)));
        if (a32) put(e, INSTR_CREATE_mov_ld(e->dc, reg(gsz(ta, 4)), reg(gsz(ta, 4))));
        if (uses_seg(mem)) {   // a meta fs:/gs: would be DR's
            dr_insert_get_seg_base(e->dc, e->bb, e->where, opnd_get_segment(mem), td);
            put(e, INSTR_CREATE_add(e->dc, reg(ta), reg(td)));
        }
        opnd_t src = opnd_create_base_disp(ta, DR_REG_NULL, 0, 0, esz);
        put(e, INSTR_CREATE_mov_ld(e->dc, reg(gsz(td, es)), src));   /* the only instruction that may fault */
        put(e, INSTR_CREATE_mov_st(e->dc, el, reg(gsz(td, es))));
        put(e, skip);   // a masked-off element is not read, but its mask element is cleared as well
        put(e, INSTR_CREATE_mov_st(e->dc, mk, OPND_CREATE_INT32(0)));
        put(e, INSTR_CREATE_vmovdqu(e->dc, reg(ymm_of(dr)), FRAME(c)));
        put(e, INSTR_CREATE_vmovdqu(e->dc, reg(ymm_of(mr)), FRAME(b)));
    }
    for (uint off = count * es; off < 32; off += 8) {
        put(e, INSTR_CREATE_mov_st(e->dc, tls_opnd(offsetof(tls_t, b) + off, OPSZ_8), OPND_CREATE_INT32(0)));
        put(e, INSTR_CREATE_mov_st(e->dc, tls_opnd(offsetof(tls_t, c) + off, OPSZ_8), OPND_CREATE_INT32(0)));
    }
    flags_restore(e);
    load_g(e, DR_REG_RAX, 0);
    put(e, INSTR_CREATE_vmovdqu(e->dc, reg(ymm_of(dr)), FRAME(c)));   /* commit */
    put(e, INSTR_CREATE_vmovdqu(e->dc, reg(ymm_of(mr)), FRAME(b)));
    to_next_pc(e);
    for (int i = 0; i < 4; i++) load_g(e, t[i], i + 1);
}

/* cpuid: report x86-64-v3, on top of what the target CPU has. Hosts' extras are hidden, so a
 * program takes the same paths here as there, and none reaches AVX-512 on a capable host. */
static void cpuid_call(void) {
    void *dc = dr_get_current_drcontext();
    dr_mcontext_t mc;
    mc.size = sizeof mc;
    mc.flags = DR_MC_ALL;
    dr_get_mcontext(dc, &mc);
    uint32_t leaf = (uint32_t)mc.xax, sub = (uint32_t)mc.xcx, a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf), "c"(sub));
    if (leaf == 1) {
        c |= (1u << 12) | (1u << 22);   /* FMA, MOVBE */
    } else if (leaf == 7) {
        /* Kept: SMEP, ERMS, INVPCID. Set: BMI1, AVX2, BMI2. FSGSBASE is hidden: DR keeps its own TLS
         * in a segment and does not mangle rdgsbase or wrgsbase, and they are off the list. */
        b = sub != 0 ? 0 : (b & ((1u << 7) | (1u << 9) | (1u << 10))) | (1u << 3) | (1u << 5) | (1u << 8);
        a = c = d = 0;
    } else if (leaf == 0xd && sub == 1) {
        a &= 1u;                        /* XSAVEOPT kept; XSAVEC, XGETBV1, XSAVES hidden: Ivy Bridge lacks them, and glibc's resolver would pick them */
        c = d = 0;
    } else if (leaf == 0x80000001) {
        c = (c & 1u) | (1u << 5);       /* LAHF kept; LZCNT set */
    }
    mc.xax = a;
    mc.xbx = b;
    mc.xcx = c;
    mc.xdx = d;
    dr_set_mcontext(dc, &mc);
}

static bool all_mem_supported(instr_t *in) {
    for (int i = 0; i < instr_num_srcs(in); i++)
        if (opnd_is_memory_reference(instr_get_src(in, i)) && !mem_supported(instr_get_src(in, i))) return false;
    for (int i = 0; i < instr_num_dsts(in); i++)
        if (opnd_is_memory_reference(instr_get_dst(in, i)) && !mem_supported(instr_get_dst(in, i))) return false;
    return true;
}

static bool vec_shape_ok(instr_t *in) {
    if (instr_num_dsts(in) != 1 || instr_num_srcs(in) != 2) return false;
    opnd_t d = instr_get_dst(in, 0), a, b;
    vec_srcs(in, &a, &b);
    return opnd_is_reg(d) && reg_is_ymm(opnd_get_reg(d)) && opnd_is_reg(a) && reg_is_ymm(opnd_get_reg(a));
}

/* EVEX has no DR flag in this version: it is opcode 0x62 after any legacy padding. */
/*
 * AVX1 has the same operation under another opcode for these, faults and all, so the application
 * instruction stays and only its opcode changes. DR lists the store forms of the masked moves'
 * operands in the other order.
 */
static bool substitute(instr_t *in) {
    int to = OP_INVALID;
    bool mem_src = instr_num_srcs(in) > 0 && opnd_is_memory_reference(instr_get_src(in, 0));
    bool ydst = instr_num_dsts(in) > 0 && opnd_is_reg(instr_get_dst(in, 0)) && reg_is_ymm(opnd_get_reg(instr_get_dst(in, 0)));
    switch (instr_get_opcode(in)) {
    case OP_vextracti128: to = OP_vextractf128; break;
    case OP_vinserti128: to = OP_vinsertf128; break;
    case OP_vperm2i128: to = OP_vperm2f128; break;
    case OP_vbroadcasti128: to = OP_vbroadcastf128; break;
    case OP_vpmaskmovd: to = OP_vmaskmovps; break;
    case OP_vpmaskmovq: to = OP_vmaskmovpd; break;
    case OP_vpblendd: to = OP_vblendps; break;
    /* Bitwise: the same bits and faults as the float-domain forms (no alignment requirement under VEX). */
    case OP_vpand: if (ydst) to = OP_vandps; break;
    case OP_vpandn: if (ydst) to = OP_vandnps; break;
    case OP_vpor: if (ydst) to = OP_vorps; break;
    case OP_vpxor: if (ydst) to = OP_vxorps; break;
    case OP_vpbroadcastd: if (mem_src) to = OP_vbroadcastss; break;
    case OP_vpbroadcastq: if (mem_src && ydst) to = OP_vbroadcastsd; break;
    case OP_vmovntdqa: to = OP_vmovdqa; break;
    }
    if (to == OP_INVALID) return false;
    int from = instr_get_opcode(in);
    instr_set_opcode(in, to);
    if ((from == OP_vpmaskmovd || from == OP_vpmaskmovq) && opnd_is_memory_reference(instr_get_dst(in, 0))) {
        opnd_t s0 = instr_get_src(in, 0);
        instr_set_src(in, 0, instr_get_src(in, 1));
        instr_set_src(in, 1, s0);
    }
    return true;
}

void expand_refuse(void *dc, instrlist_t *bb, instr_t *in) {
    instrlist_set_translation_target(bb, instr_get_app_pc(in));
    dr_insert_clean_call(dc, bb, in, (void *)refuse_call, false, 2, OPND_CREATE_INTPTR(instr_get_app_pc(in)),
                         OPND_CREATE_INT32(instr_get_opcode(in)));
    instrlist_set_translation_target(bb, NULL);
    instrlist_remove(bb, in);
    instr_destroy(dc, in);
}

void expand_instr(void *dc, instrlist_t *bb, instr_t *in, bool lacked) {
    int opc = instr_get_opcode(in);
    if (!lacked && opc != OP_cpuid) return;
    if (lacked && !instr_is_evex_encoded(dc, in) && substitute(in)) return;
    emit_t e = {dc, bb, in, instr_get_app_pc(in), {DR_REG_NULL}};
    instrlist_set_translation_target(bb, e.pc);
    const form_t *f;
    if (opc == OP_cpuid) {
        dr_insert_clean_call_ex(dc, bb, in, (void *)cpuid_call,
                                DR_CLEANCALL_READS_APP_CONTEXT | DR_CLEANCALL_WRITES_APP_CONTEXT, 0);
    } else if (instr_is_evex_encoded(dc, in) || !all_mem_supported(in) ||
               /* XOP bextr shares the opcode, and its control is an immediate the helper does not take. */
               (opc == OP_bextr && opnd_is_immed(instr_get_src(in, 1)))) {
        dr_insert_clean_call(dc, bb, in, (void *)refuse_call, false, 2, OPND_CREATE_INTPTR(e.pc),
                             OPND_CREATE_INT32(opc));
    } else if (gather_ok(in)) {
        expand_gather(&e, in);
    } else if ((f = FIND(bmi_forms, opc)) != NULL) {
        pick_scratch(&e, in);
        if (!helpers_only && bmi_inline_ok(in)) expand_bmi_inline(&e, in);
        else expand_bmi(&e, in, f);
    } else if ((f = FIND(vec_forms, opc)) != NULL) {
        pick_scratch(&e, in);
        if (inline_form(opc) && !helpers_only && vec_shape_ok(in)) expand_inline(&e, in);
        else if ((wide_form(opc) || bcast_form(opc) || abs_form(opc)) && !helpers_only && oneop_ok(in))
            expand_oneop(&e, in);
        else if (cross_form(opc) && !helpers_only && cross_ok(in))
            expand_cross(&e, in);
        else if (!expand_vec(&e, in, f))
            dr_insert_clean_call(dc, bb, in, (void *)refuse_call, false, 2, OPND_CREATE_INTPTR(e.pc),
                                 OPND_CREATE_INT32(opc));
    } else {
        dr_insert_clean_call(dc, bb, in, (void *)refuse_call, false, 2, OPND_CREATE_INTPTR(e.pc),
                             OPND_CREATE_INT32(opc));
    }
    instrlist_set_translation_target(bb, NULL);
    instrlist_remove(bb, in);
    instr_destroy(dc, in);
}

/* A slot saved and not yet reloaded at the interrupted instruction holds the application's value. */
static bool event_restore(void *dc, bool restore_memory, dr_restore_state_info_t *info) {
    (void)restore_memory;
    byte *pc = info->fragment_info.cache_start_pc;
    if (pc == NULL || !info->raw_mcontext_valid) return true;
    byte *end = info->raw_mcontext->pc;
    reg_id_t y_live[3] = {DR_REG_NULL, DR_REG_NULL, DR_REG_NULL};
    reg_id_t g_live[6] = {DR_REG_NULL, DR_REG_NULL, DR_REG_NULL, DR_REG_NULL, DR_REG_NULL, DR_REG_NULL};
    bool fl_live = false;
    instr_t in;
    instr_init(dc, &in);
    while (pc != NULL && pc < end) {
        instr_reset(dc, &in);
        pc = decode(dc, pc, &in);
        if (pc != NULL && instr_valid(&in) && instr_get_opcode(&in) == OP_sahf) fl_live = false;
        if (pc == NULL ||!instr_valid(&in) || instr_num_srcs(&in) < 1 || instr_num_dsts(&in) < 1) continue;
        opnd_t s = instr_get_src(&in, 0), d = instr_get_dst(&in, 0);
        bool st = opnd_is_far_base_disp(d) && opnd_get_segment(d) == tls_seg && opnd_is_reg(s);
        bool ld = opnd_is_far_base_disp(s) && opnd_get_segment(s) == tls_seg && opnd_is_reg(d);
        if (!st && !ld) continue;
        int off = opnd_get_disp(st ? d : s) - (int)tls_offs;
        reg_id_t r = opnd_get_reg(st ? s : d);
        if (off == (int)offsetof(tls_t, fl)) {
            if (st) fl_live = true;
        } else if (off >= (int)offsetof(tls_t, save) && off < (int)offsetof(tls_t, a) && reg_is_ymm(r))
            y_live[off / (int)sizeof(ymm256)] = st ? r : DR_REG_NULL;
        else if (off >= (int)offsetof(tls_t, gsave) && off < (int)offsetof(tls_t, fl))
            g_live[(off - (int)offsetof(tls_t, gsave)) / 8] = st ? r : DR_REG_NULL;
    }
    instr_free(dc, &in);
    tls_t *t = (tls_t *)drmgr_get_tls_field(dc, tls_idx);
    for (int i = 0; i < 3; i++) {
        if (y_live[i] == DR_REG_NULL) continue;
        byte *dst = (byte *)&info->mcontext->simd[y_live[i] - DR_REG_YMM0];
        for (size_t k = 0; k < sizeof(ymm256); k++) dst[k] = t->save[i].b[k];
    }
    for (int i = 0; i < 6; i++)
        if (g_live[i] != DR_REG_NULL) reg_set_value(g_live[i], info->mcontext, t->gsave[i]);
    if (fl_live) {   /* ah holds SF ZF AF PF CF in their eflags positions; al bit 0 is OF */
        uint64 img = t->fl;
        info->mcontext->xflags = (info->mcontext->xflags & ~0x8D5ul) | ((img >> 8) & 0xD5) | ((img & 1) ? 0x800 : 0);
    }
    return true;
}

/* The frame helpers' stack: as deep as DynamoRIO's own, with a guard page below it. */
#define HSTACK_GUARD 4096
#define HSTACK_SIZE (64 * 1024)

static void event_thread_init(void *dc) {
    tls_t *t = (tls_t *)(dr_get_dr_segment_base(tls_seg) + tls_offs);
    t->self = (uint64)t;
    byte *s = dr_raw_mem_alloc(HSTACK_GUARD + HSTACK_SIZE, DR_MEMPROT_READ | DR_MEMPROT_WRITE, NULL);
    if (s == NULL || !dr_memory_protect(s, HSTACK_GUARD, DR_MEMPROT_NONE)) dr_abort_with_code(V3COMPAT_REFUSED);
    t->stack = (uint64)(s + HSTACK_GUARD + HSTACK_SIZE);
    t->thunk = (uint64)lean_thunk;
    drmgr_set_tls_field(dc, tls_idx, t);
}

static void event_thread_exit(void *dc) {
    tls_t *t = (tls_t *)drmgr_get_tls_field(dc, tls_idx);
    if (lab_verify && t != NULL) {
        uint64 any = 0;
        for (int i = 0; i < 32; i++) any |= t->vacc.b[i];
        if (any) dr_fprintf(STDERR, "V3VERIFY mismatch tid=%d\n", (int)dr_get_thread_id(dc));
    }
    if (t == NULL || t->stack == 0) return;
    byte *base = (byte *)t->stack - HSTACK_SIZE - HSTACK_GUARD, *sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    /* A helper that exits the process (vec_call's refusals) runs this on the stack itself: leave it. */
    if (sp >= base && sp < (byte *)t->stack) return;
    dr_raw_mem_free(base, HSTACK_GUARD + HSTACK_SIZE);
}

bool expand_init(bool helpers_only_) {
    helpers_only = helpers_only_;
    if (!dr_raw_tls_calloc(&tls_seg, &tls_offs, TLS_SLOTS, 32)) return false;
    tls_idx = drmgr_register_tls_field();
    if (tls_idx < 0) return false;
    drmgr_register_thread_init_event(event_thread_init);
    drmgr_register_thread_exit_event(event_thread_exit);
    return drmgr_register_restore_state_ex_event(event_restore);
}

void expand_exit(void) {
    drmgr_unregister_restore_state_ex_event(event_restore);
    drmgr_unregister_thread_init_event(event_thread_init);
    drmgr_unregister_thread_exit_event(event_thread_exit);
    drmgr_unregister_tls_field(tls_idx);
    dr_raw_tls_cfree(tls_offs, TLS_SLOTS);
}

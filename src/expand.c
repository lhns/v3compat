/*
 * Every expansion is rewindable and commits last: until one final write, the only application
 * state it touches is scratch saved in raw TLS, which event_restore puts back. A memory operand
 * is read by one real load with translation P, so a fault is the real fault at P. Inline code
 * leaves flags alone; only the mcontext path writes them, in its atomic commit.
 */
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
    uint64 gsave[2];
    uint64 self;
} tls_t;
#define TLS_SLOTS (sizeof(tls_t) / sizeof(void *))

static reg_id_t tls_seg;
static uint tls_offs;
static int tls_idx;
static bool helpers_only;

static opnd_t tls_opnd(size_t off, opnd_size_t sz) {
    return opnd_create_far_base_disp(tls_seg, DR_REG_NULL, DR_REG_NULL, 0, (int)(tls_offs + off), sz);
}
#define SAVE_Y(i) tls_opnd(offsetof(tls_t, save) + (i) * sizeof(ymm256), OPSZ_32)
#define SAVE_G(i) tls_opnd(offsetof(tls_t, gsave) + (i) * sizeof(uint64), OPSZ_8)
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

/* DR keeps its own TLS in gs, so a meta instruction cannot reach the application's. */
static bool mem_supported(opnd_t m) {
    return opnd_get_segment(m) != DR_SEG_GS && !opnd_is_far_rel_addr(m) && !opnd_is_far_abs_addr(m);
}
static bool needs_abs(opnd_t m) { return opnd_is_rel_addr(m) || opnd_is_abs_addr(m); }

/* The one instruction that may fault, and `t` is written only if the load succeeds. */
static void load_mem(emit_t *e, opnd_t m, reg_id_t t) {
    uint size = opnd_size_in_bytes(opnd_get_size(m));
    /* The application's address may be out of rip reach from the code cache. */
    bool abs = needs_abs(m);
    if (abs) {
        put(e, INSTR_CREATE_mov_st(e->dc, SAVE_G(0), reg(DR_REG_RAX)));
        instrlist_insert_mov_immed_ptrsz(e->dc, (ptr_int_t)opnd_get_addr(m), reg(DR_REG_RAX), e->bb, e->where,
                                         NULL, NULL);
        m = opnd_create_base_disp(DR_REG_RAX, DR_REG_NULL, 0, 0, opnd_get_size(m));
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
    if (abs) put(e, INSTR_CREATE_mov_ld(e->dc, reg(DR_REG_RAX), SAVE_G(0)));
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
 * The second source is ymm or memory for add and multiply; xmm, memory or an immediate for shifts.
 */
static bool inline_form(int opc) {
    return opc == OP_vpaddq || opc == OP_vpmuludq || opc == OP_vpsrlq || opc == OP_vpsllq;
}

static void expand_inline(emit_t *e, instr_t *in) {
    int opc = instr_get_opcode(in);
    reg_id_t d = opnd_get_reg(instr_get_dst(in, 0));
    opnd_t ao, b;
    vec_srcs(in, &ao, &b);
    reg_id_t a = opnd_get_reg(ao);
    bool shift = opc == OP_vpsrlq || opc == OP_vpsllq;
    bool mem = opnd_is_memory_reference(b);
    /* DR gives a shift count the vector's width: it is xmm or m128 for ymm as well. */
    if (shift && mem) opnd_set_size(&b, OPSZ_16);
    if (shift && opnd_is_reg(b)) b = reg(xmm_of(ymm_of(opnd_get_reg(b))));
    reg_id_t t0 = e->scratch[0], t1 = e->scratch[1], t2 = e->scratch[2];
    reg_id_t t0x = xmm_of(t0), t1x = xmm_of(t1), t2x = xmm_of(t2);

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
}

static void refuse_call(app_pc pc, int opc) {
    dr_fprintf(STDERR, "v3compat: refusing %s at %p: not emulated\n", decode_opcode_name(opc), pc);
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
    F3(vpsignb, VPSIGNB), F3(vpsignw, VPSIGNW), F3(vpsignd, VPSIGND), F3(vphaddd, VPHADDD),
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

/* vec_call descriptor: op | type<<8 | imm<<16 | flags<<24 */
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
    uint64 desc = (uint64)f->op | ((uint64)f->type << 8) | ((uint64)imm << 16) | ((uint64)fl << 24);
    dr_insert_clean_call_ex(e->dc, e->bb, e->where, (void *)vec_call, DR_CLEANCALL_ALWAYS_OUT_OF_LINE, 1,
                            OPND_CREATE_INT64(desc));
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
    if (abs) {
        put(e, INSTR_CREATE_mov_st(e->dc, SAVE_G(1), reg(addr)));
        instrlist_insert_mov_immed_ptrsz(e->dc, (ptr_int_t)opnd_get_addr(m), reg(addr), e->bb, e->where, NULL, NULL);
        m = opnd_create_base_disp(addr, DR_REG_NULL, 0, 0, opnd_get_size(m));
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
    if (abs) put(e, INSTR_CREATE_mov_ld(e->dc, reg(addr), SAVE_G(1)));
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
        /* Kept: FSGSBASE, SMEP, ERMS, INVPCID. Set: BMI1, AVX2, BMI2. */
        b = sub != 0 ? 0 : (b & ((1u << 0) | (1u << 7) | (1u << 9) | (1u << 10))) | (1u << 3) | (1u << 5) | (1u << 8);
        a = c = d = 0;
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
bool instr_is_evex_encoded(void *dc, instr_t *in) {
    if (!instr_raw_bits_valid(in)) return false;
    const byte *p = instr_get_raw_bits(in);
    for (uint i = 0; i < instr_length(dc, in); i++, p++) {
        if (*p == 0x2e || *p == 0x36 || *p == 0x3e || *p == 0x26 || *p == 0x64 || *p == 0x65 || *p == 0x67) continue;
        return *p == 0x62;
    }
    return false;
}

/*
 * AVX1 has the same operation under another opcode for these, faults and all, so the application
 * instruction stays and only its opcode changes. DR's vmovntdqa and the store forms of the masked
 * moves list their operands in the other order.
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
    if (from == OP_vmovntdqa && opnd_is_memory_reference(instr_get_dst(in, 0))) {
        opnd_t d = instr_get_dst(in, 0);
        instr_set_dst(in, 0, instr_get_src(in, 0));
        instr_set_src(in, 0, d);
    }
    return true;
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
    } else if (instr_is_evex_encoded(dc, in) || !all_mem_supported(in)) {
        dr_insert_clean_call(dc, bb, in, (void *)refuse_call, false, 2, OPND_CREATE_INTPTR(e.pc),
                             OPND_CREATE_INT32(opc));
    } else if ((f = FIND(bmi_forms, opc)) != NULL) {
        pick_scratch(&e, in);
        expand_bmi(&e, in, f);
    } else if ((f = FIND(vec_forms, opc)) != NULL) {
        pick_scratch(&e, in);
        if (inline_form(opc) && !helpers_only && vec_shape_ok(in)) expand_inline(&e, in);
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
    reg_id_t y_live[3] = {DR_REG_NULL, DR_REG_NULL, DR_REG_NULL}, g_live[2] = {DR_REG_NULL, DR_REG_NULL};
    instr_t in;
    instr_init(dc, &in);
    while (pc != NULL && pc < end) {
        instr_reset(dc, &in);
        pc = decode(dc, pc, &in);
        if (pc == NULL || !instr_valid(&in) || instr_num_srcs(&in) < 1 || instr_num_dsts(&in) < 1) continue;
        opnd_t s = instr_get_src(&in, 0), d = instr_get_dst(&in, 0);
        bool st = opnd_is_far_base_disp(d) && opnd_get_segment(d) == tls_seg && opnd_is_reg(s);
        bool ld = opnd_is_far_base_disp(s) && opnd_get_segment(s) == tls_seg && opnd_is_reg(d);
        if (!st && !ld) continue;
        int off = opnd_get_disp(st ? d : s) - (int)tls_offs;
        reg_id_t r = opnd_get_reg(st ? s : d);
        if (off >= (int)offsetof(tls_t, save) && off < (int)offsetof(tls_t, a) && reg_is_ymm(r))
            y_live[off / (int)sizeof(ymm256)] = st ? r : DR_REG_NULL;
        else if (off >= (int)offsetof(tls_t, gsave) && off < (int)offsetof(tls_t, self))
            g_live[(off - (int)offsetof(tls_t, gsave)) / 8] = st ? r : DR_REG_NULL;
    }
    instr_free(dc, &in);
    tls_t *t = (tls_t *)drmgr_get_tls_field(dc, tls_idx);
    for (int i = 0; i < 3; i++) {
        if (y_live[i] == DR_REG_NULL) continue;
        byte *dst = (byte *)&info->mcontext->simd[y_live[i] - DR_REG_YMM0];
        for (size_t k = 0; k < sizeof(ymm256); k++) dst[k] = t->save[i].b[k];
    }
    for (int i = 0; i < 2; i++)
        if (g_live[i] != DR_REG_NULL) reg_set_value(g_live[i], info->mcontext, t->gsave[i]);
    return true;
}

static void event_thread_init(void *dc) {
    tls_t *t = (tls_t *)(dr_get_dr_segment_base(tls_seg) + tls_offs);
    t->self = (uint64)t;
    drmgr_set_tls_field(dc, tls_idx, t);
}

bool expand_init(bool helpers_only_) {
    helpers_only = helpers_only_;
    if (!dr_raw_tls_calloc(&tls_seg, &tls_offs, TLS_SLOTS, 32)) return false;
    tls_idx = drmgr_register_tls_field();
    if (tls_idx < 0) return false;
    drmgr_register_thread_init_event(event_thread_init);
    return drmgr_register_restore_state_ex_event(event_restore);
}

void expand_exit(void) {
    drmgr_unregister_restore_state_ex_event(event_restore);
    drmgr_unregister_thread_init_event(event_thread_init);
    drmgr_unregister_tls_field(tls_idx);
    dr_raw_tls_cfree(tls_offs, TLS_SLOTS);
}

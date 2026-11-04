/* Classifies instructions and looks them up on the generated list. Runs at translation only. */
#include "allow.h"

/* DR exports no libc string functions to clients (calling one dies at first use). */
static bool starts(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return false;
    return true;
}
static bool contains(const char *s, const char *p) {
    for (; *s; s++) if (starts(s, p)) return true;
    return false;
}

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

/* BMI/AVX2 opcodes are matched by DR opcode or mnemonic. */
int classify(void *dc, instr_t *in) {
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

bool instr_is_evex_encoded(void *dc, instr_t *in) {
    if (!instr_raw_bits_valid(in)) return false;
    const byte *p = instr_get_raw_bits(in);
    for (uint i = 0; i < instr_length(dc, in); i++, p++) {
        if (*p == 0x2e || *p == 0x36 || *p == 0x3e || *p == 0x26 || *p == 0x64 || *p == 0x65 || *p == 0x67) continue;
        return *p == 0x62;
    }
    return false;
}

static char *put(char *o, const char *end, const char *s) {
    while (*s && o < end) *o++ = *s++;
    return o;
}

static char *put_num(char *o, const char *end, uint v) {
    char t[12];
    int n = 0;
    do t[n++] = (char)('0' + v % 10); while ((v /= 10) != 0);
    while (n > 0 && o < end) *o++ = t[--n];
    return o;
}

/* A mandatory prefix or REX.W can select another instruction on this CPU where DR reports one
 * opcode, so the bytes decide. Segment and address-size prefixes change the address only. */
static char *put_bits(void *dc, instr_t *in, char *o, const char *end) {
    const byte *p = instr_raw_bits_valid(in) ? instr_get_raw_bits(in) : instr_get_app_pc(in);
    const byte *stop = p + instr_length(dc, in);
    bool has66 = false, f2 = false, f3 = false, lock = false;
    for (; p < stop; p++) {
        if (*p == 0x66) has66 = true;
        else if (*p == 0xf2) f2 = true;
        else if (*p == 0xf3) f3 = true;
        else if (*p == 0xf0) lock = true;
        else if (*p != 0x26 && *p != 0x2e && *p != 0x36 && *p != 0x3e && *p != 0x64 && *p != 0x65 && *p != 0x67) break;
    }
    if (has66) o = put(o, end, " 66");
    if (f2) o = put(o, end, " f2");
    if (f3) o = put(o, end, " f3");
    if (lock) o = put(o, end, " lock");
    if (p < stop && (*p & 0xf8) == 0x48) o = put(o, end, " rexw");
    /* With REX.B, 90 is xchg r8, rax. DR calls F3 41 90 pause either way, and CPUs disagree. */
    if (p + 1 < stop && (*p & 0xf1) == 0x41 && p[1] == 0x90) o = put(o, end, " rexb");
    /* In 64-bit mode C4 and C5 cannot be LES and LDS. */
    if (p + 2 < stop && *p == 0xc5) {
        o = put(o, end, " vex");
        if (p[1] & 4) o = put(o, end, " l");
    } else if (p + 3 < stop && *p == 0xc4) {
        o = put(o, end, " vex");
        if (p[2] & 0x80) o = put(o, end, " w");
        if (p[2] & 4) o = put(o, end, " l");
    }
    return o;
}

static char *put_opnd(char *o, const char *end, opnd_t x) {
    if (opnd_is_reg(x)) {
        reg_id_t r = opnd_get_reg(x);
        if (reg_is_gpr(r)) {
            uint sz = opnd_size_in_bytes(reg_get_size(r));
            o = put(o, end, sz == 8 ? "g64" : sz == 4 ? "g32" : sz == 2 ? "g16" : "g8");
        } else if (reg_is_strictly_xmm(r)) o = put(o, end, "x");
        else if (reg_is_strictly_ymm(r)) o = put(o, end, "y");
        else if (reg_is_strictly_zmm(r)) o = put(o, end, "z");
        else if (reg_is_opmask(r)) o = put(o, end, "k");
        else if (reg_is_mmx(r)) o = put(o, end, "mm");
        else if (reg_is_fp(r)) o = put(o, end, "st");
        else if (reg_is_segment(r)) o = put(o, end, "seg");
        else { o = put(o, end, "r"); o = put_num(o, end, r); }
    } else if (opnd_is_memory_reference(x)) {
        o = put(o, end, opnd_is_vsib(x) ? "vm" : "m");
        o = put_num(o, end, opnd_get_size(x));
    } else if (opnd_is_immed(x)) o = put(o, end, "i");
    else if (opnd_is_pc(x) || opnd_is_instr(x)) o = put(o, end, "p");
    else if (opnd_is_far_pc(x) || opnd_is_far_instr(x)) o = put(o, end, "fp");
    else o = put(o, end, "?");
    return o;
}

uint64 allow_key(void *dc, instr_t *in, char *text) {
    char *o = text;
    const char *end = text + ALLOW_TEXT_MAX - 1;
    o = put(o, end, decode_opcode_name(instr_get_opcode(in)));
    o = put_bits(dc, in, o, end);
    o = put(o, end, " d:");
    for (int i = 0; i < instr_num_dsts(in); i++) {
        if (i) o = put(o, end, ",");
        o = put_opnd(o, end, instr_get_dst(in, i));
    }
    o = put(o, end, " s:");
    for (int i = 0; i < instr_num_srcs(in); i++) {
        if (i) o = put(o, end, ",");
        o = put_opnd(o, end, instr_get_src(in, i));
    }
    *o = 0;
    uint64 h = 0xcbf29ce484222325ULL;
    for (const char *p = text; *p; p++) h = (h ^ (byte)*p) * 0x100000001b3ULL;
    return h;
}

#include "allowlist.inc"

static bool in_table(const uint64 *t, size_t n, uint64 k) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (t[m] == k) return true;
        if (t[m] < k) lo = m + 1; else hi = m;
    }
    return false;
}

bool allow_listed(uint64 key, bool emulated) {
    return emulated ? in_table(allow_emulated, sizeof allow_emulated / sizeof *allow_emulated, key)
                    : in_table(allow_native, sizeof allow_native / sizeof *allow_native, key);
}

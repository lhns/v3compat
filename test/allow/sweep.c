/*
 * sweep.c: the allowlist's universe. Decodes the legacy and VEX opcode space with XED's tables,
 * every prefix and modrm, and prints one test row per distinct form. Regeneration only (regen.sh);
 * CI uses the committed output. Columns: bytes, undefined-flags mask, iform, iclass, isa set,
 * how Ivy Bridge decodes it (same, ud, as:ICLASS), why the probe does not run it (- if it does).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xed-interface.h"
#include "layout.h"

static int decode(const uint8_t *b, int n, xed_decoded_inst_t *x, xed_chip_enum_t chip) {
    xed_state_t st;
    xed_state_zero(&st);
    st.mmode = XED_MACHINE_MODE_LONG_64;
    st.stack_addr_width = XED_ADDRESS_WIDTH_64b;
    xed_decoded_inst_zero_set_mode(x, &st);
    xed_decoded_inst_set_input_chip(x, chip);
    return xed_decode(x, b, n) == XED_ERROR_NONE;
}

/* Why a form is not run: it would leave the harness, or its result is not comparable. */
static const char *not_run(xed_decoded_inst_t *x) {
    const char *ic = xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(x));
    const char *f = xed_iform_enum_t2str(xed_decoded_inst_get_iform_enum(x));
    xed_category_enum_t c = xed_decoded_inst_get_category(x);
    if (xed_decoded_inst_get_attribute(x, XED_ATTRIBUTE_RING0)) return "privileged";
    if (c == XED_CATEGORY_SYSCALL || c == XED_CATEGORY_SYSRET || c == XED_CATEGORY_INTERRUPT) return "kernel-entry";
    if (strstr(ic, "_FAR") || strstr(ic, "IRET")) return "far-control-flow";
    if (!strncmp(ic, "XSAVE", 5) || !strncmp(ic, "XRSTOR", 6) || !strncmp(ic, "FXSAVE", 6) || !strncmp(ic, "FXRSTOR", 7))
        return "state-layout";
    if (!strncmp(f, "POP_FS", 6) || !strncmp(f, "POP_GS", 6) || !strncmp(f, "MOV_SEG", 7) || !strcmp(ic, "LFS") ||
        !strcmp(ic, "LGS") || !strcmp(ic, "LSS") || !strcmp(ic, "WRFSBASE") || !strcmp(ic, "WRGSBASE"))
        return "segment-state";
    static const char *const nondet[] = {"RDTSC", "RDTSCP", "RDRAND", "RDSEED", "RDPMC", "RDPID", "CPUID", "XGETBV",
                                         "RDPRU", "RDFSBASE", "RDGSBASE", "SIDT", "SGDT", "SLDT", "STR", NULL};
    for (int i = 0; nondet[i]; i++) if (!strcmp(ic, nondet[i])) return "nondeterministic";
    return "-";
}

#define SET (1u << 22)
static char *seen[SET];
static int first(const char *k) {
    uint32_t h = 2166136261u;
    for (const char *p = k; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (h &= SET - 1; seen[h]; h = (h + 1) & (SET - 1)) if (!strcmp(seen[h], k)) return 0;
    seen[h] = strdup(k);
    return 1;
}

static void emit(const uint8_t *b, int len, xed_decoded_inst_t *x) {
    const xed_simple_flag_t *sf = xed_decoded_inst_get_rflags_info(x);
    unsigned undef = sf ? xed_flag_set_mask(xed_simple_flag_get_undefined_flag_set(sf)) : 0;
    xed_decoded_inst_t y;
    char ivb[64] = "ud";
    if (decode(b, len, &y, XED_CHIP_IVYBRIDGE)) {
        if (xed_decoded_inst_get_iclass(&y) == xed_decoded_inst_get_iclass(x)) strcpy(ivb, "same");
        else snprintf(ivb, sizeof ivb, "as:%s", xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(&y)));
    }
    for (int i = 0; i < len; i++) printf("%02x", b[i]);
    printf("\t%x\t%s\t%s\t%s\t%s\t%s\n", undef, xed_iform_enum_t2str(xed_decoded_inst_get_iform_enum(x)),
           xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(x)), xed_isa_set_enum_t2str(xed_decoded_inst_get_isa_set(x)),
           ivb, not_run(x));
}

/* If b[0..n) starts a form not seen yet under these encoding dimensions: one row per immediate value. */
static void try_form(uint8_t *b, int n, const char *dims) {
    xed_decoded_inst_t x;
    memset(b + n, 0, 15);
    if (!decode(b, 15, &x, XED_CHIP_ALL)) return;
    char k[256];
    snprintf(k, sizeof k, "%s|%s", xed_iform_enum_t2str(xed_decoded_inst_get_iform_enum(&x)), dims);
    if (!first(k)) return;
    int len = xed_decoded_inst_get_length(&x), iw = xed_decoded_inst_get_immediate_width(&x);
    xed_category_enum_t c = xed_decoded_inst_get_category(&x);
    if (xed_decoded_inst_get_iclass(&x) == XED_ICLASS_MOV && xed_decoded_inst_get_memory_displacement_width(&x, 0) == 8) {
        uint64_t a = P_RBX; /* moffs: the absolute address must land in the scratch area */
        memcpy(b + len - 8, &a, 8);
        emit(b, len, &x);
        return;
    }
    /* What follows the opcode is displacement, not a modrm: 0 reaches the next instruction. */
    int rel = xed_decoded_inst_get_branch_displacement_width(&x);
    if (rel) memset(b + len - rel, 0, rel);
    /* Shift by 1 has an implicit immediate: a width and no bytes. */
    if (iw == 0 || rel || strstr(xed_iform_enum_t2str(xed_decoded_inst_get_iform_enum(&x)), "_ONE") ||
        xed_decoded_inst_get_second_immediate(&x) != 0) {
        /* enter's and extrq's two immediates stay 0: the byte tried as a modrm would allocate 50 KB. */
        if (iw && xed_decoded_inst_get_second_immediate(&x) != 0) memset(b + len - iw - 1, 0, iw + 1);
        emit(b, len, &x);
        return;
    }
    static const uint8_t vals[] = {0, 1, 3, 0x11, 0x55, 0x80, 0xff};
    xed_iform_enum_t form = xed_decoded_inst_get_iform_enum(&x);
    for (size_t v = 0; v < (iw == 1 ? sizeof vals : 3); v++) {
        memset(b + len - iw, 0, iw);
        b[len - iw] = vals[v * (iw == 1 ? 1 : 2)];
        if (decode(b, 15, &x, XED_CHIP_ALL) && xed_decoded_inst_get_iform_enum(&x) == form) emit(b, len, &x);
    }
}

/* Every modrm is tried, as opcodes extend through it; rcx and rsi first (layout.h). A form without
 * a modrm decodes on the first try and ignores the byte. */
static const uint8_t ORD_REG[8] = {1, 7, 2, 6, 0, 3, 5, 4}, ORD_RM[8] = {6, 1, 7, 2, 0, 3, 5, 4};
static void modrms(uint8_t *b, int n, const char *dims) {
    for (int r = 0; r < 8; r++)
        for (int m = 0; m < 8; m++) {
            b[n] = 0xc0 | ORD_REG[r] << 3 | ORD_RM[m];
            try_form(b, n + 1, dims);
        }
    for (int r = 0; r < 8; r++) {
        b[n] = ORD_REG[r] << 3 | 3;                  /* [rbx] */
        try_form(b, n + 1, dims);
        b[n] = ORD_REG[r] << 3 | 4; b[n + 1] = 0x2b; /* [rbx+rbp]: the only way to VSIB forms */
        try_form(b, n + 2, dims);
    }
}

int main(int argc, char **argv) {
    xed_tables_init();
    if (argc > 1 && !strcmp(argv[1], "iforms")) { /* the universe to account for: every iform in the tables */
        for (int f = 1; f < XED_IFORM_LAST; f++)
            printf("%s\t%s\n", xed_iform_enum_t2str((xed_iform_enum_t)f), xed_isa_set_enum_t2str(xed_iform_to_isa_set((xed_iform_enum_t)f)));
        return 0;
    }
    static const char *const pfx[] = {"", "\x66", "\xf2", "\xf3", "\x66\xf2", "\x66\xf3"};
    static const uint8_t rex[] = {0, 0x48, 0x41, 0x49};
    uint8_t b[64];
    char d[64];
    for (int lock = 0; lock < 2; lock++)
        for (size_t p = 0; p < 6; p++)
            for (size_t r = 0; r < 4; r++)
                for (int map = 0; map < 4; map++)
                    for (int op = 0; op < 256; op++) {
                        int n = 0;
                        if (map == 1 && (op == 0x38 || op == 0x3a)) continue; /* escapes: maps 2 and 3 */
                        if (map == 0 && ((op & 0xf0) == 0x40 || op == 0x0f || op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e ||
                                         op == 0x64 || op == 0x65 || op == 0x66 || op == 0x67 || op == 0xf0 ||
                                         op == 0xf2 || op == 0xf3 || op == 0xc4 || op == 0xc5 || op == 0x62))
                            continue;
                        if (lock) b[n++] = 0xf0;
                        for (const char *q = pfx[p]; *q; q++) b[n++] = (uint8_t)*q;
                        if (rex[r]) b[n++] = rex[r];
                        if (map) b[n++] = 0x0f;
                        if (map == 2) b[n++] = 0x38;
                        if (map == 3) b[n++] = 0x3a;
                        b[n++] = op;
                        snprintf(d, sizeof d, "L%d P%zu R%zu", lock, p, r);
                        modrms(b, n, d);
                    }
    /* vvvv=1111 throughout: a real register (0) for the forms that read it, required by the rest. */
    for (int map = 1; map <= 3; map++)
        for (int pp = 0; pp < 4; pp++)
            for (int w = 0; w < 2; w++)
                for (int l = 0; l < 2; l++)
                    for (int op = 0; op < 256; op++) {
                        b[0] = 0xc4;
                        b[1] = 0xe0 | map;
                        b[2] = w << 7 | 0xf << 3 | l << 2 | pp;
                        b[3] = op;
                        snprintf(d, sizeof d, "V%d %d %d %d", map, pp, w, l);
                        modrms(b, 4, d);
                    }
    return 0;
}

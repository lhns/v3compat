/* What may run: docs/plan.md "Translation rules", list in allowlist.inc, built by test/allow/gen.py. */
#ifndef V3COMPAT_ALLOW_H
#define V3COMPAT_ALLOW_H
#include "dr_api.h"

/* Classes the CPU lacks on AVX-only parts; C_N: none of them. */
enum cls { C_AVX2, C_FMA, C_BMI, C_LZCNT, C_TZCNT, C_MOVBE, C_INSTRS, C_N };
int classify(void *dc, instr_t *in);
/* EVEX (AVX-512) is never emulated: it is refused. */
bool instr_is_evex_encoded(void *dc, instr_t *in);

#define ALLOW_TEXT_MAX 256
/* DR opcode, the encoding bits that can change meaning, operand classes. Fills text; returns its hash. */
uint64 allow_key(void *dc, instr_t *in, char *text);
/* On the emulated list, or else on the native one? */
bool allow_listed(uint64 key, bool emulated);
#endif

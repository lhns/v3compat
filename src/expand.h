/* Replacing the instructions the CPU lacks. See docs/plan.md "Translation rules". */
#ifndef V3COMPAT_EXPAND_H
#define V3COMPAT_EXPAND_H
#include "dr_api.h"

/* Exit status when a program executes an instruction nothing handles (ADR 0001). */
#define V3COMPAT_REFUSED 86
/* Exit status when an exec is refused: DynamoRIO cannot run that program (src/exec_check.h). */
#define V3COMPAT_REFUSED_EXEC 87

bool expand_init(bool helpers_only);
void expand_exit(void);
/* Replaces `in` if this CPU cannot run it; refuses it if `lacked` and nothing handles it. */
void expand_instr(void *dc, instrlist_t *bb, instr_t *in, bool lacked);
/* Replaces `in` with a clean exit, status 86, if it is ever executed. */
void expand_refuse(void *dc, instrlist_t *bb, instr_t *in);
#endif

/* Fixed addresses of the probe, shared with sweep.c: results must not depend on the load address. */
#ifndef ALLOW_LAYOUT_H
#define ALLOW_LAYOUT_H
#define P_DATA   0x10000000UL
#define P_DATASZ 0x4000UL
/* Modrm memory is [rbx]. The sweep prefers rcx and rsi as registers, and rsi is a nonzero divisor. */
#define P_RBX    (P_DATA + 0x1000)
#define P_CODE   0x20000000UL
#define P_CODESZ 0x1000UL
#define P_LAND   (P_CODE + 64) /* after the instruction and its nops: the jump back to the harness */
#endif

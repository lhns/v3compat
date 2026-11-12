# 0007: Hot operations inline in Phase 1; the helper boundary

## Status
Accepted, 2026-11-04. Amends [0005](0005-avxemu-core-as-helpers.md): helpers no longer cover everything.

## Context
Sending every instruction the CPU lacks to a helper cannot meet Phase 1's target. Reading and writing the mcontext in
a clean call costs about 490 ns per operation on the target CPU, and a raw-TLS frame about 80 ns. A database's compute
query can execute a hundred million such operations, nearly all of them a few lane-wise ones.

## Decision
Four paths, chosen per form:

- **Inline**, in two 128-bit halves, for the lane-wise operations measurement shows dominate. Forms join on evidence.
- **Native substitution**, for forms AVX1 has under another opcode with the same semantics and faults.
- **Frame helpers**, for all other vector forms: operands are copied to raw TLS, and a helper with no mcontext computes.
- **Mcontext helpers**, for rare forms that write flags or several registers.

Every path commits last or is one instruction. Each inline form must match its helper in the semantic suite. No path
uses flag liveness: inline sequences leave flags alone, as DynamoRIO's flag table overstates writes.

## Consequences
Two implementations of the hot forms, which the suite compares on every run. Scratch ymm registers are saved in TLS, which
must stay 32-byte aligned for speed. Refused forms stop the program with exit status 86.

## Amendment, 2026-11-05
The scalar `rorx`, `andn`, `mulx` and `movbe` (load; the store already was) are inline too: a clean call costs far more than the operation, whatever the program's mix. Flags are not left to liveness: `rorx` and `mulx` clobber them, so each saves them with `lahf`/`seto` into raw TLS and restores them after, always; `andn` needs no save as `and` writes the flags it defines.

## Amendment, 2026-11-05
The AVX2 gathers are a fifth path, inline and unrolled per element, because avxemu's archive never touches memory. They are
the exception to "commits last": each active element is loaded (the only faulting instruction), stored to the TLS frame,
and dst and mask are rewritten from the frame, so a fault leaves the elements before it done and restartable, as on
hardware. Flags and scratch GPRs are saved and restored like `rorx`. Unprocessed mask elements keep their value, as on AMD
(avxemu's `docs/vendor-differences.md`).

Segments: memory operands with `fs:` or `gs:` (absolute, base, index forms) are rewritten with the application's segment base from `dr_insert_get_seg_base`, as `fs:` already was; an `fs:`/`gs:` operand with a 32-bit address (0x67) or rip-relative is refused (exit 86). The application's gs base is 0: a program that sets it (`arch_prctl(ARCH_SET_GS)`) crashes under DynamoRIO itself.

## Amendment, 2026-11-12
`shlx`, `shrx`, `sarx`, `bzhi`, `tzcnt` and `lzcnt` are inline too, chosen by executed counts in duckdb. The shifts save the flags like `rorx`; `bzhi`, `tzcnt` and `lzcnt` branch on the zero or out-of-width case and write only flags the instruction defines (the undefined AF and PF follow the sequence, as for `andn`). `bextr`, `blsi`/`blsr`/`blsmsk`, `pdep` and `pext` stay on the helper: too rare to pay.

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

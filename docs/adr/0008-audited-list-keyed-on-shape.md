# 0008: The audited list, keyed on shape and built from evidence

## Status
Accepted, 2026-11-04. Implements the audited list [0001](0001-correctness-over-speed.md) calls for.

## Context
A DR opcode does not identify an instruction: forms of one opcode can differ in what this CPU has (ymm against xmm) or
in meaning (an immediate where a register is expected, a prefix DR folds away). A list of opcodes cannot refuse those.
A hand-written list would record what its author remembers, not what the CPU does.

## Decision
A key is the DR opcode, the bytes that can select another instruction (66, F2, F3, lock, REX.W, VEX.W, VEX.L) and the
operand classes in DR's order. Segment and address-size prefixes and register numbers are not in it: they change no
meaning across CPUs. One list holds native keys and one emulated keys, compiled in as sorted hashes and looked up at
translation only.

The list is generated. XED's tables give the universe, DR gives each form's key, and a form is native only if XED's Ivy
Bridge tables agree on the instruction and Ivy Bridge and an AVX2 machine give the same registers, flags and memory from
fixed seeds. Architecturally undefined flags are masked by XED's tables. A form is emulated only if the avxemu suite
covers it. One bad form refuses its whole key. Exceptions, each with a reason, are in `test/allow/policy.tsv`: hints that
XED sees differently, faults that are the point, and what cannot be compared.

Bytes DR cannot decode are refused from the SIGILL they raise. `cpuid` hides FSGSBASE, as DR neither mangles nor could
keep its own segment safe from those instructions.

## Consequences
A real program using a form nobody tested stops with status 86 and names it; `V3COMPAT_AUDIT=1` lists all of them, and
the fix is more evidence, not a wider rule. AVX2 evidence comes from CI's runners, not approved Intel AVX2 references, so
[allowlist.md](../allowlist.md) states what that leaves unverified. Bumping DR or XED means rerunning `test/allow/regen.sh`;
CI fails until then.

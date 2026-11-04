# 0005: AVXEmu's core as helpers, its mechanism retired

## Status
Accepted.

## Context
AVXEmu has carefully tested instruction semantics and a differential test approach, and it is CC0. Its patching
mechanism is what cannot cover libraries, static binaries or JIT code, and most review findings are in that
mechanism.

## Decision
Use the emulation core (`exec.c`, `exec_bmi.c`, `softfma.c`) as helpers for cross-lane, FMA and gather instructions,
rebuilt with VEX encoding. Keep the test suite in the [lhns/avxemu](https://github.com/lhns/avxemu) fork. On Linux, retire
trampolines, the SIGILL handler, live relocation and the ELF metadata parser. Nothing is sent upstream for now.

## Consequences
Semantic fixes keep their value. Helpers are slow, so hot instructions need new inline expansions. If Phase 0 fails,
the preload remains as a narrow fallback for provable images.

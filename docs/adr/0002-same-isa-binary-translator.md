# 0002: A same-ISA binary translator, not a kernel module or patching

## Status
Proposed: depends on Phase 0.

## Context
The goal covers libraries, `dlopen`, static binaries, Go and JIT code. Trap designs, including a kernel module and
a hypervisor, cannot see `lzcnt`/`tzcnt`, since those do not fault. In a KVM guest each #UD also starts with a VM
exit. A module bug panics the node. Patching in memory needs proven instruction boundaries, which libraries and
JIT code cannot supply, and interposition does not reach static binaries.

## Decision
Translate code as it is first executed. Copy most instructions unchanged, expand or call helpers for the rest.

## Consequences
One mechanism replaces patching, trampolines, the SIGILL fallback, interposition and CPUID faulting. Every executed
instruction now passes through the translator, a larger correctness surface than patched sites, and some overhead
applies to all code. The speed target is an estimate until measured. A sparse-AVX2 program may run slower than under
AVXEmu's patching.

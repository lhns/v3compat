# 0001: Correctness over speed; refuse what is not understood

## Status
Accepted

## Context
Emulation errors do not announce themselves. On AVX-only CPUs, `lzcnt` and `tzcnt` run as `bsr` and `bsf` without
faulting, and return wrong answers. A review of AVXEmu found `crc32` decoded as `MOVBE`, corrupting its input.
Crashes are visible and can be dealt with; wrong results reach the data.

## Decision
A clean refusal beats a guess. Instructions outside an audited list stop the program. Speed work never weakens a
differential test against real hardware.

## Consequences
Some programs will refuse to run until their instructions are added. The audited list needs differential testing
to build. Results are strong evidence and not proof, so storage and database primaries stay off this.

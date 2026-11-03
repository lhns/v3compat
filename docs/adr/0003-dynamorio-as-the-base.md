# 0003: Build on DynamoRIO

## Status
Proposed: depends on Phase 0.

## Context
A correct x86-64 translator is years of work: decoding, signals, threads, `exec`, self-modifying code. Intel Pin is
closed source and its SDE costs 42 s of load-time CPU on a 77 MB binary. QEMU user translates all code, is about 12x
slower on our query, and does not follow `exec`. DynamoRIO is open source, translates lazily and allows arbitrary
instruction rewriting.

## Decision
Write a DynamoRIO client. Do not write a translator from scratch.

## Consequences
A large dependency and its bugs, and a tie to its release cycle. Its core and `drmgr`, `drreg`, `drx` are BSD. Its
`drutil`, `drwrap` and `drsyscall` are LGPL 2.1, so if used they ship as separate shared libraries, never statically
linked. Replacing it would mean rewriting the client, though helpers and tests stay.

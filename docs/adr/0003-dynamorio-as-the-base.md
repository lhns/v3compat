# 0003: Build on our DynamoRIO fork

## Status
Accepted.

## Context
A correct x86-64 translator is years of work: decoding, signals, threads, `exec`, self-modifying code. Intel Pin is
closed source and its SDE costs 42 s of load-time CPU on a 77 MB binary. QEMU user translates all code, is about 12x
slower on our query, and does not follow `exec`. DynamoRIO is open source, translates lazily and allows arbitrary
instruction rewriting. Stock DynamoRIO 11.3.0 hangs programs that rely on async signals, and also hangs at exit; see the plan.

## Decision
Write a DynamoRIO client. Do not write a translator from scratch. Build only against our fork
([lhns/dynamorio](https://github.com/lhns/dynamorio)), pinned by commit. Nothing is sent upstream.

## Consequences
A large dependency and its bugs. We build it from source and carry its fixes ourselves, including merging upstream releases. Its core and `drmgr`, `drreg`, `drx` are BSD. Its
`drutil`, `drwrap` and `drsyscall` are LGPL 2.1, so if used they ship as separate shared libraries, never statically
linked. Replacing it would mean rewriting the client, though helpers and tests stay.

## Amendment 2026-11-11
The fork tracks upstream master, not only releases: our fixes are rebased onto it as flat commits, and the pin moves only
after the lab, stress and DynamoRIO suite runs match the previous pin, with an upstream control for any fix whose
conflict needed new logic.

# 0010: DynamoRIO's cache reset stays off; a mitigation, not a repair

## Status
Accepted, 2026-11-05.

## Context
A full cache reset suspends every thread and relocates it out of the code cache. Relocation does not reproduce the
application's state in three known ways: registers in the x64 trace compare (fixed in our fork), flags in the same
window (upstream i#400, open), and clean calls before setup or after teardown of their saved state (upstream i#4219,
open, the likely cause of crashes under the client). The result is a wrong answer with exit status 0 as well as crashes,
in any multithreaded program that retires enough trace heads to fill the pending-deletion list. Stock upstream does it
too. Reset under code churn also hangs. Reset exists to reclaim memory, so this trades memory for correctness
([0001](0001-correctness-over-speed.md)).

## Decision
- The launcher passes `-no_enable_reset`, ahead of the user's options, so `V3COMPAT_DR_OPTS` can still turn reset on to
  test it.
- The fork carries the register fix, since other relocation paths could reach it.
- This is a mitigation: the flag and clean-call faults remain and are only unreachable while nothing relocates a thread.
  Reset may return only when a relocation test under the client passes: `test/apps/duckdb-reset.sh`, forced, under
  `v3compat-run`.

## Consequences
- Retired fragments are freed only by the ordinary pending-deletion path. Programs that do not generate code use about
  the same memory. Programs that keep generating and unmapping code grow without bound and end in DynamoRIO's
  out-of-memory abort, status 255, not in a hang or a wrong result: JIT servers are affected, others not.
- Anything new that relocates a thread (`dr_flush_region`, coarse-grain units, detach) brings these faults back and must
  be tested with `test/regstress.c`.
- Measurements: [plan](../plan.md), "The cache reset".

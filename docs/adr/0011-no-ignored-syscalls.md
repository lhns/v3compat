# 0011: DynamoRIO handles every system call at a synch point

## Status
Accepted, 2026-11-05. Removes the unbounded-growth consequence of [0010](0010-no-cache-reset.md), whose decision stands.

## Context
Fragments retired by a flush are freed once every thread has passed a synch point. A thread blocked in a system call
counts as passed, but only if the call went through `do_syscall`. DynamoRIO runs "ignorable" calls (`futex` and `pause`
among them) inside a fragment, where it cannot be acted for, so one blocked thread keeps everything retired since it
blocked. With reset off nothing else frees them, and a program that keeps generating and unmapping code grows
until the out-of-memory abort.

## Decision
The launcher passes `-no_ignore_syscalls`, ahead of the user's options. A DynamoRIO option, so no fork change: acting
for a thread blocked inside a fragment would race its wake-up, and the mechanism for calls through `do_syscall` exists.

## Consequences
- Every system call leaves the cache. Its cost did not show in a tight system call loop or in duckdb; measurements in
  the [plan](../plan.md), "The cache reset".
- Retired fragments are freed while threads wait; `test/jitgrowth.c` fails if that stops.

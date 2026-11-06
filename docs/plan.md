# Plan

Run x86-64-v3 programs (AVX2, FMA, BMI1, BMI2, LZCNT, MOVBE) on CPUs that have AVX but not AVX2, such as
Ivy Bridge, at close to native speed.

**Status:** Phase 1. Emulation, the launcher and exec follow run on DynamoRIO built from our fork. Phase 1's whole-process
time and the thread-creation stress test are unmet; see "Open items". Decisions are in the [ADRs](adr/README.md);
this file holds the current state and the numbers behind it.

Claims are tagged: **[measured]** on real hardware, **[source]** read in source or docs, **[memory]** from
memory and unverified, **[estimate]** unmeasured.

## Goal and requirements

In order of priority: correct (a silently wrong result is the worst outcome; a clean refusal beats a guess), complete
(main executable, libraries, `dlopen`, static and musl, Go, JIT, children across `exec`), fast (startup and
near-native speed for code that rarely needs emulation), contained (opt-in per container, native passthrough on capable
hosts, a failure affects one container).

Non-goals: CPUs without AVX, AVX-512, other architectures. If you can, move the workload to an AVX2 host or rebuild it
for x86-64-v2: both are simpler and faster than anything here.

## Why this design

duckdb 2.0.0 on Ivy Bridge, against native on an AVX2 host **[measured]**:

| | `select 42` | Compute query | Peak memory | Follows `exec` |
|---|---|---|---|---|
| Native (AVX2 host) | 0.16 s | 0.59 s | 43 MB | n/a |
| QEMU user 10.0 | 4.7 s | 9.7 s | 180 MB | No, children die with SIGILL |
| Intel SDE 10.13 | 47 s | 60 s | 650 MB | Yes |

The AVXEmu preload patches sites in memory. `lzcnt` and `tzcnt` do not fault here, they silently run as `bsr` and `bsf`,
and it patches them in the main executable only, so in a shared library they return wrong answers. It cannot prove
coverage for libraries or JIT code, and does not reach static binaries **[measured]**. Kernel module, eBPF, seccomp
patching, ahead-of-time rewriting and hypervisor traps were rejected: [ADR 0002](adr/0002-same-isa-binary-translator.md).

## Design

A client on [DynamoRIO](https://github.com/DynamoRIO/dynamorio) decodes code as it first executes, so no metadata is
needed. Instructions the CPU has are copied; the others are expanded or sent to a helper
([ADR 0003](adr/0003-dynamorio-as-the-base.md), [0005](adr/0005-avxemu-core-as-helpers.md),
[0007](adr/0007-hot-ops-inline-in-phase-1.md)).

### Translation rules

| Instruction | Handling | Cost per op |
|---|---|---|
| Hot lane-wise AVX2: `vpaddq`, `vpmuludq`, `vpsrlq`, `vpsllq` | Inline, two 128-bit operations | 3 ns; 5 ns from memory **[measured]** |
| Forms AVX1 has under another opcode | Native substitution | Native |
| Other vector forms, FMA | Helper through a raw-TLS frame, no mcontext | 80-90 ns **[measured]**; FMA more |
| `lzcnt`, `tzcnt`, BMI1, BMI2, MOVBE loads | Helper through the mcontext | 320-400 ns **[measured]** |
| MOVBE stores | Inline byte swap and store | Small **[estimate]** |
| `cpuid` | Translated to report the emulated CPU | Rare |
| `xgetbv` | Native: AVX2 adds no XCR0 bits | Native |

- **Unknown instructions are refused.** Every translated form is looked up by DR opcode, encoding bits and operand classes
  ([ADR 0008](adr/0008-audited-list-keyed-on-shape.md)); anything off the list exits with status 86 if executed. Evidence and
  exclusions: [allowlist.md](allowlist.md); regenerate with `test/allow/regen.sh`. The lookup costs about 0.25 s of duckdb's
  5.7 s startup **[measured]**. hello, zstd and duckdb's three queries translate no form off the list **[measured]**.
- **Signals** map the code-cache PC back to the application instruction; an expansion must be rewindable or commit last.
- **JIT and self-modifying code**: DynamoRIO write-protects translated pages.
- **Passthrough** on capable hosts keys on a pinned node label, not CPUID: masking CPUID does not disable the silicon, so
  `lzcnt` can change meaning when a VM migrates ([ADR 0004](adr/0004-opt-in-per-container.md)).

| Work | Repo |
|---|---|
| DynamoRIO client, launcher, expansions, `cpuid`, tests | `v3compat` |
| Emulation helpers and their fixes; the semantic suite | [lhns/avxemu](https://github.com/lhns/avxemu) |
| DynamoRIO fork | [lhns/dynamorio](https://github.com/lhns/dynamorio) |
| OCI wrapper | [v3compat-runtime](https://github.com/lhns/v3compat-runtime) |

The translation cache is not built ([ADR 0006](adr/0006-read-only-translation-cache.md), proposed).

## Testing

With a translator every executed instruction passes through it, so tests matter more than the design.

1. **Spec.** Every form generated from XED's tables; CI fails if a form has no status. Of 578 forms, 84 are refused.
2. **Decoder.** Differential against a real disassembler.
3. **Semantics.** Differential against real AVX2 silicon, references recorded on AVX2 and replayed on AVX-only CPUs. The
   forms match the references apart from `mem_read` zero-fill. References come from AMD EPYC, so Intel NaN priority is
   unverified until a recording on Intel AVX2 hardware. `test/sem/known-dr.txt` lists forms where plain DynamoRIO differs
   from native (OF around `movbe` and `vbroadcasti128` loads).
4. **Paths.** In `v3compat` the suite runs through the client on Ivy Bridge with inline expansions on and off.
5. **Process.** Partly built: faults and flags, `test/regstress.c`, exec identity, setuid races. Signal injection into an
   expansion is not.
6. **Applications.** duckdb output hashes match native on Ivy Bridge; CI also runs zstd.

Strong evidence, not proof. Keep storage and database primaries off it.

## Measurements

### DynamoRIO 11.3.0 alone, AVX2 runners **[measured]**

| Workload | Native | DynamoRIO | Factor |
|---|---|---|---|
| duckdb `select 42` | 0.036 s | 0.71 s | 19.6x |
| duckdb mixed SQL | 4.74 s | 7.93 s | 1.67x |
| zstd -9 | 1.14 s | 1.31 s | 1.15x (about 1.25x on Ivy Bridge) |
| Plain C benchmark | 1.37 s | 1.82 s | 1.33x (about 1.2x on Ivy Bridge) |
| Node CPU benchmark | 1.13 s | 2.70 s | 2.4x |
| Java CPU benchmark | 0.73 s | 9.51 s | 13x |
| `java -version` | 0.024 s | 2.0 s | 84x |

JVM workloads are out of scope for now. Default Go builds (`GOAMD64=v1`) need no AVX2 and run natively. `-persist` does not
persist a client's blocks and made runs slower (4.5 s against 0.7 s). Density in duckdb: AVX2-only mnemonics 0.016% of
executed instructions, BMI2 about 0.03%. SIGILL round trip in a VM on Ivy Bridge: 12.3 us, 86 us under DynamoRIO.

### Phase 1 results

duckdb's compute query (`select sum(range*range) from range(30000000)`), AMD runner: 1.67G instructions, 135.5M AVX2, of
which `vpaddq`, `vpmuludq`, `vpsrlq`, `vpsllq` are 99.8%. BMI 0.63M, `lzcnt` 46k, `tzcnt` 11k, FMA none.

Per operation on Ivy Bridge (an empty loop iteration is about 0.5 ns):

| Path | ns per op |
|---|---|
| Inline, register sources | 2.9-3.2 |
| Inline `vpaddq` from memory | 4.9 |
| Inline with the TLS block not 32-byte aligned | 10 |
| Frame helper | 78-94 |
| Mcontext helper (`tzcnt`) | 316-402 |
| Clean call reading and writing the mcontext | 466-525 |

duckdb 2.0.0 whole-process wall time, cold; output hashes equal in all columns:

| Query | Native, AMD AVX2 | Client, AMD AVX2 | Client, Ivy Bridge |
|---|---|---|---|
| `select 42` | 0.04 s | 0.92 s | 5.1-5.4 s |
| Compute query | 0.16 s | 1.35 s | 5.9-7.4 s; 10.5 s once |
| Compute query, `threads=1` | | | 6.5 s |
| `varied.sql` (mixed SQL) | 4.56 s | 49 s | 165 s |

Startup is about 5 s of the Ivy Bridge time; the compute query adds about 1.1 s.

### Launcher and exec follow ([ADR 0009](adr/0009-static-launcher-no-drrun.md))

Statuses: 86 refused instruction, 87 the launcher refused the program, 125 launcher failure, 126 and 127 as in a shell;
app statuses and signals pass through. An exec refused under the client fails with an errno and a message on stderr.

Measured on Ivy Bridge under Docker's default seccomp, no ptrace, each child running `lzcnt` and checking DynamoRIO is mapped:

- Followed: `execve`, `execveat`, `fexecve` (also memfd), threads, `vfork`, `posix_spawn`, shebang, empty or NULL env,
  closed fds 0-2, NULL argv; glibc, static, static-PIE and musl in the directions tested.
- Refused: setuid, setgid and file capabilities where the kernel would change privileges (`EPERM`, also from a thread or
  `vfork` child), an executable but unreadable file (`EPERM`), a 32-bit child (`ENOEXEC`). Root running a setuid-root file,
  no_new_privs and nosuid mounts run, as they change nothing natively.
- Rewritten to the app: `readlink`, `open`, `stat`, `statx` of `/proc/self/exe` and its variants, short buffers included.
- Each exec opens the file once, checks that descriptor and has DynamoRIO load from it, so a swap cannot separate check and
  exec (`test/setuid.sh`, `test/exec_identity.sh`). Remaining holes give no rights, **[source]**: a process caught by a swap
  can die at startup with status 255; writes to the file are not denied (no `ETXTBSY`); an interpreter is opened by name;
  the exec needs a free descriptor; what only `binfmt_misc` could run fails with `ENOEXEC`.
- Differs from native: `/proc/self/maps` also lists DynamoRIO and the client; `/proc/self/auxv` has the loader's values;
  `DYNAMORIO_*` variables are in the app's environment. Not measured: `execve` with argv[0] of `/proc/self/exe`, and
  `/proc/self/exe` relative to a directory fd **[source]**.

### The cache reset

[ADR 0010](adr/0010-no-cache-reset.md). On GitHub AVX2 runners, AMD and Intel, not Ivy Bridge. "Bad" is a wrong output, a crash, an error exit or a timeout of
`test/apps/duckdb-varied.sql` unless noted. Before the fix main was bad 11 of 106 (once a wrong number with exit 0); stock
upstream with no client 50 of 482, 28 silent; native 0 of 60.

| On the fork with the register fix | Bad |
|---|---|
| Client, as shipped (reset off) | 0 of 230: at most 1.3% (95%) |
| DynamoRIO alone, default reset | 0 of 643 |
| DynamoRIO alone, resets forced | 0 of 521 |
| Client, resets forced, regexp query | 21 of 21; helpers only 20 of 20; no expansions 0 of 70 |
| `test/regstress.c`, DynamoRIO alone, resets forced, code churn | 11 of 551 |
| The same, default reset | 21 of 22 hang |
| The same, reset off, with code churn, signals or both | 0 of 296 |
| The same, client, reset off, about 1 kHz of signals per thread on 8 threads | 0 of 377 |

The fixed fault: the x64 trace compare's tag spill was taken as redundant, so a relocated thread got an application code
address in `rax`.

Reset off and code generation, [ADR 0011](adr/0011-no-ignored-syscalls.md): a program that generates and unmaps code while
another thread waits in a system call grew 18.4 KB per region on `-ignore_syscalls`, the default, and 0.0 on
`-no_ignore_syscalls`, over 3,000 regions **[measured]**. Cost: `getppid` 485-491 ns against 488-494, duckdb's query 39.8-40.1 s
against 39.8-40.3 s, three runs each.

**The rare crash, fixed in the fork** (branch `fix-signal-unlink-walk`). A signal arriving while a thread is in a fragment, or
in `fcache_enter` about to jump to one, unlinks that fragment from the signal handler, and the synch point it took walked the
deletion queues. The walk dropped the thread's own claim on fragments retired since its last synch point, so the fragment
could be freed under it; it then ran the free list (SIGSEGV, SIGBUS, or a silent status 255 from inside DynamoRIO). If the
slot was reused first, it could have run another fragment's code, so a wrong result was possible; none was seen. The
handler now defers the walk to the thread's next real synch point. `regstress 8 1 sig jit` under the client, 1,500 runs a
job **[measured]**: without the change, 17 of 9,000 on main and 21 of 12,000 on the same lab with an unrelated
change; with it, 0 of 12,000 (at most 0.025%, 95%), on AMD EPYC 7763, 9V45
and 9V74. On main with the fix: duckdb 0 of 240, signal stress 0 of 390, reg/flags stress 0 of 306 under the client and
0 of 306 on DynamoRIO alone, the short loop 0 of 4,500 with `-no_ignore_syscalls` and 0 of 4,500 without.

CI runs the query 30 times under the client: against a 10% bad rate that catches it with 96%, against 2%, 45%.

### The hang (fork branch `fix-pending-signal-self-link`)

Two threads signalled in one shared fragment looped forever. Not specific to Go. GitHub runners, **[measured]**:

| Test | Stock | Fork | Native |
|---|---|---|---|
| Go tight loop + GC | 356/1000 | 0/1000 | 0/1000 |
| Go thread churn, no preemption | 3/1000 | 0/1000 | not run |
| C reproducer, 8 threads | 30/30 | 0/200 | 0/30 |

`-thread_private` is not a fallback: it hung too.

## Phases

A failure at a go/no-go stops the plan. Phase 0 (is DynamoRIO good enough) is done; its startup criterion was revised after
seeing results to cold startup under 5 s on Ivy Bridge, and Phase 1's compute target to exclude startup (about 1.1 s against
3 s). Both were judged after the fact and approved by the user.

| Phase | Go/no-go | State |
|---|---|---|
| **1. Correct, not yet fast** | `lzcnt` in a `dlopen`ed library, static children, duckdb output correct; compute excluding startup under 3 s | Met, except the thread-creation stress test |
| **2. Fast** | Dense cost under 5 ns per op; compute under 1.5 s; signal injection clean | Not started |
| **3. Startup** | Warm `select 42` under 0.5 s; memory under 180 MB; bad entries rejected | Not started; needs the cache |
| **4. Production** | Wrapper integration, passthrough, canary; two weeks with no divergence from native | Not started |

## Open items

- **Whole-process time.** 5.9-7.4 s for the compute query on Ivy Bridge until Phase 3. The mixed workload is about 11x on
  AMD (1.67x with no client) and needs more forms inline.
- **Thread-creation stress test.** A test creating threads without limit near exit died after 50 minutes on stock and fork,
  passed natively; cause unknown. To return with a thread cap.
- **A thread of a vfork parent ending the process** while another waits in `vfork` for a child that has not exited hangs
  (3 of 3; native exits): the waiting thread cannot answer the exit's synch. Not tested: a child killed holding a DynamoRIO
  lock, or before its thread init. A child dying of a signal otherwise no longer hangs the parent, with `vfork` or `clone3`
  (`test/vfork_sig.c`): 9 of 9 before, 0 of 300 after, at most 1% (95%) **[measured]**.
- **Cache reset.** Flags lost in the relocation window (upstream i#400), crashes under the client with forced resets
  (likely upstream i#4219, not tested), hangs under reset with code churn. Anything new that relocates a thread needs
  `test/regstress.c`.
- **Signal injection** into an expansion is untested; asynchronous signals arrive at fragment boundaries.
- **Intel NaN priority** is unverified until a recording on Intel AVX2 hardware.
- **The fork's** signal, lock and exit changes are ours to maintain. Known limits: a signal can be delayed past a syscall in
  a shared fragment; the nudge relink is unchanged; a thread can hold a stale pointer to a flushed fragment. The lock change
  affects every thread running during exit and is the one most worth review.
- **JIT-heavy programs** may regress past AVXEmu's 1.2x: Node 2.4x, Java 13x. **FMA** under correct rounding stays slow.

## License

Apache-2.0, chosen for its patent grant. DynamoRIO core, `drmgr`, `drreg`, `drx` are BSD; `drutil`, `drwrap`, `drsyscall`
are LGPL 2.1 and ship as separate shared libraries, never statically linked; AVXEmu is CC0 **[source]**. Binaries must carry
DynamoRIO's notices.

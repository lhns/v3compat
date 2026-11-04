# Plan

Run x86-64-v3 programs (AVX2, FMA, BMI1, BMI2, LZCNT, MOVBE) on CPUs that have AVX but not AVX2, such as
Ivy Bridge, at close to native speed.

**Status:** Phase 1. DynamoRIO is decided, built only from our fork. Emulation, the launcher and exec follow run; see "Phase 1 results".

Claims are tagged: **[measured]** on real hardware, **[source]** read in source or docs, **[memory]** from
memory and unverified, **[estimate]** unmeasured.

## Goal and requirements

In order of priority:

1. **Correct.** A silently wrong result is the worst outcome. A clean refusal beats a guess.
2. **Complete.** The main executable, shared libraries, `dlopen`, static and musl binaries, Go, JIT code and
   child processes across `exec`.
3. **Fast.** Fast startup, and near-native speed for code that rarely needs emulation.
4. **Contained.** Opt-in per container. A failure affects one container, never the node. Native passthrough
   on capable hosts.

Non-goals: CPUs without AVX (they have no `ymm` registers), AVX-512, other architectures.

**When not to use this.** Move the workload to an AVX2 host or rebuild it for x86-64-v2 if you can. Both are
simpler and faster than anything here. This is for workloads that must run on the old host.

## Why existing tools are not enough

duckdb 2.0.0 (77 MB, dense v3 build), on Ivy Bridge, against native on an AVX2 host **[measured]**:

| | `select 42` | Compute query | Peak memory | Follows `exec` |
|---|---|---|---|---|
| Native (AVX2 host) | 0.16 s | 0.59 s | 43 MB | n/a |
| QEMU user 10.0 | 4.7 s | 9.7 s | 180 MB | No, children die with SIGILL |
| Intel SDE 10.13 | 47 s | 60 s | 650 MB | Yes |

SDE's startup is load-time CPU work tied to the binary: hello world takes 1.2 s and `-version` takes 45 s.
Stripping symbols, disabling the chip check and disabling child following did not help. It is closed source
and offers no persistent cache **[measured, help output]**.

[AVXEmu](https://github.com/lhns/avxemu) patches sites in memory and runs everything else natively. Its
authors report about 1.2x on a binary with sparse AVX2 **[their numbers]**. On real Ivy Bridge hardware, its
own test suite passes, and AVX2/FMA/BMI in the main executable gives correct results, including in containers
with the default seccomp profile **[measured]**. Its limits are structural:

- `lzcnt` and `tzcnt` do not fault here. They silently run as `bsr` and `bsf`. AVXEmu patches them in the main
  executable only, so in a shared library they return wrong answers **[measured]**.
- Patching needs instruction boundaries. Its Linux port proves coverage from function and unwind metadata,
  and refuses what it cannot prove. Libraries and JIT code have no such proof.
- Interposition does not reach static binaries **[measured]**.
- A review found fixable bugs in the mechanism: signals during emulation, `fork` during relocation, a thread
  with SIGILL blocked being killed.

## Options considered

| Option | Coverage | Speed | Risk to node | Static and JIT |
|---|---|---|---|---|
| Kernel module | Traps only; `lzcnt` needs an in-kernel scanner | About 1000x native per trap **[estimate]** | Panic takes the node down | Static yes, JIT `lzcnt` no |
| eBPF | No #UD hook, FPU or register-rewrite path **[memory]** | n/a | Low | No |
| Seccomp plus patching | Provable code only | Thunks or better | None | Partial |
| Ahead-of-time rewrite | Known files only | Best | None | No JIT |
| Hypervisor #UD | Traps only | VM exit per trap | Whole host | No `lzcnt` |
| **Dynamic binary translator** | **All executed code** | About 2x target **[estimate]** | None | Yes, to be proven |
| AVX2 host | All | Native | None | Yes |

**Why not a kernel module**, in spite of its appeal for static binaries:

- `lzcnt`/`tzcnt` never trap, so a trap design cannot see the one case of silent corruption already confirmed.
- In a KVM guest, #UD is in the base exception bitmap, so every fault starts with a VM exit into `handle_ud`
  **[source]**. A module saves only the signal leg.
- `register_die_notifier` and `kernel_fpu_begin_mask` are available to GPL modules, but `exc_invalid_op` is
  `noinstr`, `security_add_hooks` is `__init`, and `_install_special_mapping` is not visibly exported **[source]**.
- A bug panics the node, and the module must track kernel updates.

## Design

A **same-ISA dynamic binary translator**: a client on [DynamoRIO](https://github.com/DynamoRIO/dynamorio).
Code is decoded as it is first executed, so no metadata is needed. Most instructions are copied unchanged, and
those the CPU lacks are expanded or sent to a helper.

One mechanism replaces `lzcnt` patching, short-site trampolines, branch-target rules, live patching, the
SIGILL fallback, signal interposition and CPUID faulting, since `cpuid` is translated like anything else.

```text
OCI wrapper (v3compat-runtime)
  host has AVX2?  -> run natively
  otherwise       -> launcher -> program under the translator
                                   |-- common instructions: unchanged
                                   |-- hot lane-wise AVX2: inline expansion
                                   |-- the rest: native AVX1 equivalents, helpers (from AVXEmu)
                                   `-- anything unknown: refuse
```

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

Rules:

- **Unknown instructions are refused.** Every translated form is looked up by DR opcode, encoding bits and operand
  classes ([ADR 0008](adr/0008-audited-list-keyed-on-shape.md)). Native keys run as they are, emulated keys go through
  the expansions, and anything else exits with status 86 if executed. Evidence and exclusions:
  [allowlist.md](allowlist.md). Regenerate with `test/allow/regen.sh`. The lookup is at translation only: about 0.25 s
  of duckdb's 5.7 s startup, 480,000 translated instructions at about 1,400 cycles each **[measured]**. hello, zstd and
  duckdb's three queries translate no form off the list **[measured]**.
- **Signals** map the code-cache PC back to the application instruction. An expansion must be rewindable or
  commit its result last, and a signal-injection test must prove it.
- **JIT and self-modifying code**: translated pages are write-protected, and a write invalidates them.
- **Helpers** are rebuilt with VEX encoding, since SSE/AVX transitions are expensive on this CPU **[memory]**.
- **Migration:** masking CPUID does not disable the silicon, so `lzcnt` can change meaning when a VM moves
  between hosts **[memory]**. Passthrough must key on a pinned node label rather than CPUID. Always-expanded
  AVX1 code stays correct either way.

### Reused from AVXEmu

Kept: the emulation core (`exec.c`, `exec_bmi.c`, `softfma.c`) as helpers taking operands directly, the core semantic
fixes, and the test suite. Dropped: patching, trampolines, the SIGILL handler, live relocation, the ELF metadata
parser and probably the decoder, as DynamoRIO decodes.

The preload is a poor fallback: it refuses duckdb and even Debian's `zstd`, as it cannot prove where `lzcnt` and `tzcnt`
are **[measured]**.

### Where the work lives

| Work | Repo |
|---|---|
| DynamoRIO client, launcher, inline expansions, `lzcnt`/`tzcnt`, `cpuid`, translation cache | `v3compat` |
| Emulation helpers and their fixes; the test suite | [lhns/avxemu](https://github.com/lhns/avxemu) |
| OCI wrapper | [v3compat-runtime](https://github.com/lhns/v3compat-runtime) |

### What other translators teach

| Project | Take | Avoid |
|---|---|---|
| Intel SDE and Pin | In-process injection, translating from the first instruction | Eager load work: 42 s and 600 MB on one binary |
| FEX-Emu | 256-bit split into two 128-bit ops; disk cache keyed by build ID, with a size cap | |
| Rosetta 2 | Ahead-of-time lookup plus JIT; separate writer; write then rename; read-only cache | Writable caches: cache poisoning |
| QEMU TCG | Self-loading of ELF images | Helper-call vector lowering |
| Microsoft Prism | Evidence that AVX2/BMI/FMA emulation is shippable | |

### Translation cache

Keyed by file hash, stored as a read-only mount for containers, written by a separate process, with write then
rename, a size cap, and stale or corrupt entries rejected. Never shared writable between containers.

## Testing

The tests matter more than the design: with a translator, every executed instruction passes through it, which
is a larger correctness surface than patching a few sites.

Status: stages 1, 2, 3 and 6 are built. Of 578 forms from XED, 84 are refused. The rest match the recorded references on
the helper-fixes branch, apart from `mem_read` zero-fill, left for the translator. References come from AMD EPYC, so Intel NaN
priority is unverified until a recording on Intel AVX2 hardware. In `v3compat`, the suite runs through the client on Ivy Bridge, with inline expansions on and off (stage 4, required for every inline form). Stage 5 is partly built: faults and flags. Signal injection is not.

Developed in the AVXEmu fork, independent of the mechanism:

1. **Spec.** Every instruction form generated from XED's tables. CI fails if a form has no status.
2. **Decoder.** Differential against a real disassembler, including legacy encodings that must not be touched.
3. **Semantics.** Differential against real AVX2 silicon: all registers, flags and memory; edge values; every
   imm8; page boundaries; all rounding modes, FTZ, DAZ, NaN classes and sticky flags. References are recorded on
   AVX2 hardware and replayed on AVX-only CPUs.
4. **Paths.** Inline and helper forms of each instruction give identical results.
5. **Process.** `fork`, threads, signals at every point, `sigaltstack`, unwinding, JIT, poisoned cache.
6. **Applications.** duckdb, zstd, OpenSSL, sqlite and others: output hashes identical to native.

This gives strong evidence, not proof. Keep storage and database primaries off it.

## Order of work

1. Settle the hang, then decide on DynamoRIO. Done.
2. Merge the test suite. Done.
3. Scaffold `v3compat`: a DynamoRIO client, pinned, with no emulation; the launcher; CI; the semantic suite running through
   the client. Done when a test runs through it and fails only because emulation is missing.
4. Expose the helper interface in the fork: the emulation core as a library, with no decoder or preload parts.
5. Fix the helpers against their tests: FMA, half-written stores, `vcvtph2ps`.
6. Phase 1 below, on the fork. Hot lane-wise forms are inlined within it, ahead of the launcher work, since measurement showed helpers alone cannot reach its target ([ADR 0007](adr/0007-hot-ops-inline-in-phase-1.md)).

The scaffold comes first because it decides how helpers are called. The preload's decoder bugs are not fixed unless the
preload stays.

## Phases

Every phase has a go/no-go. A failure stops the plan.

| Phase | Work | Go/no-go |
|---|---|---|
| **0. Is DynamoRIO good enough?** | Unmodified DynamoRIO with a null client on an AVX2 host: duckdb, Go, static musl and glibc, JVM, Node, exec chains, default seccomp. Instruction density of duckdb, static and dynamic | Revised after results, see below: cold startup under 5 s on Ivy Bridge. No crashes. **Static targets and Go work** under default seccomp without ptrace |
| **1. Correct, not yet fast** | Launcher, exec follow, `/proc/self/exe`; helpers, with the hot forms inline; translated CPUID and `lzcnt`; test suite hook | `lzcnt` in a dlopen'd library correct; static children correct; duckdb correct; compute, excluding startup, under 3 s (revised after results, see below) |
| **2. Fast** | Inline expansion, flag liveness, register allocation in hot traces | Dense cost under 5 ns per op; compute under 1.5 s; signal injection clean |
| **3. Startup** | Translation cache | Warm `select 42` under 0.5 s; memory under 180 MB; bad entries rejected |
| **4. Production** | Wrapper integration, passthrough, canary | Two weeks with no divergence from native |

### Phase 0 results

DynamoRIO 11.3.0, no emulation yet, on AVX2 runners unless noted **[measured]**.

| Workload | Native | DynamoRIO | Factor |
|---|---|---|---|
| duckdb `select 42` | 0.036 s | 0.71 s | 19.6x |
| duckdb mixed SQL | 4.74 s | 7.93 s | 1.67x |
| zstd -9 | 1.14 s | 1.31 s | 1.15x (about 1.25x on Ivy Bridge) |
| Plain C benchmark | 1.37 s | 1.82 s | 1.33x (about 1.2x on Ivy Bridge) |
| Node CPU benchmark | 1.13 s | 2.70 s | 2.4x |
| Java CPU benchmark | 0.73 s | 9.51 s | 13x |
| `java -version` | 0.024 s | 2.0 s | 84x |

- **Works**, also with ptrace denied by seccomp and under Docker's default profile: static glibc, static musl,
  static-PIE, exec chains in every direction, `dlopen`, signals, C++ exceptions, `pthread_cancel`, `rseq`, self-modifying code.
- **A hang, found with Go.** See below. Open.
- **setuid** children silently lose their privileges, so they must be detected and refused.
- **`-persist`** wrote nothing without a client and made later runs slower: 4.5 s against 0.7 s. Its docs say a
  client's blocks are not persisted by default. So phase 3 means our own cache, or accepting the startup.
- **Density** in duckdb, by instructions executed in a query: AVX2-only mnemonics 0.016%, BMI2 about 0.03%,
  `vp*` at most 8.5% (the tool does not separate XMM from YMM; XMM runs natively).
- **SIGILL round trip** in a VM on Ivy Bridge: 12.3 us, 86 us under DynamoRIO, against 4.2 us on bare Haswell in AVXEmu's docs.

### Phase 1 results

duckdb's compute query (`select sum(range*range) from range(30000000)`) on an AVX2 AMD runner, executed instructions under
the client **[measured]**: 1.67G in all, 135.5M AVX2, of which `vpaddq` 60.1M, `vpmuludq` 45.0M, `vpsrlq` 15.1M and `vpsllq` 15.0M
are 99.8%. BMI 0.63M, `lzcnt` 46k, `tzcnt` 11k, FMA none. Helpers alone would take about 11 s of CPU at the frame's 80 ns.

Per operation on Ivy Bridge, against about 0.5 ns for an empty loop iteration **[measured]**:

| Path | ns per op |
|---|---|
| Inline `vpaddq`, `vpmuludq`, `vpsrlq`, `vpsllq`, register sources | 2.9-3.2 |
| Inline `vpaddq` from memory | 4.9 |
| Inline with the TLS block not 32-byte aligned | 10 |
| Frame helper | 78-94 |
| Mcontext helper (`tzcnt`) | 316-402 |
| Clean call reading and writing the mcontext, `vec_exec` | 466-525 |

duckdb 2.0.0 whole-process wall time, cold **[measured]**. Output hashes are the same in all three columns:

| Query | Native, AMD AVX2 | Client, AMD AVX2 | Client, Ivy Bridge |
|---|---|---|---|
| `select 42` | 0.04 s | 0.92 s | 5.1-5.4 s |
| Compute query | 0.16 s | 1.35 s | 5.9-7.4 s; 10.5 s once |
| Compute query, `threads=1` | | | 6.5 s |
| `varied.sql` (mixed SQL) | 4.56 s | 49 s | 165 s |

**Against the go/no-go.** `lzcnt` in a `dlopen`ed library, static children and duckdb's output are correct. Compute under 3 s is
not met as a whole-process time (5.9-7.4 s on Ivy Bridge): startup alone is about 5 s, and the query adds about 1.1 s to it.
Startup is DynamoRIO translation, which Phase 3's cache addresses. The mixed workload, at about 11x on the AMD host against
1.67x with no client, needs more forms inline.

**Criterion revised after seeing results.** Phase 1's compute target now measures compute excluding startup: about 1.1 s
against 3 s, which passes. Startup is measured separately and gated by Phase 3's warm-start target (`select 42` under
0.5 s), not by Phase 1. The whole-process 5.9-7.4 s above would have failed the original wording. This is a judgement made
after the fact, user-approved, and recorded as such, like the Phase 0 revision. **Still unmet in Phase 1:** the whole-process
time, until Phase 3; the mixed workload's 11x; the thread-creation stress test, not settled.

### Launcher, exec follow, `/proc/self/exe`

[ADR 0009](adr/0009-static-launcher-no-drrun.md). The launcher is a static, freestanding binary that execs `libdynamorio.so` with
its options in the environment, with no drrun, config file, shell or libc in the image. Statuses: 86 refused instruction, 87 refused
exec, 125 launcher failure, 126 and 127 as in a shell; app statuses and signals pass through.

All below on Ivy Bridge with no AVX2, under Docker's default seccomp and no ptrace, each child running `lzcnt` and checking DynamoRIO
is mapped **[measured]**; a subset also in a scratch image with `--read-only`, uid 65534 and the install mounted read-only.

| What | DynamoRIO alone | Now |
|---|---|---|
| `execve`, threads, `vfork`, `posix_spawn`, shebang, empty or NULL env, closed fds 0-2; glibc, static, static-PIE, musl in the directions tested | Follows | Same |
| `execveat`, `fexecve`, also on a memfd or a close-on-exec fd | Child runs untranslated, silently | Follows |
| `execve` with NULL argv | Child untranslated, or DynamoRIO crashes on a script | Follows |
| Executable but unreadable file | Child untranslated, silently [source] | Refused, 87 |
| setuid, setgid, file capabilities, where the kernel would change privileges | Runs with the wrong rights | Refused, 87, also from a thread or `vfork` child |
| 32-bit child | `execve` fails `ENOENT` [source] | Refused, 87 |
| argv0; `AT_EXECFN`, `AT_PHDR`, `AT_PHNUM`, `AT_SECURE` | Right | Same |
| `readlink`, `readlinkat` of `/proc/self/exe`, `/proc/<pid>/exe`, `/proc/<tid>/exe` | Right, but -1 on a buffer shorter than the path | The kernel's answer: truncates, `EINVAL` on size 0 |
| `readlink` of `/proc/thread-self/exe`; `open`, `stat`, `statx` of any of them | The loader | The app, also if a memfd |

Refusal follows the kernel's rule, not the mode bits: root running a setuid-root file, no_new_privs and nosuid mounts run, as they
change nothing natively.

Still differs from native **[measured]**:

- `/proc/self/maps` lists DynamoRIO and the client too, and for a PIE the first named mapping is the client. Not filtered: a program
  choosing addresses from it must see what is occupied.
- `/proc/self/auxv` has the loader's values (`getauxval` is right). `/proc/<pid>/exe` read from another process shows the loader;
  fixing that needs `CAP_SYS_RESOURCE`.
- `DYNAMORIO_EXE_PATH`, `_RUNUNDER` and `_OPTIONS` are in the app's environment. The client's descriptor for the executable is in
  `/proc/self/fd`.
- `clone3` returns `ENOSYS`, as under Docker's default seccomp: a static glibc `posix_spawn` child dies with SIGSEGV under DynamoRIO's
  `clone3`, with or without this client. The cause is open and this is a stopgap, to be removed with it; libcs fall back to `clone`.
- `AT_EXECFN` after `fexecve` is a path, not `/dev/fd/N`.

Not measured: `execve` with argv[0] of `/proc/self/exe` gets the app's path instead **[source]**, and `/proc/self/exe` reached relative
to a directory fd is not rewritten **[source]**.

### The hang

Settled in [lhns/dynamorio](https://github.com/lhns/dynamorio), branch `fix-pending-signal-self-link`. **[measured]**, on
GitHub runners, not Ivy Bridge.

Not specific to Go: a C reproducer hangs too. When two threads get a signal in one shared fragment, the first unlinks it
and the second sees it already unlinked and records nothing. The first then relinks it, and the second loops forever
with its signal pending. Fixing that exposed two more stock bugs: a shared lock leaked once exit begins, and a thread
cloned during exit that deadlocks with the exit synch.

| Test | Stock | Fork | Native |
|---|---|---|---|
| Go tight loop + GC | 356/1000 | 0/1000 | 0/1000 |
| Same, `GOAMD64=v3` | not run | 0/1000 | 0/1000 |
| Go thread churn, no preemption | 3/1000 | 0/1000 | not run |
| C reproducer, 8 threads | 30/30 | 0/200 | 0/30 |

DynamoRIO's 33 signal and thread tests pass, with no measurable cost. **Not settled:** a stress test creating threads
without limit near exit died after 50 minutes on stock and fork, but passed natively; the cause is unknown and the test
is to return with a thread cap. `-thread_private` is not a fallback: it hung too, costing about 25% more time and 50%
more memory on 100 threads of duckdb.

**Criterion revised after seeing results.** The original go/no-go was startup under 1 s and under 2x native. Startup
was 18-84x, so it failed. The 2x limit cannot hold for a program that natively starts in 0.04 s, as translation is
the work being measured, so absolute time replaces it. This is a judgement made after the fact and is recorded as
such. duckdb's startup on Ivy Bridge cannot be measured until emulation exists: native is about 4x slower there,
which suggests about 3 s **[estimate]**.

**Scope.** JVM workloads are out of scope for now, at 13x and 84x: move those pods or rebuild them. Default Go builds
(`GOAMD64=v1`) need no AVX2 and run natively.

**If Phase 0 fails:** use the AVXEmu preload for provable images only, refuse the rest, or write our own translator
only if the numbers justify it.

## Risks and open questions

- **The speed target is an estimate.** Dense AVX2 cost is unproven by anything measured so far.
- **Dependency on DynamoRIO.** Large and mature, with its own bugs in every executed instruction path.
- **The fork.** Its signal, lock and exit changes are ours to maintain. Known limits: a signal can be delayed past a
  syscall in a shared fragment; the nudge relink is unchanged; a thread can hold a stale pointer to a flushed fragment.
  The lock change affects every thread running during exit and is the one most worth a review.
- **JIT-heavy programs** may regress past AVXEmu's 1.2x. Measured: Node 2.4x, Java 13x.
- **FMA** under correct rounding stays slow. FMA-heavy programs may not reach the targets.

## License

Apache-2.0, chosen for its patent grant. Compatible with its inputs:

- DynamoRIO core and `drmgr`, `drreg`, `drx`: BSD **[source]**.
- DynamoRIO's `drutil`, `drwrap`, `drsyscall`: LGPL 2.1 **[source]**. They ship as separate shared libraries, never
  statically linked.
- AVXEmu: CC0 **[source]**.

Binaries must carry DynamoRIO's notices.

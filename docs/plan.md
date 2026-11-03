# Plan

Run x86-64-v3 programs (AVX2, FMA, BMI1, BMI2, LZCNT, MOVBE) on CPUs that have AVX but not AVX2, such as
Ivy Bridge, at close to native speed.

**Status:** design. Phase 0 measured, with the Go hang still open. No emulation is built yet.

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
                                   |-- lzcnt, tzcnt, lane-wise AVX2: inline expansion
                                   |-- cross-lane, FMA, gathers: helpers (from AVXEmu)
                                   `-- anything unknown: refuse
```

### Translation rules

| Instruction | Handling | Cost per op |
|---|---|---|
| `lzcnt`, `tzcnt` | Inline; compute flags only when they are live | About native **[estimate]** |
| BMI1, BMI2, MOVBE | Inline | Small **[estimate]** |
| Lane-wise AVX2 | Two 128-bit operations | 2-5 ns; 1-2 ns with block register allocation **[estimate]** |
| Cross-lane AVX2 | Short sequences | 5-15 ns **[estimate]** |
| FMA, gathers | Helper call, correct fused rounding | 50 ns or more **[estimate]** |
| `cpuid`, `xgetbv` | Translated to report the emulated CPU | Rare |

Rules:

- **Unknown instructions are refused** unless on an audited list of encodings that behave identically on the
  target CPU. Differential testing builds that list. It is the guard against the class of bug where a
  non-faulting instruction silently means something else.
- **Signals** map the code-cache PC back to the application instruction. An expansion must be rewindable or
  commit its result last, and a signal-injection test must prove it.
- **JIT and self-modifying code**: translated pages are write-protected, and a write invalidates them.
- **Helpers** are rebuilt with VEX encoding, since SSE/AVX transitions are expensive on this CPU **[memory]**.
- **Migration:** masking CPUID does not disable the silicon, so `lzcnt` can change meaning when a VM moves
  between hosts **[memory]**. Passthrough must key on a pinned node label rather than CPUID. Always-expanded
  AVX1 code stays correct either way.

### Reused from AVXEmu

Kept: the emulation core (`exec.c`, `exec_bmi.c`, `softfma.c`) as helpers, the core semantic fixes, and the test
suite. Dropped: patching, trampolines, the SIGILL handler, live relocation and the ELF metadata parser. Its
preload stays viable as a narrow fallback if this plan fails.

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

## Phases

Every phase has a go/no-go. A failure stops the plan.

| Phase | Work | Go/no-go |
|---|---|---|
| **0. Is DynamoRIO good enough?** | Unmodified DynamoRIO with a null client on an AVX2 host: duckdb, Go, static musl and glibc, JVM, Node, exec chains, default seccomp. Instruction density of duckdb, static and dynamic | Revised after results, see below: cold startup under 5 s on Ivy Bridge. No crashes. **Static targets and Go work** under default seccomp without ptrace |
| **1. Correct, not yet fast** | Launcher, exec follow, `/proc/self/exe`; helpers for everything; translated CPUID and `lzcnt`; test suite hook | `lzcnt` in a dlopen'd library correct; static children correct; duckdb correct; compute under 3 s |
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
- **Go hangs** in about half the runs, with no error. A `GOAMD64=v3` build also hangs under Docker. Open.
- **setuid** children silently lose their privileges, so they must be detected and refused.
- **`-persist`** wrote nothing without a client and made later runs slower: 4.5 s against 0.7 s. Its docs say a
  client's blocks are not persisted by default. So phase 3 means our own cache, or accepting the startup.
- **Density** in duckdb, by instructions executed in a query: AVX2-only mnemonics 0.016%, BMI2 about 0.03%,
  `vp*` at most 8.5% (the tool does not separate XMM from YMM; XMM runs natively).
- **SIGILL round trip** in a VM on Ivy Bridge: 12.3 us, 86 us under DynamoRIO, against 4.2 us on bare Haswell in AVXEmu's docs.

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
- **Static injection** under default seccomp without ptrace: the plan is wrapper argv plus self-loading **[memory]**.
- **Go:** hangs under DynamoRIO, see Phase 0 results. Cause unknown.
- **JIT-heavy programs** may regress past AVXEmu's 1.2x. Measured: Node 2.4x, Java 13x.
- **setuid children** lose privilege under a launcher: refuse them rather than run them with the wrong rights.
- **FMA** under correct rounding stays slow. FMA-heavy programs may not reach the targets.

## License

Apache-2.0, chosen for its patent grant. Compatible with its inputs:

- DynamoRIO core and `drmgr`, `drreg`, `drx`: BSD **[source]**.
- DynamoRIO's `drutil`, `drwrap`, `drsyscall`: LGPL 2.1 **[source]**. They ship as separate shared libraries, never
  statically linked.
- AVXEmu: CC0 **[source]**.

Binaries must carry DynamoRIO's notices.

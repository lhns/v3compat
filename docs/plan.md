# Plan

Run x86-64-v3 programs (AVX2, FMA, BMI1, BMI2, LZCNT, MOVBE) on CPUs that have AVX but not AVX2, such as
Ivy Bridge, at close to native speed.

**Status:** Phase 1 met (its compute target excludes startup); Phase 2, speed, in progress. Emulation, the launcher and
exec follow run on DynamoRIO built from our fork. Whole-process time, startup included, is still far from native; see
"Open items". Decisions are in the [ADRs](adr/README.md); this file holds the current state and the numbers behind it.
How to build, test and run labs: [howto.md](howto.md).

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

As of main. Costs are Ivy Bridge unless noted; GitHub runners show helpers 2-4x cheaper.

| Instruction | Handling | Cost per op |
|---|---|---|
| Hot lane-wise AVX2: add, sub, compare, min, max, `vpmullw`/`vpmulld`/`vpmuludq`, `vpshufb`, shifts; `vpmovzx`/`vpmovsx`, `vpbroadcast` (register source; also to xmm), `vpabs`; `vpermd`/`vpermps`, `vpermq`/`vpermpd`, `vpshufd`/`vpshufhw`/`vpshuflw`, `vpmovmskb` | Inline, two 128-bit halves | 2-5 ns **[measured]**; 2.2 ns for `vpcmpeqd`, `vpmovzxbq`, `vpbroadcastb`, `vpabsd` on EPYC 9V45 |
| Forms AVX1 has under another opcode, including ymm `vpand`/`vpor`/`vpxor`/`vpandn` | Native substitution | Native (`vpand` 0.06 ns **[measured]**) |
| Other vector forms (`vpsllv*`, `vpunpck*`, `vpack*`, ...), FMA | Helper through a raw-TLS frame, no mcontext | 80-90 ns **[measured]**; FMA more |
| `rorx`, `andn`, `mulx`, `shlx`, `shrx`, `sarx`, `bzhi`, `tzcnt`, `lzcnt`, MOVBE loads and stores | Inline | 0.6-1.7 ns **[measured]** for the shifts, `bzhi`, `tzcnt`, `lzcnt` (Xeon 8573C; 80-85 ns as helpers) |
| `bextr`, `blsi`/`blsr`/`blsmsk`, `pdep`, `pext` | Helper through the mcontext | 320-400 ns **[measured]** |
| AVX2 gathers | Inline, unrolled per element: one real load each, the mask and destination written back per element; also under `V3COMPAT_HELPERS_ONLY=1` | Not measured |
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
| DynamoRIO client, launcher, expansions, `cpuid`; tests of what the client does around an instruction (fault state, flags, scratch, segments, refusals, exec) | `v3compat` |
| Emulation helpers and their fixes; the semantic suite: every emulated form's result against silicon, which `semantic-hw` runs under the client | [lhns/avxemu](https://github.com/lhns/avxemu) |
| DynamoRIO fork | [lhns/dynamorio](https://github.com/lhns/dynamorio) |
| OCI wrapper | [v3compat-runtime](https://github.com/lhns/v3compat-runtime) |

The translation cache is built for tests and labs through Phase 2 and not deployed ([ADR 0006](adr/0006-read-only-translation-cache.md),
accepted); its design, what Phase 2 built and the owner's open decisions: [translation-cache.md](translation-cache.md).

## Testing

With a translator every executed instruction passes through it, so tests matter more than the design.

1. **Spec.** Every form generated from XED's tables; CI fails if a form has no status. Of avxemu's 578 forms, 574 are
   tested; the 4 refused are the F16C conversions, which Ivy Bridge has natively. `test/sem/refused.txt` lists the
   tested forms the client still refuses: none, and `semantic-hw` fails if the list is not exact either way.
2. **Decoder.** Differential against a real disassembler.
3. **Semantics.** Differential against real AVX2 silicon, references recorded on AVX2 and replayed on AVX-only CPUs. The
   forms match the references apart from `mem_read` zero-fill. The reference was recorded on AMD EPYC. On Intel Xeon
   8370C (GitHub runners, two machines) all 574 forms of the suite, every case, match it natively, under the client and
   with helpers only, so NaN priority and every other recorded value agree with Intel **[measured]**. lhns/avxemu's
   recording on an older Intel part differs only in a faulting `movbe` store, which the 8370C does as AMD does.
4. **Paths.** In `v3compat` the suite runs through the client on Ivy Bridge with inline expansions on and off.
5. **Process.** Partly built: faults and flags, `test/regstress.c` (with asynchronous signals: `sig`, and `xlsig`, below),
   exec identity, setuid races.
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

JVM workloads were out of scope then; they are in scope now ("Open items"). Default Go builds (`GOAMD64=v1`) need no AVX2 and run natively. `-persist` does not
persist a client's blocks and made runs slower (4.5 s against 0.7 s). Density in duckdb: AVX2-only mnemonics 0.016% of
executed instructions, BMI2 about 0.03%. SIGILL round trip in a VM on Ivy Bridge: 12.3 us, 86 us under DynamoRIO.

### Phase 1 results

duckdb's compute query (`select sum(range*range) from range(30000000)`), AMD runner: 1.67G instructions, 135.5M AVX2, of
which `vpaddq`, `vpmuludq`, `vpsrlq`, `vpsllq` are 99.8%. (The first inline set; the lane-wise forms since added were chosen by executed counts in duckdb and zstd.) BMI 0.63M, `lzcnt` 46k, `tzcnt` 11k, FMA none.

Per operation on Ivy Bridge (an empty loop iteration is about 0.5 ns):

| Path | ns per op |
|---|---|
| Inline, register sources | 2.9-3.2 |
| Inline `vpaddq` from memory | 4.9 |
| Inline with the TLS block not 32-byte aligned | 10 |
| Frame helper | 78-94 |
| Mcontext helper (`tzcnt`, since inlined) | 316-402 |
| Clean call reading and writing the mcontext | 466-525 |

duckdb 2.0.0 whole-process wall time, cold; output hashes equal in all columns:

| Query | Native, AMD AVX2 | Client, AMD AVX2 | Client, Ivy Bridge |
|---|---|---|---|
| `select 42` | 0.04 s | 0.92 s | 5.1-5.4 s |
| Compute query | 0.16 s | 1.35 s | 5.9-7.4 s; 10.5 s once |
| Compute query, `threads=1` | | | 6.5 s |
| `varied.sql` (mixed SQL) | 4.56 s | 49 s | 165 s |

Startup is about 5 s of the Ivy Bridge time; the compute query adds about 1.1 s.

### JIT workloads and the performance baseline (2026-11-12)

`bench/` on GitHub runners (EPYC 7763 and Xeon 8370C), every configuration interleaved in one job, median wall,
digests equal native wherever a run succeeded, main before the inline PRs **[measured]**:

| Workload | Native | DynamoRIO alone | Client |
|---|---|---|---|
| Node `bench/jit.js` | 0.93-0.98 s | 4.1-4.5 s | 5.2-6.0 s |
| `node -e 0` / `npm --version` | 0.02 / 0.07 s | 0.46-0.51 s / not measured | 0.73-0.83 / 2.95-3.3 s |
| Java `bench/Bench.java` | 1.15-1.21 s | 17.8-22.8 s | 85-120 s (5 of 9 runs) |
| `java -version` | 0.02-0.03 s | 2.8-3.6 s | 3.7-4.8 s |
| javac (compiling `Bench.java`) | 0.78-0.81 s | 78-93 s | crashes (below) |
| duckdb compute / varied | 0.15-0.17 / 4.8-5.2 s | 0.8-0.9 / 8.4-8.9 s | 1.4-1.7 / 14.9-16.2 s |

- **Two kinds of cost.** Emulation-dense code pays per helper call: Java's bit loop executed 318M `shrx`, 164M `tzcnt`
  and 159M `lzcnt` through the mcontext helper (about 85 ns each on a runner), duckdb's mixed query 649M BMI and 515M
  AVX2. JIT-heavy startup pays DynamoRIO: javac builds 7.9M blocks; 36-52% of its time is the kernel serving
  `/proc/self/maps` reads (814k `read` calls, 148k `mprotect`, 22k signals) and most of the rest is libdynamorio.
  Most of those reads were DynamoRIO toggling its own generated code's protection, fixed in the fork (below, "Gencode
  protection"): javac's reads fell to about 50k, and to about 200 with the new-module check fixed (below).
- **Any client costs 15-45% over DynamoRIO alone** on JIT startup (Node 2.63 to 3.03 s with an empty client, 3.26 s with
  ours): DynamoRIO fully decodes every block for a client. Our own translation work is about 3% of samples.
  `-fast_client_decode` is internal and does not allow changing app code. The launcher's options make no difference.
- **Node's v3 forms come from glibc's AVX2 string routines**, not V8 (turning V8's v3 flags off changes no count).
- **FMA** through the helper, with avxemu's integer fast path for round to nearest with normal operands and result
  (lhns/avxemu#11) **[measured]**, EPYC 9V74, one interleaved job against the previous pin, ns/op: `vfmadd231ss` 76 to 37,
  `sd` 68 to 42, `ps`/`pd` xmm 279 to 72 and 136 to 60, `ps`/`pd` ymm 351 to 91 and 171 to 78; dgemm 0.78 to 0.39 s, sgemm
  0.79 to 0.26 s, Horner 0.19 to 0.11 s; Java's `Math.fma` round 593 to 375 ms (9V74), 455 to 270 ms (9V45). The lean
  call (#39) takes scalar FMA another 205 to 90 ns on EPYC 7763. Only `Math.fma` in Java hit it among the workloads (65M).
- **Inline PRs**, one job each against main: #33, merged (`shlx`, `shrx`, `sarx`, `bzhi`, `tzcnt`, `lzcnt`: 80-85 to
  0.6-1.7 ns, duckdb varied 13.93 to 12.43 s, Java's bit round 7.0 s to 96 ms). #35, merged (vector forms: about 14 to 2.2 ns,
  duckdb varied 8.32 to 6.67 s). #40, merged (broadcasts to xmm, `vpmovmskb`, `vpshufd`, `vpermq`/`vpermd`: about 25-27 to 3.1 ns,
  duckdb varied 10.63 to 10.09 s on EPYC 7763). #39, merged (lean helper call, ADR 0007 (4)): a frame helper 27.7 to 23.5 ns
  (Xeon 8370C), 20.8 to 17.5 (EPYC 9V74). None measured on Ivy Bridge.

### Startup breakdown (2026-11-12)

GitHub runners, lab `lab-pcache-startup`, medians of 10 interleaved reps **[measured]**; details and the profile in
[translation-cache.md](translation-cache.md). duckdb `select 42` on EPYC 7763: native 0.03 s, DynamoRIO alone 0.69 s (0.50 s without traces), empty
client 0.89 s (0.65 s), client 1.09 s (0.80 s). It builds 77k blocks and 3.4k traces, all in
file-backed modules; a static hello is 0.02 s under the client, so startup is per-block work, about 14 µs a block.
`node -e 0`: 61k blocks, 94% in modules. DynamoRIO's `-persist` without a client: duckdb 1.6-3.0 s using its cache against
0.47 s without; with a client it writes nothing.

### Launcher and exec follow ([ADR 0009](adr/0009-static-launcher-no-drrun.md))

Statuses: 86 refused instruction, 87 the launcher refused the program, 89 a vfork child was killed holding a lock of
DynamoRIO's (the fork, below), 125 launcher failure, 126 and 127 as in a shell;
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
| Client, as shipped (reset off) | 0 of 230 at the time: at most 1.3% (95%); 0 of 240 at each later pin |
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
0 of 306 on DynamoRIO alone, the short loop 0 of 4,500 with `-no_ignore_syscalls` and 0 of 4,500 without. With code churn and no app signals, 2 of 12,000 still
crashed under the client with the fix (2 of 3,000 before; 0 of 6,000 on DynamoRIO alone); that is the torn exit below.

**The torn exit, fixed in the fork** (branch `fix-exit-offset-16`). On AMD EPYC 9V45 (Zen 5) a thread running a fragment's
exit `jmp` while another thread re-links it jumped to neither the old nor the new target: instruction fetch read the rel32
half old, half new when it straddled a 16-byte boundary. DynamoRIO only kept patched offsets within a cache line. Each bb's
start now shifts by 0-3 bytes so that every exit offset stays within 16 bytes (a quadword where possible); trace exits and
stubs are padded to 4 bytes. 9V45 lab, `regstress 8 1 jit` under the client, 6,000 runs a job **[measured]**: base 6 of
24,000, fix 0 of 102,000 (at most 0.003%, 95%). DynamoRIO's own suite, base and fix built on the same runner: the same
failures apart from known flakes, except `api.static_prepop` (below).

**`MAP_32BIT`, fixed in the fork** (branch `fix-map32bit`). The kernel looks for `MAP_32BIT` space only in [1G,2G), above a
floor randomized by up to 32 MB. For a non-PIE program DynamoRIO reserves its 1 GB of code space at `-vm_base` (0x3f000000)
plus up to 256 MB, which covers all of that range in about 6% of runs, so the mmap failed (`test/gather.c` exited 2). After
that ENOMEM DynamoRIO now maps at a free range below 2 GB, a hint the kernel accepts for `MAP_32BIT`. Tested by
`test/map32.c`, once as placed and once with `-vm_base` over the whole range. GitHub runners **[measured]**: before, gather 19
of 300 and a bare `MAP_32BIT` mmap 17 of 300 (PIE 0 of 300, native 0 of 300), the forced case 20 of 20; after, gather 0 of
700, the mmap 0 of 600 and the forced case 0 of 300. No change elsewhere: duckdb 0 of
240, signal stress 0 of 390, reg/flags stress 0 of 306 under the client and 0 of 306 on DynamoRIO alone, the short loop
0 of 4,500.

**A process ending while a thread waits in `vfork`, fixed in the fork** (branch `fix-vfork-exit`). The exit's synch sent
each other thread a suspend signal and waited for it, but the kernel holds a thread in `vfork` until its child execs or
exits, so the process waited for its child; the child could also block on a lock the exiting thread held. A thread blocked
in a vfork-like syscall is now left to the kernel: its record is dropped, its state kept, and if the child lets it go
first it exits. Whether it is in the kernel comes from `/proc/<pid>/task/<tid>/syscall`; a non-dumpable process cannot
read that, and then the mark DynamoRIO sets around the syscall decides. A group that dies of a signal while a vfork child
shares the address space now ends only itself, so the child runs on, as natively. `test/vfork_exit.c`, GitHub runners
**[measured]**: before, every run hung, under the client and on DynamoRIO alone, in all four ways of ending (another thread
exits, SIGTERM, the main thread waiting, `clone3`); after, 0 of 1,200 bad under the client and 0 of 1,200 on DynamoRIO
alone (at most 0.25%, 95%), including 200 where the child exits or execs as the process ends, the child finishing every
time; non-dumpable 0 of 300 (before the fallback 3 of 3 runs hung). A vfork that fails, or a child killed before its
thread init, left the count of threads not yet initialized raised, and under the client the exit then waited for it
(about 50 s from the option defaults); parent and child now share it and whoever gets to it drops it. Failing vforks
under the client: 15 of 15 waited over 20 s before, 0 of 200 after **[measured]**; the killed child is not measured
(below). DynamoRIO's suite, base and fix on the same runner: the same failures apart from the known flake
`client.attach_test`. No change elsewhere: duckdb 0 of 240, signal stress 0 of 390, reg/flags stress 0 of 306 under the
client and 0 of 306 on DynamoRIO alone, the short loop 0 of 4,500.

That fix had two races, both fixed (fork branch `fix-vfork-exit-race`). The exit looked once for a thread in vfork and
then waited without bound for its suspend signal; a thread still inside the clone syscall reads as `running`, so it was
not abandoned, then slept in vfork with the signal pending: a hang. Now the wait also abandons it once it is in vfork.
An abandoned thread let go by its child came back with no record and its DR TLS still the clone copy, so its child's
SIGCHLD aborted the process ("multiple native handlers", status 40); it now exits on any signal (most aborts left were
the exiting thread's, fixed below with #19). Both need load:
`test/vfork_exit_load.sh` (CI, 60 s) runs each way of ending, 8 copies at once; it failed 19 of 20 runs before, 0 of 20
after. Same runner, `vfork_exit 20` per run, 1,920 runs a way of ending under the client on EPYC 7763, 9V45 and 9V74
**[measured]**: before, the hang in 28, 29, 2 and 31 runs (another thread exits, SIGTERM, the main thread waiting,
`clone3`) and 16 in the race mode (12 aborts, 4 hangs); after, 0 in 9,600 (at most 0.16% a way, 95%). DynamoRIO alone,
1,600 runs: 14 before, 0 after. DynamoRIO's suite, base and fix on the same runner: the same failures apart from known
flakes. No change elsewhere: duckdb 0 of 240, signal stress 0 of 390, reg/flags stress 0 of 306 under the client and 0
of 306 on DynamoRIO alone, the short loop 0 of 4,500.

**Thread creation that fails, fixed in the fork** (branch `fix-thread-exit`). The thread-creation stress test is back,
capped, as `test/thread_exit.c`: threads keep starting threads (at most 64, or 1,000) while the main thread exits; in one
mode every creation fails, as at a thread limit (`RLIMIT_NPROC` 0). Before a thread-creating clone DynamoRIO raises its
count of threads not yet initialized and allocates the new thread's stack, both undone by the new thread; a clone that
failed left both, so every later synch of all threads, the exit's included, waited for the count until its loop limit,
and each failure leaked the stack **[source]**. Every creation failing, 10 runs each **[measured]**: before, every exit
took 51 s, under the client and on DynamoRIO alone, with RSS at 78-82 MB after about 19,000 failures; after, 0.4-0.6 s
and 6 MB after 64,000. With 1,000 threads churning under the client the exit is prompt: 0 of 420 runs bad, at most 5.9 s
(at most 0.7%, 95%), 195 of them on EPYC 7763. The original uncapped run's death after 50 minutes was not reproduced.
No change elsewhere: duckdb 0 of 240, signal stress 0 of 390, reg/flags stress 0 of 306 under the client and 0 of 306 on
DynamoRIO alone, the short loop 0 of 4,500.

**Signals sent by `kill`, fixed in the fork** (branch `fix-kill-signal-hold`). DynamoRIO holds an asynchronous signal to
the end of the fragment it interrupted, but not one it may not always delay (SIGXCPU, SIGSEGV, SIGABRT and others): sent by
another thread, that one was delivered at once, its state translated from wherever the fragment was, inside an expansion
too. That state was often wrong: `regstress xlsig` (SIGXCPU from another thread) failed every run (at least 100 each) under
the client, with helpers only, and on DynamoRIO alone (crashes, wrong flags, a GPR wrong at an emulated instruction); natively 0
**[measured]**. A signal sent with `kill` or `tgkill` now waits for the fragment's end like SIGUSR1: 0 of 190 runs in each
of those three and with code churn (at most 1.6%, 95%), on EPYC 7763, 9V74 and 9V45 and Xeon 8573C, with 4,000-8,000
signals a run at an emulated instruction and every register right **[measured]**. SIGUSR1 was already clean. A signal
from another thread is no longer delivered inside an expansion; faults there are translated (`test/fault_state.c`). The stress
numbers above were taken with this change in.

**The fork on upstream master** (branch `rebase-master-20261110`). The fixes above are rebased from `6847bb2`
(2025-01-10) onto upstream `058821c` (2026-11-09). Upstream had made none of them redundant, so none was dropped; its
widened TLS offsets made the spill-slot fix key on `reg_spilled`; the reset query on DynamoRIO alone, resets
forced, is still bad 159 of 160 on upstream and 0 of 160 on the fork **[measured]**. Its decoder now names CET, WAITPKG and FRED
instructions it took for `nop`, fences or `clac`, and lists `movntdqa` load-first, so the list's keys were recomputed
from the committed rows: the evidence is unchanged, and 12 keys were added (`endbr32`, `endbr64`, `rdssp`). Its
`drmgr.h` turns the plain exit and syscall-filter registrations into undefined symbols, so the client registers them
through drmgr. On the rebased fork **[measured]**: the 9V45 torn-exit lab 0 of 28,000 (7 runners); `regstress xlsig` 0
of 200 under the client, with helpers only and on DynamoRIO alone; `vfork_exit` 0 of 2,400 process ends on DynamoRIO alone, under the
client see "That fix had two races" above; failed thread creation exits in at most 0.6 s and 1,000 churning
threads in at most 5.3 s (120 runs); duckdb 0 of 240, signal stress 0 of 390, reg/flags stress 0 of 306 under the
client and 0 of 306 on DynamoRIO alone, the short loop 0 of 4,500. DynamoRIO's suite, upstream and fork on the same
runner, release and debug on four runners: the same failures apart from known flakes, once the decoder test expects
66 `movbe` as `data16 movbe`, as it does 66 `lzcnt`.

**Trace teardown under any client, fixed in the fork** (branch `fix-trace-blk-vmlists`,
[lhns/dynamorio#8](https://github.com/lhns/dynamorio/pull/8)). javac and Java crashed in `remove_fraglist_entry`
under any client, an empty one too, on the fork and on stock upstream: javac 8 of 8 under the client, 7 of 8 with an
empty client. A trace keeps per-block area lists only when a client is present; `end_and_emit_trace` hid the trace from
flushes (cleared `trace_vmlist`, `trace_tag`) before it could go nolinking, but freed those lists only afterwards, so a
flush by another thread in that window freed their entries as stale (case 9381) under the builder. Lab-only prints
found it (`lab-perf-trace-diag`, run 43242817702). The lists now go with `trace_vmlist`. After **[measured]**: 0 of 50
javac crashes in libdynamorio under the client and 0 of 50 with an empty client, the Java benchmark 0 of 12, no flush
left a trace's lists behind (run 43247919081); duckdb 0 of 240, signal stress 0 of 390, reg/flags stress 0 of 306
under the client and 0 of 306 on DynamoRIO alone, the short loop 0 of 4,500 (run 43248017594). DynamoRIO's suite,
base and fix on one runner: the same failures apart from known flakes and `client.memory_dump_test`,
which fails from the build directory's name (below, "Known flakes"). Reproduce with `lab/javac.sh`.

**Gencode protection without reading `/proc/self/maps`, fixed in the fork** (branch `gencode-prot-no-maps`, merged as `14836b0`,
[lhns/dynamorio#9](https://github.com/lhns/dynamorio/pull/9)). `protect_generated_code()` asked `get_memory_info()` for
the bits to keep on every toggle, and DynamoRIO's own reservation is always answered from `/proc/self/maps`; flushes
toggle twice per special-IBL relink. That was 91-93% of the maps reads for `java -version` and javac (lab-only counting
branch `lab-perf-maps-callers`, run 43279669317). The bits are known (r-x; r-- for the `-satisfy_w_xor_x` view) and are
now passed. Base and fix interleaved on one Xeon 6975P-C (run 43333748319, 6 runs each, all succeeded but one base javac
on DynamoRIO alone) **[measured]**: `java -version` 2.37 to 1.50 s under the client and 1.95 to 1.11 s alone, javac 81.8 to
66.6 s and 57.6 to 43.8 s, Node 3.84 to 3.78 s, duckdb unchanged; javac's `read` calls 730-773k to 52-56k. The
`mprotect` calls stay. Stress set unchanged (duckdb 0 of 240, signal 0 of 390, reg/flags 0 of 306 under the client and 0
of 306 alone, short 0 of 4,500), ctest 272 of 272, DynamoRIO's suite the same failures on base and fix (run 43333761161).

**The new-module check without reading `/proc/self/maps`, fixed in the fork** (branch `memquery-procmap`, merged as `0bd5cd8`,
[lhns/dynamorio#13](https://github.com/lhns/dynamorio/pull/13)). After #9 nearly all remaining maps reads were
`memquery_from_os()` for the i#1760 check on each new code area outside a module. Linux 6.11+ answers it with the
`PROCMAP_QUERY` ioctl; free addresses, `[vsyscall]` and older kernels still walk the file. Both paths answered every
query in one lab-only build: 0 mismatches in 245k (run 43400000808). Base `14836b0` against the fix and a stub without the
check, interleaved, 20 reps (javac 6), all succeeded **[measured]**: `java -version` 1.37 to 1.18 s under the client on EPYC 9V45
and 2.68 to 2.25 s on 7763, javac 51.5 to 49.8 s and 93.7 to 91.1 s, Node 3.24 to 2.80 s and 5.49 to 5.03 s, duckdb
unchanged, at the stub's level; javac's `read` calls 50-54k to about 200. Stress set unchanged (duckdb 0 of 240, signal
0 of 390, reg/flags 0 of 306 under the client and 0 of 306 alone, short 0 of 4,500), DynamoRIO's suite the same failures
on base and fix once built in the same directory (runs 43400012869, 43408308171).

**The IBL's deletion race, fixed in the fork** (branch `fix-ibl-tag`, [lhns/dynamorio#11](https://github.com/lhns/dynamorio/pull/11)).
A thread retiring a shared IBL entry points its start pc at `target_delete` and then overwrites its tag with `FAKE_TAG`; a
thread that had matched the tag but read the redirected pc reread the tag there, could get `FAKE_TAG`, and dispatch built
a block at -1 and forged a SIGSEGV. The hit path now keeps the matched tag in a TLS slot for `target_delete`.
`test/trace_flush.c` (now a ctest), base `14836b0` against the fix, interleaved (run 43396958829, EPYC 9V74 and 9V45)
**[measured]**: DynamoRIO alone 86 of 120 bad before, 0 of 120 after; under the client 49 of 120, 0 of 120. Stress set
unchanged (duckdb 0 of 240, signal 0 of 390, reg/flags 0 of 306 under the client and 0 of 306 alone, short 0 of 4,500),
speed unchanged; DynamoRIO's suite the same failures on base and fix (run 43396992185). Measured before the rebase onto `0bd5cd8`.

**A new trace freed under the thread that emitted it, fixed in the fork** (branch `fix-trace-emit-stale`,
[lhns/dynamorio#12](https://github.com/lhns/dynamorio/pull/12)). `end_and_emit_trace()` returned its trace after removing
the shared trace head, which can walk the pending-deletion list and sign this thread off a flush of the new trace; the trace
could be freed, and dispatch entered it, since its was-I-flushed check only runs when the thread is behind. The thread then
ran into the freed exit stubs (a DynamoRIO crash at a pc in its own heap, status 255, or a JVM abort). A lab-only ledger of
freed fragments found it (run 43397328455). It now looks the trace up again. `trace_flush` with
`-lazy_deletion_max_pending 1` on DynamoRIO alone, on top of the IBL fix (run 43404515993) **[measured]**: 120 of 120 bad
before, 0 of 120 after; Java under the client 14 of 16 and 0 of 16, javac 11 of 12 and 0 of 12. With both fixes against
`0bd5cd8`, interleaved, defaults (run 43437004196): status 255 in 9 of 1,872 runs before and 0 of 1,872 after.

**A signal at thread start, fixed in the fork** (branch `fix-sig-thread-start`,
[lhns/dynamorio#15](https://github.com/lhns/dynamorio/pull/15)). A signal sent to a thread right after its clone reached
DynamoRIO before the thread's signal state existed. DynamoRIO took the default action ("default action in native
thread", status 40), even for a signal the program handles. A thread-creating clone, except a vfork, now starts the child
with signals blocked until its signal state exists. `test/thread_start_sig.c` (a ctest), base `ebffb4a`, interleaved, six
jobs (runs 43554472071, 43557257607) **[measured]**: 600 of 600 bad before, 0 of 600 after, under the client and on
DynamoRIO alone. DynamoRIO's suite the same failures on base and fix (run 43557262702). `vfork_exit` under the client on
EPYC 7763 failed with "multiple native handlers" on base too (10 of 800; fix 19 of 800, run 43565816523): a signal in a
thread's own exit, fixed below.

**SIGXCPU raised by the kernel, fixed in the fork** (branch `fix-sigxcpu-delay`,
[lhns/dynamorio#17](https://github.com/lhns/dynamorio/pull/17)). DynamoRIO counted SIGXCPU among the signals it may not
always delay, as if it could be an instruction's fault; the kernel raises it only asynchronously (`RLIMIT_CPU`, a
descriptor's `F_SETSIG`). One raised by the kernel (si_code > 0, not `kill`) that arrived in DynamoRIO's own code aborted
the process as a DynamoRIO crash ("Received SIGother", status 255), with or without a client; one in the cache was
delivered at once. It is now always delayable. `test/kio.h` has the kernel raise SIGXCPU in a chosen thread through an
`O_ASYNC` pipe with `F_SETSIG`, thousands of times a second; `regstress kio` and `block_end kio` (an emulated instruction
at each block end) are ctests. Pin `75f24ff` against the fix, interleaved (run 43718354913, Xeon 6975P-C) **[measured]**:
regstress kio under the client 20 of 20 bad before, 0 of 20 after; block_end kio 10 of 10 and 0 of 10; on DynamoRIO alone
20 of 20 bad each before (run 43681301324), 0 of 40 each after; natively 0 of 40. Stress set unchanged (duckdb 0 of 240,
signal 0 of 1,300, reg/flags 0 of 1,020 under the client and 0 of 1,020 alone, short 0 of 4,500), `xl` 0 of 190 in each
mode. `regstress xlsig` now sends SIGSTKFLT, which DynamoRIO still may not always delay, so it keeps testing the hold of a
kill-sent signal.

**An emulated instruction that ended a block ran twice, fixed in the client** (branch `fix-emulated-block-end`).
DynamoRIO takes a block's fall-through from the last application instruction the client leaves in it **[source]**
(`client_process_bb`, "Client might have truncated"). The client removes what it emulates, so a block that ended on an
emulated instruction without a branch continued at that instruction. Blocks end that way after 256 instructions and, in
code DynamoRIO sandboxes (writable and executed: a JIT's), before the sixth store. C2's unrolled loop has
`vpaddd %ymm0,%ymm1,%ymm0` before each store: the wrong `arrays` value of Java's benchmark. The client now ends such a
block with an application jump to the next instruction. `test/block_end.c` (a ctest) fails on base and passes, inline and
helpers only, 14 of 14 jobs each (run 43637914298). Bench's `arrays` kernel with a safepoint every millisecond, 168 runs
each, base `d12001a` interleaved with the fix (run 43638957274) **[measured]**: with `-sandbox_writable` (every writable
region sandboxed) 168 wrong values on base, 0 with the fix; with DynamoRIO's defaults 16 on base, and 0 with the fix in
run 43637914298. On the lab branch at pin `509c12d` (run 43633075953, 140 each): 7 with defaults, 0 with
`-ro2sandbox_threshold 0` (never sandboxed), 140 with `-sandbox_writable`.

**A store that changed its own block skipped the emulated instruction after it, fixed in the client** (branch `seq-rwx`).
In a sandboxed block DynamoRIO checks after each store whether the block's code changed and, if so, leaves it for the
next application instruction left in it **[source]** (`insert_selfmod_sandbox`, `after_write`). The client removes what
it emulates, so that was the instruction after the emulated one. Each expansion now starts with an application `nop` at
the emulated instruction's address. `test/seq_rwx.c` (ctest, sandboxed: inline and helpers only) runs random sequences
of emulated and native instructions in RWX memory, across block ends and the sixth store, with stores into their own code
right before an emulated instruction, against native. With `-sandbox_writable`, seed 1, 300 sequences a run **[measured]**:
5 of 5 runs differ from native on base, 0 of 5 with the fix (run 43693416475, Xeon 6975P-C; also 5 of 5 and 0 of 5 in run
43681301324). Stress set unchanged (duckdb 0 of 240, signal 0 of 1,300, reg/flags 0 of 1,020 under the client and 0 of 1,020
alone, short 0 of 4,500) and speed unchanged (same run, EPYC 9V74: duckdb varied median 8.02 s against 8.01 s, Java 14.09
against 14.26 s, Node 3.77 against 3.73 s, every `bench_bmi` form within 0.03 ns).

CI runs the query 30 times under the client: against a 10% bad rate that catches it with 96%, against 2%, 45%.

**A write fault in a sandboxed fragment could not be translated, fixed in the fork** (branch `fix-selfmod-recreate`,
[lhns/dynamorio#18](https://github.com/lhns/dynamorio/pull/18)). With the default write-protection for code, a write
fault from inside a sandboxed fragment is translated by rebuilding the fragment from its saved copy. The rebuild resolved
rip-relative operands against the copy's address, so a store that had been emitted mangled through `rax` (target out of
reach) was rebuilt unmangled, the rebuilt list was shorter, and the walk overshot. Translation failed and the fallback in
`translate_sigcontext` used an already-overwritten `mcontext.pc`: NULL, so a release build resumed at pc 0 and faulted
there forever. This was the cause of the `seq_rwx` differences under DynamoRIO's defaults. The rebuild now decodes with
`decode_from_copy()` at the app pc and clears `rip_rel_valid`, and the fallback uses the sigcontext's cache pc; a write
fault whose context cannot be translated stops the process instead of resuming. Stock DynamoRIO 058821c has the same bug
(debug run 43846235758); per the owner it is fixed in the fork only. `seq-rwx-default` and `seq-rwx-helpers-default`
(ctests) run `seq_rwx` under DynamoRIO's defaults. Seed 1, 300 sequences, pin `f41973e` against main's pin
**[measured]**: 5 of 5 runs differ on main, inline and helpers only, 0 of 5 with the fix, DynamoRIO alone 0 of 20 (run
43895130650); interleaved, 10 of 10 against 0 of 10 inline, helpers only and seed 2, DynamoRIO alone 0 of 40 (run
43925199612). Fork suite against its base: no regression (run 43913425865; `tool.drcachesim.threads` 13 against 11 of 400
and `api.detach_signal` 1 against 2 of 400 are noise, run 43925013986). Stress set 0 bad (run 43925199612): duckdb
0 of 240, signal 0 of 1,300, reg/flags 0 of 1,020 per mode, short 0 of 4,500, xl 0 of 190 per mode, javac 0 of 8 per
setup.

**A signal in a thread's own exit, fixed in the fork** (branch `fix-exit-sigmask`,
[lhns/dynamorio#19](https://github.com/lhns/dynamorio/pull/19)). A thread ending with `exit` or `exit_group` blocks
signals for its exit (i#2921), but `dynamo_thread_exit_common()` restored the app's mask when the thread went native,
before its dcontext and TLS were gone **[source]**. A signal in that window took `execute_native_handler()`; once a vfork
child had run, which sets `multiple_handlers_present`, the process aborted with "multiple native handlers" (status 40 for
the process, so `vfork_exit` returns 1). In `vfork_exit`'s race mode the child's SIGCHLD lands there as the group's main
thread ends. This is stock DynamoRIO code; before the vfork-exit fix that exit hung instead. A thread in
`cleanup_and_terminate()` now keeps its mask; detach and going native still restore the app's. Evidence **[measured]**:
- A lab-only dump at the abort (run 43948017298, EPYC 7763): 215 of 215 the main thread in its own `exit_group`, the
  interrupted mask empty, no DR TLS, SIGCHLD from the vfork child.
- A lab-only 20 ms sleep at four points of the exit (run 43954599586, 320 runs an arm, race mode): after the thread's
  record is removed 164 of 320 runs bad on base, 0 of 320 with the fix; elsewhere at most 3 of 320 on base, 0 with the fix.
- `vfork_exit_dyn 20` under the client, interleaved, EPYC 7763 (run 43956311210): 82 of 3,000 bad on base `19fea7e`,
  0 of 3,000 with the fix.
- Race mode on DynamoRIO alone, about 890 runs an arm (run 43955001267): before the race fix (`0f97a362`) 5 bad, 4 of them
  this abort; base 5, all this abort; fix 0. Stock `058821c` and `b2a31c51` (before the vfork-exit fix) hang every run.
- With #16 merged, the cache lab's four arms (no cache, training, warm, verify) 0 of 2,800 each on EPYC 7763 (run
  43956493569); before, no cache 2.4-3.9% and training about 3.7 times that.
- Fork suite against its base: the same failures on both after reruns (run 43954664151).

It is lab-only (`lab/vfxab.sh`): on the Xeons most runners have, base fails about 0.05% of runs.

**A signal delivered at a safe spot, fixed in the fork** (branch `fix-safe-spot-signal`). A thread waiting in
`check_wait_at_safe_spot()` has `synch_perm` set; an async signal there is delivered at once (`record_pending_signal()`,
i#2659) and the thread never returns to the wait that resets it **[source]**. It ran on with a stale permission, and a
later fork's synch-all took it as at a safe spot wherever it was, e.g. exiting or mid-clone with its count raised. The
synch waited on that count until its loop limit (about 50 s), reported success, and the fork child inherited the count
and stalled in its own exit synch. The permission is now cleared before such a delivery, under the thread's
`synch_lock`; if the lock is busy or a synch has set the thread's context, the signal waits. Evidence **[measured]**:
- A lab-only event log per thread (fork branch `diag-fork-counters`): perm 6 set on entering the wait, SIGCHLD
  delivered, `dynamo_thread_exit_common`, no reset; the synch's held thread had `is_exiting=1`, `perm=6`.
- `sigchld_exit 300 fork`, 8 runners, interleaved (run 44090766308): DynamoRIO alone 106 of 114 bad on base `521016e`,
  0 of 114 with the fix; under the client 99 of 114 and 1 of 114 (one late reap). Base failures were late reaps, hangs,
  stuck children and SIGSEGVs. Before, on base alone (run 44049241668): native 0 of 110.
- `sigchld-exit-fork` is now a ctest.

**A fault in a flushed block, translated against a sandboxed rebuild, fixed in the fork** (branch
`fix-xl8-flushed-unsandboxed`, [lhns/dynamorio#21](https://github.com/lhns/dynamorio/pull/21)). A fragment pending
deletion has no stored translation, so a fault in it is translated by rebuilding the block from app code through
`check_thread_vm_area()` **[source]**. A write to its page flushes it, and after `-ro2sandbox_threshold` writes the page is
sandboxed. A block built before that was rebuilt with the selfmod checks it was emitted without **[source]**. The walk
missed and the fault's pc became 0. HotSpot's safepoint poll faulting there gave the JVM's status 134 aborts (`get_poll_stub`, "pc
must refer to an nmethod"). A rebuild of a block built without sandboxing, and of every block of a trace, now stays
unsandboxed. Evidence **[measured]**:
- The client's view of each SIGSEGV, lab only (run 44137849808): in 11 of 11 aborts (8 with defaults, 3 with
  `-disable_traces`) the last poll fault was in a block (not a trace) with `app_code_consistent` false, at the block's
  2nd instruction (no sandbox prologue), translated to pc 0.
- Bench's `arrays` kernel, a safepoint every millisecond, 400 runs an arm, interleaved, 20 runners (run 44141984403):
  8 aborts on base `af12842`, 0 with the fix, 0 with the fix and `-disable_traces`. On base alone (run 44132089263):
  9 of 400 with defaults, 8 of 400 with `-disable_traces`, 0 of 400 with `-ro2sandbox_threshold 0` or
  `-sandbox_writable`. The earlier "0 with `-disable_traces`" (140 runs) does not hold on `af12842`.
- A lab-only print on the fix (run 44142666373): a rebuild kept unsandboxed in 4 of 180 runs, about the base abort
  rate; no translation failed.
- `test/flushed_fault.c` (ctest `flushed-fault`) faults in a loop on a code page another thread writes. 5-second runs,
  Xeon 6975P-C (run 44142431509): DynamoRIO alone 60 of 60 bad on base, 0 of 60 with the fix; under the client 28 of
  60 and 0 of 60; natively 0 of 4.
- Stress set 0 bad (run 44143673801): duckdb 0 of 240, signal 0 of 1,300, reg/flags 0 of 1,020 under the client and 0 of
  1,020 alone, short 0 of 4,500; speed unchanged (Java median 14.0 s on both, duckdb varied 6.75 against 6.76 s).
- Fork suite against its base, release and debug (run 44142730409): the same failures on both after reruns. One-side
  failures, 20 reruns each, base against fix: `client.detach_test` 0 and 1, and 20 and 20 on another runner;
  `linux.signal_pre_syscall` 5 and 6; `tool.drcachesim.instr-only-trace` 4 and 4; `client.attach_test` 3 and 2.

**A vfork child SIGKILLed while DynamoRIO runs in it, made safe in the fork** (branch `fix-vfork-kill`,
[lhns/dynamorio#22](https://github.com/lhns/dynamorio/pull/22), merged as `6185c7a`: the tree of `4a1ec1a`, which the
runs below measured). A vfork child runs DynamoRIO's thread init and code in its parent's memory, and DynamoRIO's locks
do not survive their owner. A child killed in its init still held `thread_initexit_lock`, and its parent then waited for
it in `cleanup_after_vfork_child_death()`. A child killed after the lock but before its init finished left a half-built
thread, whose exit paths the parent ran (`trace_abort`, `instrument_thread_exit_event`, a heap free of NULL: status 255)
**[measured]**.
Now:
- The parent takes `thread_initexit_lock` for the child before the syscall. The child inits under it and gives it back, or
  the parent unlocks it if the child died first.
- A child killed before its init finished loses only its record; the rest is leaked.
- While a child runs, the locks taken on its dstack are counted. If it died holding one, the parent prints
  `<vfork child N was killed holding a DynamoRIO lock: ending the process>` and the process exits with status 89.
  Such a lock stays held and its data may be half updated, so no code can run safely, DynamoRIO's exit included.
- A vfork-like clone that fails, or a child killed before it copies the signal handlers, no longer leaves
  `num_unstarted_children` raised. Stock DynamoRIO raises it for a clone without `CLONE_SIGHAND` and only the child drops
  it, so the parent's next `sigaction()` and its thread exit spun on it forever **[measured]**. Parent and child now share
  a claim on it, as on the uninitialized-thread count.

What a dead child held, from a lab-only ledger of its locks (runs 44298025874, 44301006173) **[measured]**:
- Most children die before their thread setup and hold nothing.
- Of 109 children that died in their thread init with the fix, 55 held nothing else. The others held `global_alloc_lock`,
  `heap_unit_lock` or the vm-areas lock, in `heap_create_unit()` and the global heap allocations of the dcontext and the
  thread record. Removing those holds means building the child's thread state before the clone: not done.

`test/vfork_kill.c` is now the ctest `vfork-kill` (`test/vfork_kill.sh`); after the kills it also sets a handler. It
passes on PASS or the status-89 ending at 50 kills, and one of five 3-kill runs must PASS. Base `5693e56` against the fix
`4a1ec1a`, interleaved (run 44454291109) **[measured]**:
- 300 kills a run, 24 runs an arm, Xeon 6975P-C: at random points base fails 24 of 24 under the client (17 hangs,
  7 crashes in `common_heap_free()`, status 255) and 24 of 24 on DynamoRIO alone (hangs); at once 21 and 20 of 24. The fix
  hangs or crashes 0 of 96. It ends with status 89 in 23 and 24 of 24 at random points (client, DynamoRIO alone) and in
  11 and 12 at once; the rest PASS.
- 5 kills, 36 runs an arm, Xeon 8573C and 6975P-C: base fails 15 and 13 of 36 at random points (client, DynamoRIO
  alone), 4 and 8 at once. The fix fails 0 of 144, ending with status 89 in 5, 7, 2 and 1.
- The ctest's script, 20 runs each: base 16 of 20 fail, the fix 0 of 20.
- Natively 0 bad in each.

`test/vfork_fail.c` is the ctest `vfork-fail`: vfork-like syscalls that fail (`clone` with `CLONE_THREAD` and no
`CLONE_SIGHAND`, `clone3` with a bad exit signal, `vfork` at `RLIMIT_NPROC` 0), each followed by a thread and a vfork,
100 times. Base hangs in its first `sigaction()`: 7 of 7 under the client and 7 of 7 on DynamoRIO alone (runs 44449463519,
44452094924, 44454291109); `85bf692` too, 2 of 2 each. The fix: 0 of 105 under the client and 0 of 105 on DynamoRIO alone,
Xeon 6975P-C and EPYC 7763 (runs 44452094924, 44454291109). Natively 0 of 7 bad **[measured]**.

Speed: `bench/ab.sh 10`, base against the fix, interleaved, on Xeon 6975P-C and three EPYC 9V74 (run 44454291109).
Every workload's median is within 1% on each runner (`java -version` within 9%, either way), with outputs identical to
native: the lock counting costs nothing measurable **[measured]**.

No change elsewhere **[measured]**:
- `vfork_exit_dyn 20` under the client on EPYC 7763, base against `85bf692`: 0 and 0 of 2,500 (runs 44325044680,
  44356176489, 44373592856). `sigchld_exit` with fork children 1 and 0 of 255, with vfork children 0 and 0 of 255. Not
  rerun on `4a1ec1a`: no EPYC 7763 came up.
- The stress set on `4a1ec1a` (run 44454291109): duckdb 0 of 240, signal 0 of 1,300, reg/flags 0 of 1,020 under the
  client and 0 of 1,020 on DynamoRIO alone, short 0 of 4,500.
- The vfork, sigchld, clone3 and thread ctests all pass (47 of 47).
- Fork suite against its base, release and debug, two runners each (run 44452639132): the same failures on both after
  reruns. One-side failures over 20 reruns, base against fix: `client.attach_test` 2 and 6,
  `tool.drcachesim.windows-simple` 7 and 5, `tool.drcachesim.builtin-prefetch-basic-counts` 3 and 1,
  `tool.drcacheoff.synch_attach` 0 and 4 (base failed it in its first pass).
**Flushed blocks whose code changed, fixed in the client with a fork fix** (branch `fix-xl8-flushed-stored`, fork
branch `fix-xl8-stored-xl8`, [lhns/dynamorio#25](https://github.com/lhns/dynamorio/pull/25)). #21 left two cases where a fault in a flushed block is translated against a
rebuild that does not match the emitted code, and the handler sees pc 0 **[measured]**: the block's code was rewritten
after the flush (a JIT patching a method it retires), or its fall-through crosses into a page sandboxed since, where
the rebuild stops. The client now returns `DR_EMIT_STORE_TRANSLATIONS` for blocks outside module images, so their
translation is recorded at emit and never rebuilt from memory **[source]**. DynamoRIO's `-safe_translate_flushed`
(record at flush time) deadlocks: the rebuild inside `vm_area_unlink_fragments()` takes the shared vmareas write lock
it already holds (gdb, run 44450035455; debug build "deadlock on recursive mutex_lock", run 44449537159). Stored
translations had their own bug, fixed in the fork: `recreate_app_state_from_info()` marked the instruction after a
clean call as inside it and ignored its spill, so a rip-relative store mangled through `rax` right after a helper call
resumed with DynamoRIO's address in `rax`. Evidence **[measured]**:
- `test/flushed_xl8.c` (ctests `flushed-rewrite`, `flushed-span`): base 400 of 400 runs bad in each mode, the fix
  0 of 400; 20 jobs on EPYC 7763, 9V45, 9V74, Xeon 8573C, 6973P-C and 6975P-C (run 44521030426). DynamoRIO alone fails
  both too (run 44450035455): the fix is the client's.
- The walk bug: `seq_rwx` helpers only, DR defaults, seeds 1, 5, 7, with stored translations: 3 of 3 runs differ from
  native on each seed before the fork fix (run 44463418374), 0 after, inline and helpers only (run 44471420044). A
  lab-only comparison of every stored translation of a live block against a rebuild: 0 mismatches in 117,074 over the
  whole ctest with `-store_translations` (run 44471420044).
- No change elsewhere: Bench's `arrays` with a safepoint every ms 0 of 400 on both; `flushed-fault` 0 of 1,000 on both
  (run 44595406053); stress set 0 bad (duckdb 0 of 240, signal 0 of 1,300, reg/flags 0 of 1,020 under the client and
  0 of 1,020 alone, short 0 of 4,500; runs 44521030426 and 44574749511); fork suite the same failures on both after
  reruns (run 44518216908).
- Cost, Xeon 6975P-C, 6 reps interleaved (run 44521030426): Java 13.61 to 14.18 s, `java -version` 1.45 to 1.53 s,
  Node 3.54 to 3.60 s, duckdb compute 1.02 to 1.05 s, varied 6.65 to 6.67 s; peak RSS unchanged (run 44456279663).
- Not covered: module code rewritten in place keeps the rebuild.

**`close_range` over a huge range, fixed in the fork** (branch `fix-close-range-scan`,
[lhns/dynamorio#23](https://github.com/lhns/dynamorio/pull/23), pinned at its merge `2dad0c3`). DynamoRIO's pre-handler
looked up every fd of the range in its fd and signalfd tables **[source]**. CPython's `close_fds` (the `subprocess` default) ends the range at
`INT_MAX`, glibc's `closefrom()` at `~0U`, so one `subprocess.run(["true"])` took about a minute, the vfork child busy
in that loop while the parent waited in `vfork`. It now visits only DynamoRIO's own fds and the std fds. Evidence
**[measured]**:
- Python 3.12, Xeon 8573C (run 44505587469): `subprocess.run(["true"])` natively 0.01 s, DynamoRIO alone 66.9 s, under
  the client 66.96 s; `close_fds=False`, `os.posix_spawn`, `os.fork` and C `posix_spawn` or `vfork` 0.02-0.11 s. strace:
  67 s between the child's `close_range(3, 3)` and its `close_range(5, 2147483647)`, the child running in between.
- Main's pin `5693e56` against the fix, interleaved, 3 reps, EPYC 7763 (run 44507982244): 37.7-41.1 s before,
  0.11-0.15 s after, under the client and alone. `test/close_range.c` (ctest `close-range`) past 90 s in 6 of 6 before,
  0.05-0.07 s after.
- Fork suite against its base `6185c7a`, release and debug (run 44507970821): the same failures on both after reruns
  but `client.detach_test` on one debug runner, 20 of 20 failing on both when repeated.
- Stress set (run 44507982244): duckdb 0 of 240, signal 0 of 1,300, reg/flags 0 of 1,020 under the client and 0 of 1,020
  alone, short 0 of 4,500 (Xeon 6975P-C, EPYC 9V74 and 7763).
The numbers above were measured on `3ae7f84`. The PR head `c047585` added an errno sign fix and a 32-bit range fix
on top: v3compat ctests 303 of 303 on it (lab run 44538367127).

**Closed by the fork's signal fixes**, rerun on pin `af12842` (run 44109231119 and 44111380142) **[measured]**:
- A signal to a thread in its first instructions (`test/thread_start_sig.c`) killed the process 300 of 300 under DynamoRIO, with or
  without the client (run 43418236984): now 0 of 600 in each of native, DynamoRIO alone and the client. Consistent with
  lhns/dynamorio#15, which starts a thread with signals blocked.
- javac crashed in about 2% of runs in every configuration, DynamoRIO alone too (run 43331210295; `lab/javacdiag.sh` is only in that run's lab branch, not on main):
  now 0 of 200 each under the client, an empty client and DynamoRIO alone.

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
| **1. Correct, not yet fast** | `lzcnt` in a `dlopen`ed library, static children, duckdb output correct; compute excluding startup under 3 s | Met |
| **2. Fast** | Dense cost under 5 ns per op; compute under 1.5 s; signal injection clean | In progress: inline scalar (#33), vector (#35) and cross-lane (#40) forms and the lean helper call (#39) merged |
| **3. Startup** | Warm `select 42` under 0.5 s; memory under 180 MB; bad entries rejected | Cache built through its Phase 2, lab and test only: warm `select 42` 0.34-0.63 s on runners, 39-43% below cold ([design](translation-cache.md)) |
| **4. Production** | Wrapper integration, passthrough, canary; two weeks with no divergence from native | Not started |

## Known flakes

Failures seen that are not regressions; a PR may cite them, with the counts, instead of fixing them.

- **`client.attach_test`** in DynamoRIO's own suite fails intermittently on base and fix alike: rerun it (the suite
  workflow reruns one-sided failures 20 times).
- **`tool.drcachesim.coherence` and `api.detach_signal`** (release) flake on any commit: 11 and 4 of 240 on `521016e`,
  7 and 3 of 240 on the safe-spot fix, interleaved on four runners (run 44100838667). 20 reruns can show 2 against 0.
- **`api.static_prepop`** fails in its second attach on AVX-512 runners (static re-attach loses AVX-512 state, "Won't
  fix"). v3compat does not use static attach; ignore it on AVX-512 runners, compare base and fix.
- **DynamoRIO's debug suite on EPYC 9V45**: debug builds print `<Unknown processor cache size>` and fail most tests on
  any commit. Use release there, or another runner, for the debug suite.
- **`client.memory_dump_test`** (debug and release) fails or passes by the build directory's name: the dump's section-name
  hash is keyed on a buffer the maps iterator reuses, so `[vvar]` can take an earlier region's name. Compare base and
  fix built in the same directory name.

## Open items

- **Whole-process time.** 5.9-7.4 s for the compute query on Ivy Bridge until Phase 3. The mixed workload is about 11x on
  AMD (1.67x with no client) and needs more forms inline.
- **A vfork child SIGKILLed holding a DynamoRIO lock ends the process** with status 89 instead of letting it run on
  (fork entry above). It no longer hangs or crashes. Under the client most runs of `vfork_kill` with 300 kills at random
  points end this way (23 of 24); at 5 kills 5 of 36 **[measured]**. The children die in the heap and vm-areas locks of their thread
  init and of building blocks. Letting the parent run on needs the child's thread state built before the clone, or robust
  locks. Not counted, so not caught: a child killed in its signal handler, or in DynamoRIO's exit path.
- **One `regstress sig jit` SIGSEGV** under the client in CI on the rebased fork (0.28 s in, no core), not reproduced:
  0 of 1,000 on it and 0 of 1,000 on `8029afd`, on EPYC 7763, 9V45, 9V74 and Xeon 8573C, 6973P **[measured]**.
- **The fork's** signal, lock and exit changes are ours to maintain. Known limits: a signal can be delayed past a syscall in
  a shared fragment; the nudge relink is unchanged; a thread can hold a stale pointer to a flushed fragment. The lock change
  affects every thread running during exit and is the one most worth review.
- **Order of work** (owner, 2026-11-12): the javac/Java crash, then javac under DynamoRIO alone (javac no longer
  crashes, above; Java's status 134 aborts are fixed, above), then the ranked performance list; the OCI wrapper after that. JVM workloads are in scope.
- **A `flushed-fault` crash with stored translations, not attributed:** 2 of about 2,600 runs on the fix crashed in
  `common_heap_alloc()` popping a free list (same pc both times; EPYC 9V45 and 9V74, run 44521030426), 0 of about 2,500
  on base. A rerun of 1,000 a side found none on either (run 44595406053), nor did 60 on a debug build (run 44591850156).
- **`seq_rwx` seed 10 refuses on base** ("write to code whose context cannot be translated", 3 of 3, inline and helpers
  only, run 44463418374): the rebuild of the written block recovers the pc but not the state. With the stored
  translation the digest is right. Reproducer: `sh test/seq_rwx.sh build/inst/bin/v3compat-run build/test/seq_rwx 10 300`.
- **A debug-build assert, `vmareas.c:8965` (`area_contains_frag_pc`)** on a block that falls through into a page the app
  writes (`flushed_xl8 span-below`), on base's client too (run 44473283052). Release builds do not check it.
- **`-safe_translate_flushed` deadlocks** in every flush of a written page (above); do not use it.
- **A crash under `-sandbox_writable`:** Bench's `arrays` kernel with frequent safepoints crashed 1 of 400 runs with
  status 255 ("v3compat internal crash", a SIGSEGV DynamoRIO attributes to the client, Xeon 6975P-C, run 44132089263). Not investigated.
- **Performance, ranked:** DynamoRIO's gencode `mprotect` toggling: `-protect_mask 0` cuts javac's `mprotect`s 60%
  but its wall only 1-2% (lab `lab-perf-protect-off`, run 43398713863; opt-in in lhns/v3compat#46); FMA through hardware arithmetic with an exactness guard, if
  a workload needs it. After #40 about 2M helper calls a run remain in duckdb varied, Node and javac (counted in the
  helpers, lab `lab-perf-gap`); Java's `Math.fma` is the one dense user. Runner numbers
  understate helper cost 2-4x against Ivy Bridge; an Ivy
  Bridge A/B can run on the cluster's `kube-worker-04` and is not required before a speed PR merges.

## Later

On the roadmap, not being worked on.

- **Cache reset, fixed so it can be turned on** (owner, 2026-11-18). It stays off for now
  ([ADR 0010](adr/0010-no-cache-reset.md)). Known: flags lost in the relocation window (upstream
  [i#400](https://github.com/DynamoRIO/dynamorio/issues/400)), crashes under the client with forced resets (likely upstream
  [i#4219](https://github.com/DynamoRIO/dynamorio/issues/4219), not tested), hangs under reset with code churn. Anything new
  that relocates a thread needs `test/regstress.c`.

## Won't fix

Owner's decision, 2026-11-18: these do not affect v3compat's configuration. The evidence stays here.

- **DynamoRIO alone, threads churning at exit** (no client, 1,000 threads) stalls the exit for a minute or more. v3compat
  always has a client, which synchs first. 21 of 45 runs past 120 s on three EPYC 7763s; with `-synch_at_exit` 0 of 45 past
  6.7 s **[measured]**. With no exit event DynamoRIO marks itself exited before it synchs, so new threads spin in thread
  init on `thread_initexit_lock` against the exit's synch and about 1,000 exiting threads **[measured]**. Still there on
  pin `af12842` (run 44109231119): 17 of 75 past 130 s, the rest 2.1-2.9 s, on Xeon 6975P-C and EPYC 9V74 **[measured]**.
- **DynamoRIO static re-attach loses AVX-512 state;** v3compat does not use static attach. `api.static_prepop` fails in
  its second attach on AVX-512 CPUs only (glibc's EVEX `strchrnul` returns a wrong position); 0 of 10 with glibc's AVX-512
  routines disabled. Base fails too (debug 20 of 20, release 0-5 of 20); the torn-exit fix's layout makes release fail
  every time.
- **DynamoRIO's debug suite on EPYC 9V45:** debug builds print `<Unknown processor cache size>` and fail 460 of 520 tests
  on base and fix alike. Use release there ("Known flakes").

## License

Apache-2.0, chosen for its patent grant. DynamoRIO core, `drmgr`, `drreg`, `drx` are BSD; `drutil`, `drwrap`, `drsyscall`
are LGPL 2.1 and ship as separate shared libraries, never statically linked; AVXEmu is CC0 **[source]**. Binaries must carry
DynamoRIO's notices.

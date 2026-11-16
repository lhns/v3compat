# v3compat

Run x86-64-v3 programs (AVX2, FMA, BMI, LZCNT) on x86-64 CPUs with AVX but without AVX2.

## State

Emulation runs: a same-ISA binary translator on [DynamoRIO](https://github.com/DynamoRIO/dynamorio) (our fork)
copies instructions the CPU has and expands or calls helpers for the rest. Details and numbers: the [plan](docs/plan.md).

**Works** (measured, see the plan for conditions):
- duckdb 2.0.0 gives the same output as native on Ivy Bridge, slowly: `select 42` 5.1-5.4 s, a compute query 5.9-7.4 s,
  the mixed `varied.sql` 165 s.
- `lzcnt` in a `dlopen`ed library, static, static-PIE and musl children, `exec` chains, threads and `posix_spawn`,
  under Docker's default seccomp without ptrace.
- Refusal instead of a wrong result: a form off the [audited list](docs/allowlist.md) exits with status 86;
  setuid, setgid and file-capability programs are refused at exec.

**Limits and known problems:**
- The shipped config runs with DynamoRIO's cache reset off (`-no_enable_reset`, [ADR 0010](docs/adr/0010-no-cache-reset.md)).
  The stress set on AVX2 runners at the current pin: duckdb 0 of 240 bad, signal stress 0 of 390, register and flags
  stress (`test/regstress.c`) 0 of 306 under the client and 0 of 306 on DynamoRIO alone, the short loop 0 of 4,500.
  Zero failures bound the rate, they do not prove it zero: by the rule of three, at most 1.3%, 0.8%, 1.0% and 0.07% (95%).
- Every system call leaves the code cache (`-no_ignore_syscalls`, [ADR 0011](docs/adr/0011-no-ignored-syscalls.md)), so code a JIT unmaps is freed while other threads wait.
- A `vfork` child killed with SIGKILL before it execs can hang or crash its parent (`docs/plan.md`, open items).
- Startup is DynamoRIO translation, about 5 s for duckdb on Ivy Bridge; no cache yet.
- Java and javac run under the client since the fork fixed a crash in trace building that any client hit
  ([lhns/dynamorio#8](https://github.com/lhns/dynamorio/pull/8); upstream has it). javac still fails in about 1 run in 5,
  on DynamoRIO alone too (plan, "Open items"). JVM
  workloads are in scope; see "Open items" in the plan.

**Untested or unverified:**
- Ivy Bridge since the last fix: the reset and stress results above come from AVX2 runners, not Ivy Bridge. The earlier
  Ivy Bridge runs predate it.
- Anything on a CPU other than Ivy Bridge without AVX2.
- The OCI wrapper in [v3compat-runtime](https://github.com/lhns/v3compat-runtime) still wraps Intel SDE, not `v3compat-run`; the integration is not done. No production use.

## Build and run

Linux x86-64 only. Needs cmake 3.18+, git, patch, zlib headers and C and C++ compilers. Our DynamoRIO fork is built from its pinned commit at
configure time (cached in `~/.cache/v3compat`); `-DV3COMPAT_DR_ROOT=<dir>` uses an existing install instead.

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build                       # runs against build/inst, the install layout
cmake --build build --target package-tarball # v3compat-<version>-linux-x86_64.tar.gz
build/inst/bin/v3compat-run [--] prog args...
```

- `V3COMPAT_NATIVE=1` runs `prog` without DynamoRIO. Without it the program always runs under DynamoRIO, even
  on a CPU that has AVX2.
- `V3COMPAT_DR_OPTS="..."` adds DynamoRIO options; defaults live in `launcher/v3compat-run.c`.
- `V3COMPAT_UNPROTECTED_GENCODE=1` passes `-protect_mask 0`: DynamoRIO stops making its generated code (indirect-branch
  lookup, context switch) writable around each change. javac does 60% fewer `mprotect`s and runs 1-2% faster on CI runners;
  Node and java -version gain little, duckdb nothing. The code then stays writable, so a stray write by the program corrupts
  DynamoRIO instead of faulting. Off by default.
- `V3COMPAT_STATS=1` prints, at exit to stderr, how many instructions of each class (AVX2, FMA, BMI, LZCNT, TZCNT,
  MOVBE) and of each emulated opcode were executed. It slows the program: never time a run with it.
- Startup is DynamoRIO translating the program's static init (duckdb `select 42`: about 20 times native). Measured on
  CI runners: `V3COMPAT_DR_OPTS=-disable_traces` cuts it by about 27% (compute query 18%) but costs about 3% wall and
  6% CPU on a long mixed workload, so it is not the default; use it for short-lived programs. `-trace_threshold` does
  not help (10 to 100 the same, higher is slower). The allowlist check is about 5% of startup and memoising it gained
  nothing. `-persist` does not work with the client: the client's `libdrmgr.so` fails to load
  (`undefined symbol __memcpy_chk`), and a persisted cache would need a client that registers persistence callbacks.
- The exit status and signals are the program's own. A program that would change privileges when exec'd (setuid, setgid, file capabilities) is refused: its `execve` fails with `EPERM` and a message on stderr, or the launcher exits 87 if it was the program named on the command line (see the plan).

### Semantic suite (opt-in)

`cmake -S . -B build -DV3COMPAT_SEMANTIC=ON` adds `semantic-hw`: it fetches a pinned
[avxemu](https://github.com/lhns/avxemu) commit and runs its instruction-form cases under the client against
the digests recorded on AVX2 hardware, also with inline expansions off. `test/sem/refused.txt` lists the forms still
refused, and has to be exact. A program that executes a form off the [audited list](docs/allowlist.md) exits with status 86; `V3COMPAT_AUDIT=1` lists such forms instead.

- [v3compat-runtime](https://github.com/lhns/v3compat-runtime): OCI runtime wrapper, opt-in per pod through a RuntimeClass.
- [avxemu](https://github.com/lhns/avxemu): fork supplying the instruction emulation helpers and test suite.

See the [plan](docs/plan.md) and [how to](docs/howto.md) build, test, run labs and change the fork.
See the [decisions](docs/adr/README.md).

Licensed under [Apache-2.0](LICENSE).

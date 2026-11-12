# How to

## Build and test

Linux x86-64 on a CPU with AVX2 for the full suite (the client emulates on any CPU, but the tests compare
against native). The [README](../README.md) has the commands. In short:

```sh
cmake -S . -B build -DV3COMPAT_SEMANTIC=ON && cmake --build build -j
V3COMPAT_CI=1 ctest --test-dir build --output-on-failure
```

- `-DV3COMPAT_SEMANTIC=ON` adds `semantic-hw` (every form against digests recorded on AVX2 silicon, inline and
  helpers only). CI turns it on; leave it on for any change to `src/expand.c`.
- The first configure builds the pinned DynamoRIO fork into `~/.cache/v3compat/dr-<commit>` (about 10 minutes);
  `-DV3COMPAT_DR_ROOT=<install>` uses an existing build instead. avxemu's helpers (and, with the semantic suite, its test
  cases) are fetched by CMake at the commit pinned in `CMakeLists.txt` (`AVXEMU_COMMIT`).
- `V3COMPAT_HELPERS_ONLY=1` sends every emulated form to its helper, except `movbe` stores, which are always inline.
- `allowlist-check` fails after a DynamoRIO or XED bump until `test/allow/regen.sh` is rerun ([ADR 0008](adr/0008-audited-list-keyed-on-shape.md)).
- `test/sem/refused.txt` lists the suite's forms the client refuses (exit 86). It is empty and must stay exact:
  `semantic-hw` fails when a form is refused but not listed, or listed but no longer refused.
- Prerequisites some tests need: docker (`test/docker.sh`), `musl-tools` (the musl children), root or `sudo -n` and a
  build tree that uid 65534 can reach (`chmod o+x $HOME`; `test/setuid.sh` runs checks as that user). Without them
  those tests skip (status 77); `V3COMPAT_CI=1` turns each missing prerequisite into a failure, as in CI.
- CI (`.github/workflows/ci.yml`) runs all of it on every PR, plus duckdb's hashes against native and a BMI
  microbenchmark.

## Run a lab

A lab is a throwaway branch named `lab-<topic>` (`lab-perf-*` for performance work). Pushing it runs
`.github/workflows/lab.yml`: one job per line of `lab/parts`, each on its own GitHub runner.

- Each job builds the branch into `build/`. A part whose name starts with `ab` also builds `origin/main` into
  `/tmp/base/build`. Every job compiles `bench/` into `/tmp/bench` and fetches the pinned duckdb.
- Lines starting with `RESULT` (and the CPU model) show in the job log; everything is in the artifact
  `lab-<part>` (`lab-out/log.txt`). `gh run download <id> -R lhns/v3compat` fetches them.
- Edit `lab/parts` on the lab branch for what you need; change scripts there too. Keep the branch only while it is
  evidence for a PR, then delete it.
- Artifacts expire. Durable numbers go into the PR body and, when they change a claim, into [plan.md](plan.md).

What is there:

| Script | Does |
|---|---|
| `bench/ab.sh REPS` | Base against the branch, interleaved per rep: `bench_bmi` ns/op, duckdb compute and varied, `java -version`, Java `bench/Bench.java`, Node `bench/jit.js`; output digests against native |
| `bench/jit.js`, `bench/Bench.java` | JIT workloads that print per-round times (warmup apart from steady state) and a checksum |
| `bench/fma.c` | ns/op of each FMA form (vpermd as the helper-call control) and dgemm/sgemm/Horner kernels with checksums |
| `lab/stress.sh KIND RUNS` | The stress set (`duck`, `sig`, `regc`, `regd`, `short`) and process-level labs: `exit` (a process ending while a thread waits in `vfork`), `cap` (thread creation failing or churning at exit), `bigclient` (1,000 threads at exit), `xl` (signals sent with `kill` at emulated instructions), `segv` (the torn exit, 9V45 only); `lab/loop.sh` classifies each run |
| `lab/javac.sh RUNS` | The javac crash: javac under the client, an empty client (`lab/nullclient`) and DynamoRIO alone, with each crash's offset |

The stress set every PR that touches translation or the fork reports, in the plan's terms: duckdb 0 of 240, signal
stress 0 of 390, reg/flags stress 0 of 306 under the client and 0 of 306 on DynamoRIO alone, the short loop 0 of
4,500. The default `lab/parts` is that set plus one `ab` job.

Rules of thumb: the client always emulates, even on an AVX2 runner, so runners measure emulation cost (helpers are
2-4x cheaper there than on Ivy Bridge). Use `V3COMPAT_HELPERS_ONLY=1` to compare inline against helpers,
`V3COMPAT_STATS=1` (untimed) for executed counts per opcode, `V3COMPAT_AUDIT=1` for forms off the list. For a
profile, `perf record -e cpu-clock` works on runners after `sudo sysctl kernel.perf_event_paranoid=-1`; add DynamoRIO's
symbols with `perf buildid-cache -a ~/.cache/v3compat/dr-<commit>/lib64/release/libdynamorio.so.debug`.

### A crash inside DynamoRIO

To build the fork by itself: `cmake -S <fork> -B <dir> -DDEBUG=OFF -DBUILD_TESTS=OFF -DBUILD_DOCS=OFF` (as
`cmake/FetchDynamoRIO.cmake` does; `-DDEBUG=ON -DINTERNAL=ON` for a debug build). `bin64/drrun <DR options> -c client.so
-- app` runs it with any client; an empty client is a useful control (any client changes how DynamoRIO builds
blocks).


The report gives the PC and `Base:`; `pc - base` plus 0x71000000 (the library's link address) is the address in
`libdynamorio.so.debug`: `addr2line -f -i -e <debug file> <addr>`. For a backtrace, run with
`-dumpcore_mask 0x1009` in `V3COMPAT_DR_OPTS`, wait for "Waiting for debugger" on stderr, attach `gdb -p <pid>`,
`add-symbol-file <debug file> -o <base - 0x71000000>`, select the crashing thread and set `$rip`, `$rsp`, `$rbp` and
the other registers from the report before `bt`. A debug build of the fork (`-DDEBUG=ON` in
`cmake/FetchDynamoRIO.cmake`, with its own `-DV3COMPAT_DR_CACHE`) turns the release crash into the first failed assert.

## Change the fork

The fork is [lhns/dynamorio](https://github.com/lhns/dynamorio), branch `rebase-master-20261110`: upstream master
plus our fixes as flat commits ([ADR 0003](adr/0003-dynamorio-as-the-base.md)). The plan's "The cache reset" section
lists each fix and its evidence.

1. Branch from `rebase-master-20261110`, make the change with a test where one fits (a v3compat ctest is usually the
   reproducer, as `test/vfork_exit_load.sh` was).
2. Lab it before review: on a v3compat `lab-*` branch, set `V3COMPAT_DR_COMMIT` in `cmake/FetchDynamoRIO.cmake` to the
   fork branch's commit (the cache is keyed on that file). Compare base and fix on the same runner; for a fix, show
   the failure before and none after over many runs.
3. DynamoRIO's own test suite, base and fix on the same runner, when the change is in core: push a v3compat branch
   `suite-<topic>` that sets `BASE` and `FIX` in `.github/workflows/dr-suite.yml`. It builds both with
   `-DBUILD_TESTS=ON`, release and debug, runs `ctest` twice and reruns each test that fails on one side only 20
   times. The bar is the same failures on both apart from the [known flakes](plan.md#known-flakes).
4. `gh pr create -R lhns/dynamorio --base rebase-master-20261110 --head <branch>`.
   The fork PR is merged by the owner, on the same rule as here.
5. A v3compat PR moving `V3COMPAT_DR_COMMIT` to the head of `rebase-master-20261110` once the fork PR is merged (a
   commit on that branch, not the PR branch's), with the lab evidence and an entry in the plan. The pin moves only after
   the stress set and the lab match the previous pin.

Older fork branches (`fix-*`, earlier bases) stay as they are unless the owner asks to delete them.

## Glossary

- **Runners:** GitHub's `ubuntu-24.04` machines, assigned at random: AMD EPYC 7763 (Zen 3), 9V74 (Zen 4), 9V45 (Zen 5;
  the only one that showed the torn exit), Intel Xeon 8370C, 8573C. All have AVX2; some have AVX-512, which `cpuid` under
  the client hides.
- **Torn exit:** a thread jumping through a fragment's exit while another thread re-links it fetched half the old and half
  the new jump offset, on 9V45 only (fixed in the fork). Runner models are assigned at random and `segv` skips anything
  but 9V45: run it as several jobs (one line each in `lab/parts`) and expect a few to land there.
- **PIE, static-PIE:** position-independent executables; `spie` in test names is a static-PIE build.
- **Stress set:** `test/regstress.c` runs threads in hot shared code with every register and the flags checked.
  `regc` under the client and `regd` on DynamoRIO alone (modes `jit`: code generated and unmapped, `sig`: async
  signals, `idle`), `sig` (8 threads, signals and code churn), `short` (many 1-second runs, for start and exit
  races), `xlsig` (SIGXCPU sent with `kill` at emulated instructions), `duck` (duckdb's mixed query against native
  hashes). The torn exit lab (`segv`) is `regstress jit` on 9V45 only.
- **DynamoRIO:** a *fragment* is translated code in the code cache: a *basic block* (bb), or a *trace* stitched from hot
  bbs. A *flush* removes fragments for a code range (a JIT writing code, an `munmap`); they are freed once every thread
  has passed a *synch point* (a safe spot outside the cache). A *cache reset* throws away the whole cache and relocates
  every thread, which we keep off ([ADR 0010](adr/0010-no-cache-reset.md)). A *clean call* saves the whole machine state
  to call C from the cache; our helpers use it. A *frame helper* gets its operands through raw TLS and saves no
  application state beyond what it clobbers; an *mcontext helper* reads and writes the full saved machine context
  (slower, for forms that write flags or several registers). *Linking* patches a fragment's exit to jump straight to
  the next fragment; *unlinking* sends it back through DynamoRIO (done before a flush, or to get a thread's attention).
  A *nudge* is DynamoRIO's request to a thread delivered by signal; the *nudge relink* is how the thread's fragment is
  linked again afterwards.
- **Counting:** 0 failures in n runs bounds the rate at 3/n (95%, the rule of three); the plan quotes that bound.
- **Phases:** each has a go/no-go in the plan; a failed gate stops the plan for a decision.

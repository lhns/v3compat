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
- The first configure builds the pinned DynamoRIO fork into `~/.cache/v3compat/dr-<commit>` (about 10 minutes).
- `allowlist-check` fails after a DynamoRIO or XED bump until `test/allow/regen.sh` is rerun ([ADR 0008](adr/0008-audited-list-keyed-on-shape.md)).
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
| `lab/stress.sh KIND RUNS` | The stress set (`duck`, `sig`, `regc`, `regd`, `short`) and process-level labs (`exit`, `cap`, `xl`, `segv` on 9V45); `lab/loop.sh` classifies each run |

The stress set every PR that touches translation or the fork reports, in the plan's terms: duckdb 0 of 240, signal
stress 0 of 390, reg/flags stress 0 of 306 under the client and 0 of 306 on DynamoRIO alone, the short loop 0 of
4,500. The default `lab/parts` is that set plus one `ab` job.

Rules of thumb: the client always emulates, even on an AVX2 runner, so runners measure emulation cost (helpers are
2-4x cheaper there than on Ivy Bridge). Use `V3COMPAT_HELPERS_ONLY=1` to compare inline against helpers,
`V3COMPAT_STATS=1` (untimed) for executed counts per opcode, `V3COMPAT_AUDIT=1` for forms off the list. For a
profile, `perf record -e cpu-clock` works on runners after `sudo sysctl kernel.perf_event_paranoid=-1`; add DynamoRIO's
symbols with `perf buildid-cache -a ~/.cache/v3compat/dr-<commit>/lib64/release/libdynamorio.so.debug`.

### A crash inside DynamoRIO

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
3. DynamoRIO's own suite, base and fix on the same runner, when the change is in core: a `suite-*` branch carrying a
   workflow that builds both with tests and runs `ctest` (none is kept on main).
4. `gh pr create -R lhns/dynamorio --base rebase-master-20261110 --head <branch>`.
5. A v3compat PR moving `V3COMPAT_DR_COMMIT` to the fork's merge commit, with the lab evidence and an entry in the
   plan. The pin moves only after the stress set and the lab match the previous pin.

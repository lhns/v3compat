# v3compat

Run x86-64-v3 programs (AVX2, FMA, BMI, LZCNT) on x86-64 CPUs with AVX but without AVX2.

**Status: scaffold. It emulates nothing yet.** A program that uses an instruction the CPU lacks still gets
SIGILL under it. The plan is a same-ISA dynamic binary translator: code runs natively except for the
instructions the CPU lacks. Whether it is fast enough is being measured.

## Build and run

Linux x86-64 only. Needs cmake 3.18+ and a C compiler. DynamoRIO `release_11.3.0-1` is downloaded at configure
time (sha256 checked, cached in `~/.cache/v3compat`); `-DV3COMPAT_DR_ROOT=<dir>` uses an extracted copy instead.

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build                       # runs against build/inst, the install layout
cmake --build build --target package-tarball # v3compat-<version>-linux-x86_64.tar.gz
build/inst/bin/v3compat-run [--] prog args...
```

- `V3COMPAT_NATIVE=1` runs `prog` without DynamoRIO. Without it the program always runs under DynamoRIO, even
  on a CPU that has AVX2.
- `V3COMPAT_DR_OPTS="..."` adds DynamoRIO options; defaults live in `launcher/v3compat-run`.
- `V3COMPAT_STATS=1` prints, at exit to stderr, how many distinct instructions of each class (AVX2, FMA, BMI,
  LZCNT, TZCNT, MOVBE) were translated. It counts translations, not executions.
- The exit status and signals are the program's own. `setuid` programs lose their privileges (see the plan).

### Semantic suite (opt-in)

`cmake -S . -B build -DV3COMPAT_SEMANTIC=ON` adds `semantic-hw`: it fetches a pinned
[avxemu](https://github.com/lhns/avxemu) commit and runs its instruction-form cases under the client against
the digests recorded on AVX2 hardware. On AVX2 it must pass. On an AVX-only CPU it fails with
`EMULATION MISSING` (exit 3) at the first instruction nothing emulates, which is distinct from a result mismatch
(exit 1). `test/sem/known-dr.txt` lists forms where plain DynamoRIO 11.3.0 itself differs from native
(OF is lost around `movbe` and `vbroadcasti128` loads); they are tolerated under the client only.

- [v3compat-runtime](https://github.com/lhns/v3compat-runtime): OCI runtime wrapper, opt-in per pod through a RuntimeClass.
- [avxemu](https://github.com/lhns/avxemu): fork supplying the instruction emulation helpers and test suite.

See the [plan](docs/plan.md).
See the [decisions](docs/adr/README.md).

Licensed under [Apache-2.0](LICENSE).

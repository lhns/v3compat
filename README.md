# v3compat

Run x86-64-v3 programs (AVX2, FMA, BMI, LZCNT) on x86-64 CPUs with AVX but without AVX2.

**Status: design phase, nothing to use yet.** The plan is a same-ISA dynamic binary translator: code runs
natively except for the instructions the CPU lacks. Whether it is fast enough is being measured.

- [v3compat-runtime](https://github.com/lhns/v3compat-runtime): OCI runtime wrapper, opt-in per pod through a RuntimeClass.
- [avxemu](https://github.com/lhns/avxemu): fork supplying the instruction emulation helpers and test suite.

See the [plan](docs/plan.md).
See the [decisions](docs/adr/README.md).

Licensed under [Apache-2.0](LICENSE).

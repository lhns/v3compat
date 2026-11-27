# Changelog

## 0.1.0

First release. The version comes from the tag (`v0.1.0`, passed to CMake as `V3COMPAT_VERSION`); it is not stored in the
tree. Built by `.github/workflows/release.yml`: `v3compat-0.1.0-linux-x86_64.tar.gz` and its `.sha256`, a bundle that
needs nothing outside itself.

### Runs

- x86-64-v3 programs (AVX2, FMA, BMI, LZCNT, TZCNT, MOVBE) on x86-64 CPUs with AVX but without AVX2, through
  `bin/v3compat-run [--] prog args...`: a same-ISA binary translator on our DynamoRIO fork.
- duckdb, Node, Java and javac, `lzcnt` in `dlopen`ed libraries, static, static-PIE and musl children, `exec` chains,
  threads and `posix_spawn`, under Docker's default seccomp without ptrace. Output matches native on Ivy Bridge, slowly:
  `select 42` 5.1-5.4 s, about 20x native startup.
- DynamoRIO traces are off by default (the launcher passes `-disable_traces`; owner, 2026-11-27): on Ivy Bridge javac and
  java used 27-33% less CPU. `V3COMPAT_DR_OPTS=-enable_traces` turns them back on.
- Fixed in the fork since the crash reports: the `common_heap_alloc()` heap crash (#29), the decode/emit race behind
  the status 255 and 134 crashes (crash 2, #81).

### Refuses

- An instruction form off the audited list ([docs/allowlist.md](docs/allowlist.md)): exit 86.
- setuid, setgid and file-capability programs: `execve` fails with `EPERM`; exit 87 if it was the program named on the
  command line.

### Exit statuses

40 DynamoRIO cannot handle a signal, 86 refused instruction, 87 launcher refused the program, 89 a vfork child was
killed holding a DynamoRIO lock, 125 launcher failure, 126 and 127 as in a shell. Other statuses and signals are the
program's.

### Known production limits

- A `vfork` child killed with SIGKILL while holding a DynamoRIO lock ends the process with status 89 instead of
  letting it run on.
- `test/seq_rwx.sh` seed 10 refuses ("write to code whose context cannot be translated"); it fails closed.
- A debug-build assert, `vmareas.c:8965` (`area_contains_frag_pc`); release builds do not check it.
- The translation cache is lab and test only: the launcher reads `V3COMPAT_CACHE*` for tests and labs, the runtime has a
  read-only mount for it, and the canary does not use it (owner, 2026-11-27). Startup stays DynamoRIO translation.
- Cache reset is off (ADR 0010); every system call leaves the code cache (ADR 0011).
- No production use yet; the OCI wrapper is lhns/v3compat-runtime.

### Pins

- DynamoRIO fork lhns/dynamorio at `45bfae0f37ff5af4befb66dae72490e83f8b3795` (`cmake/FetchDynamoRIO.cmake`).
- lhns/avxemu at `2412126c1f8af48abdbbed7073882ae0e81f4b70` (`CMakeLists.txt`).

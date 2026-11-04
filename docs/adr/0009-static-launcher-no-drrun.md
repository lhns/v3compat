# 0009: A static launcher that execs DynamoRIO directly

## Status
Accepted

## Context
The launcher is prefixed to any program, including images with no shell, no libc, a read-only root and no writable home. A shell
script needs `/bin/sh`; drrun needs glibc and writes a config file. DynamoRIO reads its options from the environment and propagates
them across `execve`, so neither is needed.

## Decision
`bin/v3compat-run` is a static, non-PIE, freestanding program with no libc. It sets `DYNAMORIO_EXE_PATH`, `DYNAMORIO_RUNUNDER` and
`DYNAMORIO_OPTIONS`, then execs `libdynamorio.so` with the app's argv. The client links no libc, and drrun and libdrpreload are not
shipped. This is the interface the OCI wrapper depends on: `v3compat-run [--] prog args...`, located by its own path, with
`V3COMPAT_NATIVE=1` to bypass it. It makes no CPU-based choice.

An exec that cannot run correctly under the translator exits with status 87 and a message, the kernel's own rule deciding whether
privileges would change. The client rewrites the syscalls DynamoRIO does not follow rather than changing the fork, keeping the fork's
diff to what cannot be done from outside.

## Consequences
Nothing in an image is needed beyond the mounted install. We rely on DynamoRIO's early-injection environment contract rather than its
supported tool, so a DynamoRIO update must rerun the exec tests. A refusal ends the process rather than failing the `execve`, so a
caller cannot fall back. The install path may not contain spaces, quotes or `;`, as the option string cannot carry them.

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

## Amendment, 2026-11-04
A refused exec under the client fails instead of ending the process, so a caller can handle it, keeping the stderr message; only the
launcher, which has no caller, still exits 87. Nothing executes on a refusal, so no fallback runs a child with the wrong rights.
setuid, setgid, file capabilities and unreadable files return `EPERM`, on which `execvp` stops, where `EACCES` would run the next
file in `PATH`. 32-bit children return `ENOEXEC`, as a kernel without 32-bit support does, on which glibc's `execvp` hands the file to
`/bin/sh`.

## Amendment, 2026-11-04 (2)
An exec opens the file once, judges that descriptor and has DynamoRIO load from it, including through the launcher, so a swapped path or
a file that cannot be judged is never run. It needs a small fork API, `dr_syscall_set_exec_file`, as what the client can rewrite still leaves
DynamoRIO opening the path and falling back to a real exec; the fork's diff stays to what cannot be done from outside. The client
resolves shebangs itself, as the launcher does.

## Consequences
Nothing in an image is needed beyond the mounted install. We rely on DynamoRIO's early-injection environment contract rather than its
supported tool, so a DynamoRIO update must rerun the exec tests. The install path may not contain spaces, quotes or `;`, as the option string cannot carry them.

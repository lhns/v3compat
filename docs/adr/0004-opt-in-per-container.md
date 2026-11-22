# 0004: Opt-in per container through an OCI wrapper

## Status
Accepted, with the amendments below.

## Context
A global runtime hook would put every container on a node behind new code. Detecting which binaries need help cannot
be automatic either: a program may crash on its first missing instruction with no marker in the ELF file.

## Decision
A separate repo, [v3compat-runtime](https://github.com/lhns/v3compat-runtime), provides an OCI runtime wrapper, selected per
pod through a RuntimeClass. It fails open: any wrapper error passes through to the real runtime. On a capable host
it runs the container natively. That choice keys on a pinned node label rather than CPUID, as masking CPUID does not
disable the silicon, so a VM's `lzcnt` can change meaning when it migrates.

## Consequences
The same pod runs natively on capable nodes and translated on old ones. A failure affects one container. Each
workload has to opt in, and nothing happens automatically.

## Amendment 2026-11-22
Native passthrough needs both the node's install flag (`install.sh --native`) and CPUID showing x86-64-v3; if they
disagree, the container is translated. Translating is correct on any CPU; running natively on a CPU without v3 is not.

## Amendment 2026-11-22, failure
Supersedes "fails open" on nodes that are not native: there a wrapper error refuses the container, as running natively
on a CPU without v3 crashes or silently computes wrong results ([ADR 0001](0001-correctness-over-speed.md)). Native
nodes still pass through.

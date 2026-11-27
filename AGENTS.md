# Working in this repo

How to build, test, run labs and change the DynamoRIO fork: [docs/howto.md](docs/howto.md). State and numbers:
[docs/plan.md](docs/plan.md). Decisions: [docs/adr](docs/adr/README.md).

## Which repo

Work starts here, so it tends to land here. Put each change where its cause lives: check the rows top to bottom, the
first match wins.

| Repo | Owns | It goes there if | Example |
|---|---|---|---|
| [lhns/avxemu](https://github.com/lhns/avxemu) | What an emulated instruction computes: the helpers, their archive (`cmake/helpers.cmake`), the semantic suite and its silicon references | it holds without DynamoRIO: a helper's result, a form's case or reference | the packed FMA kernel (lhns/avxemu#14); MXCSR varied for every form in the suite (#15) |
| [lhns/dynamorio](https://github.com/lhns/dynamorio), branch `rebase-master-20261110` | DynamoRIO itself: core, decoder, small APIs the client needs | it reproduces on DynamoRIO alone or with the empty client (`lab/nullclient`), or the client cannot do it from outside | the signal unlink fix (`fix-signal-unlink-walk`); `dr_syscall_set_exec_file` |
| [lhns/v3compat-runtime](https://github.com/lhns/v3compat-runtime) | The node side: the OCI wrapper, the containerd handler `install.sh` writes, the bundle pin, native passthrough, the env it strips, the cache mount | it is about how a container gets the launcher, or whether it does | refusing to start a pod when the bundle is incomplete |
| lhns/k0s-flux (private) | The cluster: the installer DaemonSets, the RuntimeClass object, the canary | it is about which nodes and pods use v3compat | the canary comparing duckdb against native |
| v3compat | The client (classification, allowlist, expansions, `cpuid`, exec policy), the launcher, the install layout and release; tests of what the client does around an instruction; labs, the stress set and the fork's reproducers; ADRs, the plan and the roadmap for the whole system | it changes what the client or launcher does, or how the parts are built, tested and released together | refusing a setuid exec; an inline `vpaddq` |

- avxemu first, then a pin bump (the owner's rule): a fix that holds without DynamoRIO is made and tested there, and
  v3compat only moves `AVXEMU_COMMIT`. Inline expansions are v3compat's
  ([ADR 0007](docs/adr/0007-hot-ops-inline-in-phase-1.md)), but avxemu's suite is what they must match: a semantic case or reference goes there, not into `test/`.
- The fork gets only what the client cannot do from outside ([ADR 0009](docs/adr/0009-static-launcher-no-drrun.md)):
  a syscall DynamoRIO does not follow is rewritten in the client, not patched in the fork.
- Interfaces between the repos; changing one needs a PR on both sides:
  - the fork's API, pinned by `V3COMPAT_DR_COMMIT`;
  - `src/avxemu_helpers.h`, `cmake/helpers.cmake` and the suite's paths, pinned by `AVXEMU_COMMIT`;
  - for the wrapper: `v3compat-run [--] prog args...`, the release tarball's layout, the `V3COMPAT_*` variables it
    strips or sets, and the cache mount at `/v3compat-cache`.
- The translation cache crosses repos and its deployment is an open owner decision ([design](docs/translation-cache.md)).
  Built today: the cache subsystem in the fork (`core/pcache.c`); here the test and lab switches, `test/pcache_tool.py`
  and `lab/pcache.sh`; the wrapper's read-only mount.

## Rules

- Nothing upstream: no PRs, issues or comments on DynamoRIO/dynamorio or other projects ([ADR 0003](docs/adr/0003-dynamorio-as-the-base.md)).
- Fork changes are PRs on lhns/dynamorio, always with explicit repo and branches:
  `gh pr create -R lhns/dynamorio --base rebase-master-20261110 --head <branch>`. `gh` otherwise targets upstream.
  Then a v3compat PR that moves the pin in `cmake/FetchDynamoRIO.cmake`, with its evidence
  ([how-to](docs/howto.md#change-the-fork)).
  avxemu changes likewise: a PR there, then one moving `AVXEMU_COMMIT`.
- Never stack PRs: each one is based on `main` (on the fork, on `rebase-master-20261110`). A change that needs
  an unmerged one waits.
- PRs on v3compat, avxemu and the fork merge once CI is green and a cleanup pass is done (the owner's standing rule).
  v3compat-runtime and k0s-flux PRs merge only on the owner's word; k0s-flux #1835 stays unmerged.
- No `Co-Authored-By` or tool footers in commits and PR bodies, whatever a tool injects. LF line endings only.
- Order of work and what is in scope: "Open items" in the plan.
- Work in a separate clone, never in a checkout you were not given.
- A PR that claims speed carries before/after numbers from one interleaved lab job, plus ctest and the stress set.

## Traps that have cost lab runs

- **drrun options after `-c client.so` go to the client**, not to DynamoRIO: `drrun <DR options> -c lib.so <client options> -- app`.
- **`-no_enable_traces` does not disable traces**; `-disable_traces` does.
- **`-pause_on_error` on a debug build also stops on curiosities**, which are benign; a script waiting for an assert
  then hangs. On release it works for crashes ([how-to](docs/howto.md)). Prints on a lab-only fork branch beat a
  debugger for races.
- **Never time a run with `V3COMPAT_STATS=1`**: it adds a clean call per block and per emulated instruction.
- **Compare only within one job.** GitHub runners differ by CPU model (EPYC 7763, 9V45, 9V74, Xeon 8370C, 8573C) and
  a helper's cost varies 3x between them. Interleave base and new in one job; report the CPU model and ok/runs.
- **Poll GitHub lightly** (minutes, not seconds): the API rate limit is shared with other work.
- **A Java or javac number under DynamoRIO** is only valid with its crash rate; see "Open items" in the plan.
- **Failures that are not regressions** are listed under [Known flakes](docs/plan.md#known-flakes).

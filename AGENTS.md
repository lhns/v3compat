# Working in this repo

How to build, test, run labs and change the DynamoRIO fork: [docs/howto.md](docs/howto.md). State and numbers:
[docs/plan.md](docs/plan.md). Decisions: [docs/adr](docs/adr/README.md).

## Rules

- Nothing upstream: no PRs, issues or comments on DynamoRIO/dynamorio or other projects ([ADR 0003](docs/adr/0003-dynamorio-as-the-base.md)).
- Fork changes are PRs on lhns/dynamorio, always with explicit repo and branches:
  `gh pr create -R lhns/dynamorio --base rebase-master-20261110 --head <branch>`. `gh` otherwise targets upstream.
  Then a v3compat PR that moves the pin in `cmake/FetchDynamoRIO.cmake`.
- Never stack PRs: each one is based on `main` (on the fork, on `rebase-master-20261110`). A change that needs
  an unmerged one waits.
- Merge only on the owner's word: an explicit merge instruction from the GitHub account `lhns`, which owns this repo,
  the fork and avxemu.
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

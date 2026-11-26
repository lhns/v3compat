# Handover

For an agent taking over v3 work across [lhns/avxemu](https://github.com/lhns/avxemu), lhns/v3compat, the DynamoRIO
fork [lhns/dynamorio](https://github.com/lhns/dynamorio) (branch `rebase-master-20261110`),
[lhns/v3compat-runtime](https://github.com/lhns/v3compat-runtime) and the k0s-flux integration. Read [AGENTS.md](../AGENTS.md)
first; state and numbers are in [plan.md](plan.md), not repeated here.

## Where things stand

- 0.1.0 is released and its output verified byte-identical to native on Ivy Bridge, with the cache ([CHANGELOG](../CHANGELOG.md)).
- The runtime runs `v3compat-run` instead of SDE.
- Plan Phase 2 (speed) is in progress; Phase 3 and 4 state is in the [Phases table](plan.md#phases).

Read: the [roadmap](roadmap.md) (every piece of work with its verdict, and what is open); plan.md "Phases" and
"Open items"; [translation-cache.md](translation-cache.md); ADRs [0004](adr/0004-opt-in-per-container.md),
[0006](adr/0006-read-only-translation-cache.md),
[0008](adr/0008-audited-list-keyed-on-shape.md), [0009](adr/0009-static-launcher-no-drrun.md).

## Owner's standing rules

- v3compat, avxemu and dynamorio PRs merge automatically after a cleanup pass.
- No rollout to the cluster for now: k0s-flux PR #1835 stays unmerged.
- Nothing upstream: no PRs to DynamoRIO.
- What holds without DynamoRIO goes to avxemu first, then a pin bump.
- Lab jobs on `kube-worker-04` may be restricted as needed; never cordon or taint the node or restrict the CI runners.
- Worktrees live in the session scratchpad, never in the user's git folder.
- A speed PR carries one interleaved lab job.
- Update the roadmap (`docs/roadmap/nodes.yaml`) in the same push as each result, merge or owner decision, and run
  its build and check first ([how-to](howto.md#roadmap)).
- kubectl works only through the jumphost: `ssh lhns@10.1.5.15 'kubectl ...'`. Never paste secrets.

## Open owner decisions

1. **#1835, the canary cache.** Drop the cache first (recommended) or keep it, which needs ADR 0006 work. Its CHANGELOG
   line saying the launcher does not use the cache needs fixing either way.
2. **Traces default**, given the [Ivy Bridge A/B](plan.md#ivy-bridge-traces-ab-2026-11-26): `-disable_traces` used
   27-33% less CPU on javac and java. README's claim is marked contradicted.
3. **Fork fix for the exit-time SIGSEGV**: keep SIGSEGV safe during the default action
   ([plan.md, Open items](plan.md#open-items)).
4. `test/misc_forms.c` keep or not; retire `semantics.intel.ref`; full-mode `semantic-hw` for imm8 as a nightly.

## Running

Job `ivy-traces`, namespace `v3compat-lab-ivy`: the signal section (nt, 1,000 runs) was at 50 of 1,000 with 0 bad,
expected done about 19:30 local on 2026-11-26. The pod then sleeps 7,200 s. To finish:

1. `ssh lhns@10.1.5.15 'kubectl -n v3compat-lab-ivy logs job/ivy-traces'`, read the `IVY-AB-JSON` line.
2. Record the signal result in plan.md's traces A/B section.
3. Delete the namespace `v3compat-lab-ivy`.

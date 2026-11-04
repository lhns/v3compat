# Architecture decision records

Hard-to-reverse decisions. An accepted decision is not edited: a new record supersedes it. "Proposed" means it
depends on a measurement in the [plan](../plan.md) that is not in yet.

| # | Decision | Status |
|---|---|---|
| [0001](0001-correctness-over-speed.md) | Correctness over speed; refuse what is not understood | Accepted |
| [0002](0002-same-isa-binary-translator.md) | A same-ISA binary translator, not a kernel module or patching | Accepted |
| [0003](0003-dynamorio-as-the-base.md) | Build on our DynamoRIO fork | Accepted |
| [0004](0004-opt-in-per-container.md) | Opt-in per container through an OCI wrapper | Accepted |
| [0005](0005-avxemu-core-as-helpers.md) | AVXEmu's core as helpers, its mechanism retired | Accepted |
| [0007](0007-hot-ops-inline-in-phase-1.md) | Hot operations inline in Phase 1; the helper boundary | Accepted |
| [0006](0006-read-only-translation-cache.md) | A read-only translation cache with a separate writer | Proposed |

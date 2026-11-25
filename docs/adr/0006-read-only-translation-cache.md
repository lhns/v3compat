# 0006: A read-only translation cache with a separate writer

## Status
Accepted, with the amendments below.

## Context
Translating at every start repeats work, and starting fast is a goal. A writable cache shared between containers is
an attack path, as a poisoned entry runs in another container's process: the same class of attack was demonstrated
against Rosetta's cache. A review of AVXEmu's cache found predictable temp files and an unkeyed checksum.

## Decision
Key entries by file hash. Containers mount the cache read-only. A separate writer creates entries, writing and then
renaming. Stale or corrupt entries are rejected and translation falls back to fresh. There is a size cap.

## Consequences
Running containers cannot poison it. The first start of a new binary is slower until the writer has run. A separate
writer has to be deployed, and invalidation is by content rather than time.

## Amendment 2026-11-12
Proposed in [translation-cache.md](../translation-cache.md), for the owner to decide: entries would be finished
fine-grained blocks. "Key entries by file hash" would become an index plus a compare against the bytes in memory at
install. DynamoRIO's `-persist` is not the base.

## Amendment 2026-11-17
The cache lives on each node of a CPU class, in a root-owned directory, one file per image digest. A training Job
pinned to that class writes it; containers mount it read-only. A Job writes to a staging path and a separate step
renames the file into the tree, so one image's run cannot write another image's entries. Integrity starts with a
hash manifest; fs-verity can replace it where the node's filesystem supports it.

## Amendment 2026-11-25
The per-image cache is a directory per image digest, mounted read-only at `/v3compat-cache`, holding one
`<program>.pcache` file per program (the launcher reads `DIR/<program>.pcache`). This replaces "one file per image digest".

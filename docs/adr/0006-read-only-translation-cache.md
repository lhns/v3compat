# 0006: A read-only translation cache with a separate writer

## Status
Proposed: depends on Phase 3.

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

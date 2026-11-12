# Translation cache across runs: design

**Status:** a design for the owner to decide on. Nothing is built. It works out
[ADR 0006](adr/0006-read-only-translation-cache.md), which stays Proposed until the owner decides. Tags as in the
[plan](plan.md): **[measured]**, **[source]**, **[estimate]**.

## Summary

- **What would be cached:** finished basic blocks. That is the code-cache body DynamoRIO emits for a block after our
  client and DynamoRIO's mangling, for code in file-backed module mappings that were never writable. On a miss DR
  installs the body as an ordinary fine-grained fragment, without decoding, without calling the client and without
  encoding. Everything else stays as it is: linking, flushing, pending deletion, signals and traces.
- **What makes a hit correct:** the block's source bytes in memory must equal the bytes the entry was built from
  (a compare at install), and the entry's key must match this build of DR, the client and the allowlist, the cache-affecting options
  and the CPU's SIMD state. Every layout-dependent choice must also be re-checked at install. If any of these fails,
  the block is translated fresh.
- **Who writes it:** a trusted writer, never the container that reads it. The first deployable writer is a
  training run at build time. Containers mount the cache read-only.
- **Gain** **[estimate]**, from a measured profile on GitHub runners. duckdb `select 42` on EPYC 7763, where native is
  0.03 s:
  - with traces on, from 1.09 s to about 0.45-0.5 s, about 2x;
  - with `-disable_traces`, about 0.25-0.3 s. That is about 4x against today's default, and it requires turning
    traces off for startup.

  If Ivy Bridge scales like the runners (a cold start there is 5.1 s), a warm start there is about 1.3 s. That still
  misses Phase 3's gate of 0.5 s, so the cache alone does not get there.
- **Smallest useful first step:** a lab-only prototype on a fork branch, with ASLR off, verify mode on and a numeric
  stop criterion ([Phases](#phases)). Before that, the owner needs to run the startup ladder on Ivy Bridge.

## Where startup goes

Lab `lab-pcache-startup` (runs 43278460957 and 43280614743), GitHub runners, medians of 10 interleaved reps, every run
`rc=0` **[measured]**. The configurations are native, DynamoRIO alone (`-no_enable_reset -no_ignore_syscalls`), an
empty client (`lab/nullclient`), our client through `v3compat-run`, and our client with `-disable_traces`. Wall times
in seconds:

| Workload | Native | DR alone | Empty client | Client |
|---|---|---|---|---|
| static hello, EPYC 7763 | 0.00 | 0.01 / 0.01 | 0.01 / 0.01 | 0.02 / 0.01 |
| duckdb `select 42`, 7763 | 0.03 | 0.69 / 0.50 | 0.89 / 0.65 | 1.09 / 0.80 |
| duckdb compute, 7763 | 0.17 | 0.91 / 0.72 | 1.14 / 0.89 | 1.57 / 1.31 |
| `node -e 0`, 7763 | 0.02 | 0.52 / 0.38 | 0.66 / 0.47 | 0.81 / 0.60 |
| duckdb `select 42`, EPYC 9V74 | 0.04 | 0.64 / 0.46 | 0.84 / 0.60 | 1.02 / 0.74 |
| duckdb `select 42`, EPYC 9V45 | 0.02 | 0.40 | 0.53 | 0.60 / 0.44 |

Each cell is traces on / `-disable_traces`. The 9V45 row is from the first run, which had no `-disable_traces` rung for
DR alone or the empty client. The first run's 7763 numbers agree within 0.02 s.

- **DR's own startup is not the cost:** a static hello runs in 0.01-0.02 s under the client. Startup is per-block
  work. duckdb `select 42` builds 77k blocks and 3.4k traces, all from file-backed modules, about 417k instructions.
  `node -e 0` builds 61k blocks, 94% from modules and 3.9k from JIT or other anonymous memory **[measured]**
  (`lab/countclient`). That comes to about 14 µs a block under the client on a 7763, or 10 µs without traces.
- **Traces cost 0.29 s of duckdb's 1.09 s** under the client (3.4k traces), and 0.19 s on DR alone. DR builds a trace
  by decoding its blocks again and calling the client again. A block cache does not cover that.
- **Where the time goes, layer by layer**, for duckdb `select 42` without traces: DR alone 0.50 s, the empty client
  (DR's full decode for any client) 0.15 s, and our client's own work 0.15 s.
- **The profile** (`perf -e cpu-clock`, duckdb `select 42` under the client, three runners) buckets as follows: DR building
  and mangling the IR 23%, lookup, link and dispatch 13-18%, encode and emit 11-12%, heap and locks 10-18%, decode 7-9%,
  our client 5-6% (`allow_key`, `put_opnd` and `in_table` lead), drmgr about 1%, kernel 3-4% and app code in the cache
  3-4% **[measured]**. On DR alone, lookup, link and dispatch are 20-25%.
- **Inclusive time** (`--call-graph dwarf`, duckdb `select 42` under the client, four runners) **[measured]**:
  `build_basic_block_fragment` 62-63% with traces on and 73% with `-disable_traces` (7763; `node -e 0` 80%), made up of `build_bb_ilist` 33-36% (with `instrument_basic_block`, our client and
  drmgr, 11%), `emit_fragment_ex` 25-26% and mangling 6-7%. Linking and the fragment table inside it are only
  about 3% (`link_new_fragment`, `fragment_add`). Trace emission is 5% of samples; the rest of the trace cost shows up as
  blocks built again for traces. `node -e 0`: 52-69%.
- **What a block cache can remove:** `build_basic_block_fragment` less its linking and table insert. That is decode, IR build
  and mangle, encode and emit, the client, and the heap work that belongs to the IR: about 60% of samples with traces on and 70% without. In seconds on the 7763 that removes about 0.66 s of 1.09 s, or 0.56 s
  of 0.80 s. The rest is still paid on every hit:
  dispatch to a new tag, hashtable insert, linking, faults on fresh cache pages and the app itself, plus the cost of a
  hit (compare, copy, relocate) **[estimate]**.

## What is cached

One entry per block, in the client-plus-mangle output form that DR places in the cache:

- **Source:** the module's identity (an index key, below) and the block's offset in the module. Under `-code_api`,
  DR elides no jumps or calls (`max_elide_jmp` and `max_elide_call` are 0, `core/options.c`) **[source]**, so a block's
  source is one contiguous range `[tag, tag+len)`. Its bytes are stored in the entry.
- **Body:** the emitted fragment body without exit stubs, plus relocation records. DR regenerates the stubs at
  install, as it does for any new fragment, because they hold per-run heap addresses (linkstubs).
- **Install metadata:** exit list (target offsets, types, the cti's offset in the body), fragment flags and
  layout preconditions.

Out of scope for the cache:
- traces;
- code in anonymous memory or in any mapping that has been writable (JIT, self-modifying code, text relocations);
- blocks DR builds specially (rseq regions, the syscall paths of `-no_ignore_syscalls`, and anything marked
  "selfmod sandbox").

All of these are translated as today. Traces could follow later; they are a separate design.

Fine-grained, not coarse: an installed entry is an ordinary fragment, so flush, unlink and pending deletion stay the
paths [ADR 0010](adr/0010-no-cache-reset.md) relies on. Nothing new relocates a thread.

## Key, validation and invalidation

There are two separate jobs.

**The index finds candidate entries.** It needs to be cheap and does not decide correctness. The key is:
- the module's ELF build-id, falling back to a hash of its ELF headers and program headers (cheap, and it matches
  across the writer's and the reader's mounts);
- a **build identity**: hashes of `libdynamorio.so`, `libv3compat.so` (which includes the allowlist and avxemu's
  helpers) and `libdrmgr.so`;
- the canonical cache-affecting DR and client options, as the launcher builds them;
- the **CPU's SIMD state**: CPUID feature words and XCR0. DR's clean-call save and restore depends on them (`ZMM_ENABLED`,
  `proc_num_simd_saved`, AVX512BW choosing `kmovq` over `kmovw` in `core/arch/x86/mangle.c`), and so does whether
  AVX-512 code has been seen yet, which can change mid-run **[source]**. Ivy Bridge has none of these, so one
  key covers a node class. Entries built on AVX-512 runners will not match Ivy Bridge. The cache therefore has to be built
  on the target CPU class.
- `-stats`, `-audit` and `V3COMPAT_DR_OPTS`: any non-default value turns the cache off. Rather than key on these, the
  launcher passes no cache path.

**Validation decides correctness.** It runs at install, for every hit:
1. The source range lies in a file-backed mapping of that module that DR's vmareas mark `VM_UNMOD_IMAGE` ("mmapped
   in and read-only since then", `core/vmareas.c`) **[source]**, and the whole range is readable and executable, as DR
   checks when it builds a block.
2. The bytes in memory equal the entry's source bytes, checked with a `memcmp` of about 20-40 bytes. This is the check
   that matters. The plan notes that writes to an executable file are not denied, so a file hash alone would not be
   enough.
3. The entry's checksum holds, against corruption.
4. Every layout-dependent choice reproduces at this address. Rip-relative, rel32 and helper targets must be within
   reach from where the body lands. Exit padding must come out the same: the fork's torn-exit fix shifts a block's start
   by 0-3 bytes and pads inside it (`nop_pad_ilist` in `emit_fragment_common`, `core/emit.c`), and both depend on the
   cache address modulo 16 **[source]**. Install therefore places the body at the recorded alignment, or the block is
   a miss. Getting this wrong would break byte-identity and the torn-exit guarantee together.

Install must also do what `emit_fragment_common` does after it encodes a block, in particular `vm_area_add_fragment`.
That registration is what lets a later `munmap`, `mprotect` or write to the source flush the fragment. Verify mode
cannot catch a missing registration. A test unmaps a library, maps different code at the same address and checks
that the new code runs.

Any failure is a miss, and the block is translated fresh. Nothing in the reader repairs or rewrites an entry.

**Invalidation** is by content, as ADR 0006 says. A new module build, DR, client or allowlist gives a new key, so
old entries simply stop matching. The writer collects entries that no recent training run has produced and enforces
the size cap. No entry is ever removed from a running process except through DR's ordinary flush.

## Correctness

- **JIT and self-modifying code never hit.** Validation step 1 excludes them. A page that becomes writable later is
  flushed by DR's existing consistency path, and the next build of that block is fresh, because the vmarea is now
  marked as written.
- **ASLR and PIE.** A body contains absolute values: app addresses (the pushed return address of a mangled `call`, the
  app PC our refusal passes to `refuse_call`, exit targets), DR's generated routines, the client's helper addresses and
  rip-relative displacements to app data. Relocation classes are module base, DR base and client base. Offsets
  into DR's TLS and our raw TLS (`tls_offs`) are fixed per build and options and are checked at install. The prototype
  runs with ASLR off to measure the gain before relocation is built. The production design records relocations
  where DR creates those values (mangle, exit cti, clean call) and does not scan the bytes for them. A body containing a value no record
  explains is not cacheable; verify mode is what finds such bodies.
- **Signals and state recreation.** When a fault or signal arrives in a fragment, DR rebuilds the block from the app
  bytes, calls the client with `translating=true`, and walks that list in step with the code in the cache
  (`recreate_app_state_from_ilist`, `core/translate.c`) **[source]**. A cached body therefore has to be byte-identical
  to what a fresh translation at the same place would produce. A divergence is detected only if the walk overshoots the
  target PC; otherwise the state is silently mis-mapped. Validation step 4 and the client's determinism make
  the two identical. Verify mode checks it. The fallback is DR's stored translations (`DR_EMIT_STORE_TRANSLATIONS`)
  in the entry, at the cost of size.
- **The client must be deterministic in its inputs:** the block's bytes, the key and the layout preconditions. Today
  it is. `expand_init` reads only its options, and `classify` and `allow_key` read only the instruction. A future
  expansion that reads process state breaks the cache, and verify mode would catch it.
- **ADR 0008's audit still applies to every instruction.** The writer ran this client and this allowlist (both part of the key) on
  these exact bytes (validation step 2). A refused form is cached as its refusal call, which behaves the same. The audit has been
  done earlier, by the writer. It has not been skipped.
- **ADR 0001 asks for a differential test, and verify mode is that test:** `V3COMPAT_CACHE_VERIFY=1`. Every hit is
  also translated fresh and compared byte for byte after relocation; any difference exits 86 and names the block. CI
  runs the app tests and the stress set with a primed cache in verify mode. The prototype includes it from the start.
- **Exec and threads.** Children reach the cache through the same launcher options. Installs go through DR's existing
  shared-fragment locks.

## Security

A cache entry is code that runs in the reading process. Anyone who can write the cache can run code in every container
that reads it; this class of attack was demonstrated against Rosetta's cache (ADR 0006).

- **Containers supply at most offsets, never code.** Every entry is produced by a trusted writer from bytes it reads
  itself. A later node writer may take lists of block offsets from containers as hints, and those are untrusted data
  that select what to translate, nothing more.
- **The cache is mounted read-only into containers**, from root-owned files that the container's users cannot write. The writer
  writes to a new file and renames it into place (ADR 0006), never in place.
- **Integrity:** each entry carries a checksum against corruption, and the file as a whole is protected by fs-verity
  or by a hash in a signed manifest, checked lazily per page. Hashing the whole file at every start costs time
  **[estimate]**: about 10-20 ms for duckdb's size.
- **What the cache does not defend against:** root on the node, or a malicious writer build. Either already owns the
  node.

## Where it lives and who writes it

- **First writer: a training run on the target CPU class**, as a build step (`v3compat-cache build -- <workload>`).
  It runs the workload under the client in writer mode and emits the entries for the blocks it built. The result is
  a read-only directory, shipped as an image layer or a node-local path. Training on CI runners does not work while
  the key includes CPUID (above), so building per image needs an Ivy Bridge-class builder.
- **Per node class, not per image**, is the likely default. One cache serves every image on Ivy Bridge nodes,
  because module identity is in the key, and shared libraries (libc, libstdc++) are shared across images.
- **Later: a node writer** (a DaemonSet), fed with offsets by containers, translating offline in a fork mode that
  builds blocks at given offsets without running them. That mode does not exist yet, so the node writer belongs to a later phase.
- **The opt-in stays per container** ([ADR 0004](adr/0004-opt-in-per-container.md)). The cache is an extra read-only mount and a
  launcher option; without it everything is as today.

## Cost and complexity **[estimate]**

- **Fork:**
  - a lookup hook in `build_basic_block_fragment` before `build_bb_ilist`;
  - install: allocate at the recorded alignment, copy, relocate, then regenerate stubs and linkstubs from the exit
    list, and register the fragment with vmareas as `emit_fragment_common` does;
  - relocation records at the creation sites;
  - writer mode;
  - verify mode.

  This is roughly 1.5-3k lines in DR core: new code on our side that every rebase onto upstream carries, in the same
  area as the fixes the fork already maintains.
- **v3compat:** the launcher option, the cache file format and its tool, and the CI jobs for verify mode.
- **Tests:** verify mode over ctest, the stress set and duckdb hashes; corrupted, truncated, stale and mismatched-key
  caches must all fall back (fail-closed tests); a module modified in memory after load; ASLR on.

## Alternatives considered

- **Reviving DR's `-persist` (coarse-grain persisted units)** is rejected.
  - *It loses time:* with no client and a cache generated once, then used without being rewritten (the file hashes
    are unchanged), medians of 5 interleaved reps per runner (run 43280614743) **[measured]**:

    | Runner | Workload | Plain | Split only, no cache | Using the cache |
    |---|---|---|---|---|
    | EPYC 9V74 | duckdb `select 42` | 0.47 s | 0.83 s | 3.03 s |
    | EPYC 9V74 | `node -e 0` | 0.36 s | 0.58 s | 1.52 s |
    | EPYC 9V74 | hello (PIE) | 0.01 s | 0.02 s | 0.06 s |
    | Xeon 6975P | duckdb `select 42` | 0.48 s | 1.04 s | 1.62 s |
    | Xeon 6975P | `node -e 0` | 0.36 s | 0.74 s | 0.80 s |
    | Xeon 6975P | hello (PIE) | 0.01 s | 0.02 s | 0.04 s |

    On EPYC 7763 (run 43291978957) the same three columns are 0.70 / 1.18 / 4.21 s for duckdb. The use-only runs do
    open the duckdb and node `.dpc` files (strace, `open`) **[measured]**. The splitting that x64 needs (below) costs
    50-110% by itself, and using the cache costs more on top.
  - *It does nothing with a client:* with any client that does not register persistence, nothing is written
    **[measured]**. Supporting it needs `DR_EMIT_PERSISTABLE` and the `dr_register_persist_*` callbacks.
  - *It does not suit our client:* a block with a meta cti whose target is not an instr (a clean call to a helper)
    cannot be coarse (`core/arch/interp.c`), so every expanded block falls out **[source]**.
  - *On x64 it splits blocks:* `-persist` turns on `-coarse_split_riprel` and `-coarse_split_calls` for correctness.
    Every rip-relative instruction and every call then becomes its own fine-grained block, and full decode is forced
    **[source]**.
  - *It conflicts with ADR 0010:* coarse units are destroyed and frozen by an all-thread synch that redirects threads
    to dispatch (`perscache.h`; `coarse_units_freeze_all` translates threads), and ADR 0010 names coarse-grain units as
    bringing the reset faults back **[source]**.
  - *It is weak and unmaintained:*
    - by default it validates with an MD5 of the module's first and last 4 KB (`-persist_load_validation 0x5`);
    - owner checks are unsupported on Linux;
    - the 64-bit Linux freeze and persist tests are disabled ("64-bit support not quite there yet");
    - `client.pcache` needs `-no-pie` (i#2868);
    - the persistence work dates from 2006-2012 (Determina, then i#40 client support), and only maintenance commits
      followed.
- **Caching only our client's work** (audit result, expansion decisions) is rejected. Its bound is the client's
  share: 11% inclusive (`instrument_basic_block`), or the 0.15 s step from the empty client to ours without traces, of
  0.80 s. DR still decodes,
  mangles and encodes every block. The README already notes that memoising the allowlist check gained nothing.
- **Caching decoded instruction lists and re-encoding at install** saves decode and the client but not mangle or
  encode, about a third of the bound, and needs the same validation. That is most of the complexity for a minority of the gain.
- **Ahead-of-time rewriting** is rejected by [ADR 0002](adr/0002-same-isa-binary-translator.md). It needs instruction boundaries
  that libraries and JIT code cannot supply. A cache keeps runtime translation as the authority and only skips work
  already done on proven-identical bytes.
- **Snapshotting a started process (CRIU)** captures app state, not just translation, and needs privileges and
  support from the wrapper. It is not a general mechanism for opt-in containers.
- **Tuning without a cache:** `-disable_traces` removes 0.29 s of 1.09 s today and costs about 3% on long runs
  (README). A cheaper allowlist key and fewer `/proc/self/maps` reads (plan, ranked list) cost less, but none of
  them reaches the per-block cost.
- **Eager install of a whole module's entries at load, pre-linked,** could also remove the dispatch and link share
  that a lazy cache keeps. It is a later option once lazy install is measured. It must not become coarse units under
  another name, and must only add fragments, never relocate threads.

## Phases

Each phase ends at a go/no-go for the owner.

0. **Ivy Bridge ladder.** Run the startup ladder (the PR adding this note has its script) on the owner's Ivy Bridge host.
   It answers whether Ivy Bridge's 5.1 s splits like the runners' 1.09 s. If kernel, faults or something
   other than block building dominates there, the cache is the wrong tool.
1. **Prototype, lab only.**
   - **Build:** a fork branch with the lookup hook, install, writer mode and verify mode. ASLR is off (`setarch -R`)
     and there is no relocation, no signing and no deployment.
   - **Measure:** duckdb `select 42` and compute, and `node -e 0`, warm against cold in one interleaved job; verify mode
     clean over ctest and the stress set.
   - **Stop criterion:** stop if warm `select 42` is not at least 40% below cold on runners with traces on, or if verify
     mode finds a mismatch that cannot be fixed by the key or the preconditions.
2. **Relocation and format.** Relocation records with ASLR on, the cache file format with checksums, the full key,
   the fail-closed tests and verify mode in CI.
3. **Deployment.** The training-run writer as a build tool, the launcher option, the read-only mount from the runtime
   wrapper, and integrity through fs-verity or a signed manifest. ADR 0006 is revised to Accepted, or superseded,
   with what was learned.
4. **Optional:** eager install, traces, and the node writer with offline translation.

## Decisions for the owner

1. Whether to fund Phase 1 when the expected gain is about 2x on startup, or 4x with traces off, not the 10x Phase 3 needs. Or whether to revise
   Phase 3's gate first, after the Ivy Bridge ladder (Phase 0, which needs access to that host).
2. Whether to accept a new subsystem of about 2k lines in the fork's core, carried through every rebase.
3. Whether the cache is per node class, built on the target CPU (recommended), or per image, which needs Ivy
   Bridge-class builders in CI.
4. Integrity: fs-verity on the nodes or a signed manifest.
5. Whether a training run at build time is an acceptable first writer, given that coverage is whatever the training
   workload executes.

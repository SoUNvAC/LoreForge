# Incremental analysis and dependency graph

Phase 19 adds the provider-neutral `LoreForge::Incremental` coordinator. It owns artifact
identity, input/output hashes, dependency edges, dirty state and selective rebuild ordering.
It never modifies imported source, calls a model implicitly, or stores a credential.

## Chapter pipeline

`ChapterPipeline::specifications` validates a hash-bound UTF-8 revision and its canonical
zero-based, contiguous chapter sequence. IDs are scoped by project and stable chapter ID.
Each chapter has these dependencies:

```text
exact source projection -> parse -> analysis -> Story State through this chapter
previous Story State ------------------------> Story State through this chapter
previous Story State -> analysis (only when configured to consume earlier narrative context)
```

The source projection hash includes chapter bytes, source ID and absolute byte boundaries.
A same-length edit to chapter 37 dirties its parse/analysis and Story State from 37 onward;
unrelated local chapter analysis remains clean. A length-changing edit also dirties shifted
chapter projections: old absolute evidence ranges cannot be reused. Original full-novel hashes
remain revision validation data, not an edge forcing every local chapter through the model.

Parser, analyzer, model, prompt, schema and Story State versions are explicit. Model version
must identify the actual weights/backend revision, not merely a mutable model alias. A canonical
configuration hash must cover all other output-affecting settings: inference parameters,
context policy, protected terms and relevant preprocessing. Context-dependent analysis must set
`analysisUsesPreviousStoryState`; omitting an actual input dependency is a caller error.
Additional context/diagnostic artifacts can use the generic graph with their actual dependencies.

## Topology, hashes and invalidation

`DependencyGraph::synchronize` atomically replaces the full topology. Invalid hashes, missing or
duplicate dependencies, duplicate IDs, source dependencies and cycles are rejected before any
state changes. Compatible clean results and running tickets survive a no-op synchronization.
Changed inputs, recipes, dependency order or topology dirty all transitive consumers. Removed
nodes disappear and their tickets cannot complete. New source projections are clean immutable
inputs; derived artifacts start dirty. Results never appear clean without their prerequisites.

A derived input fingerprint is SHA-256 over a versioned JSON array of artifact identity, kind,
recipe hash and ordered dependency identities/output hashes. JSON framing avoids ambiguous
concatenation. Output hashes describe actual canonical artifact payloads, not model request IDs.

`ChapterPipeline::invalidate` bridges Phase 18 plans, conservatively reparsing changed/provenance
chapters and invalidating snapshots at/after the cutoff. Invalid chapter/project scopes or
unresolved chapter remapping fail without mutation. Structural changes require a canonical new
chapter layout and resynchronization; the graph does not invent chapter mappings or rebase quotes.
Do not apply the same invalidation plan again after rebuilding, or the new work will be dirtied.

## Selective execution and asynchronous safety

`rebuildPlan(targets)` returns only dirty nodes in the requested prerequisite closure, in stable
topological order. Empty targets request all dirty nodes. Clean and unrelated nodes are excluded.
`rebuild` invokes an injected executor in that order, stops at failure and retains earlier successful
work. Failures stay dirty for retry; no downstream worker runs over a failed prerequisite.

For asynchronous adapters, `beginBuild` captures an immutable ticket containing dependency hashes,
input fingerprint, graph identity and a unique serial. `completeBuild` accepts a valid output hash
only if that exact ticket is still current. Invalidation revokes affected tickets, including when
bytes change and later return to their previous value. Foreign, forged, duplicate and late tickets
are rejected. Completion/graph mutation runs on one owning thread; this API is not a thread-safe
shared scheduler. Executors must stage payloads and validate completion before publishing them
to their owning repository, so a rejected ticket cannot overwrite current persisted artifacts.
The synchronous helper does not authorize reentrant topology changes from inside an executor.

## Durable receipts

`checkpoint()` emits `loreforge-dependency-checkpoint-v1` JSON with the full topology, configuration,
clean input/output receipts and dirty state. Running tasks are saved as dirty, not resumable jobs.
`restoreCheckpoint` requires an already synchronized matching topology and no running work.
It validates every source hash, ordered dependency, configuration and clean input fingerprint in
topological order, and restores atomically. Incompatible/malformed checkpoints leave the graph
unchanged; the caller can retain the current dirty plan and rebuild. No SQLite migration is needed.

Checkpoint bytes should be saved atomically in the project's derived-data storage, not Git-tracked
novel text. The coordinator provides serialization, not a UI autosave service. Checkpoints are
cache receipts, not signatures: callers must verify referenced artifact payloads against their
output hashes before restoring/using a cache. A deliberately rewritten consistent checkpoint
cannot prove that a model produced those payloads. Missing payloads must be explicitly invalidated.

## Integration boundary

Build specs, synchronize the graph, optionally apply a Phase 18 plan once, then request a target.
Workers delegate actual parsing, Context Engine/model calls, validated chapter analysis and
Story State rebuilding to their existing domain adapters. Persist derived records through
`StoryStateRepository`; persist checkpoint bytes only after accepting the matching output receipt.
This phase supplies scheduling/cache infrastructure, not a new desktop run button or an automatic
repair action. Deterministic tests use an injected worker and need no live model or network.

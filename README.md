# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

LLM desktop integration has begun with a separate workbench tab for the user's LAN-hosted
single-model API: configuration, synthetic connection/JSON probes, cancellation and honest
response metrics. Markdown chapters now support source-checked, versioned snapshots stored
in the project and complete message previews with heuristic budget checks. A separate
Novel Analysis tab now runs explicitly confirmed single-chapter analyzer/checker/reviewer
requests serially against the same model, with evidence validation, persistent history and
project/chapter token totals. No automatic novel sending, rewriting or batch runs.
See [LLM workbench](docs/llm-workbench.md) and the original
[integration proposal](docs/llm-cloud-integration-plan.md).
See [Novel analysis](docs/novel-analysis.md) for the workflow and validation limits.

Maintained Markdown source trees are the primary desktop import workflow. Use
**File > Import Markdown Source** with a directory containing `SUMMARY.md`, then
save a new `.loreforge` project outside that source tree. TOC order, volume labels,
stable path-based chapter IDs and per-file UTF-8 provenance are preserved. Website
pages and illustrations stay outside narrative analysis. See [Markdown source import](docs/markdown-source-import.md).

Phase 21 adds a versioned bilingual golden corpus and model regression suite with per-case and
aggregate dialogue, speaker, entity, event, unsupported-claim, context, proofreading, token and
latency metrics. Durable provenance-bound reports, cost/quality gates and explicit human review
protect model promotion. Offline tests use synthetic captures; they do not certify or switch a
real backend. See [model regression](docs/model-regression.md). The planned Phase 0–21 baseline
is implemented; real-model qualification and production integration still require their own review.
Phase 20 adds source-backed narrative integrity diagnostics for existence, death/speech,
character-specific knowledge, directed travel and canonical name variants. Explicit story time
supports flashbacks, evidence-backed exceptions remain auditable, and missing facts produce
notices instead of invented contradictions. Checks never rewrite source or guess temporal facts.
See [narrative integrity](docs/narrative-integrity.md).
Phase 19 adds content-addressed artifact dependencies, durable clean/dirty cache receipts,
selective topological rebuilding and stale asynchronous-result rejection. Chapter-local parsing
and analysis are reused when safe; dependent Story State and shifted evidence projections become
dirty. Parser/model/prompt/schema/configuration changes invalidate their actual consumers.
Executors are injected and never mutate source implicitly. See [incremental analysis](docs/incremental-analysis.md).
Phase 18 adds deterministic semantic revision reports: exact UTF-8 changed spans, changed
chapters, potential entity/story impact, and scoped analysis invalidation. Fully covered,
source-verified approved typo repairs are classified separately from semantic and unknown
changes. Offset shifts invalidate provenance even when chapter content is unchanged. Derived
analysis and dependent story snapshots can be invalidated atomically without altering imported
source. Phase 17 provides feature-branch contribution delivery, fast-forward remote synchronization,
remote commit tracking, generated draft PR descriptions, and an asynchronous GitHub REST
adapter with replaceable authentication and commit/check/PR inspection. Contributions verify
the reviewed local and pushed commit before publication. Git pushes use the user's configured
SSH credentials; API tokens are resolved at request time and never stored by the adapter.
Phase 16 provides local Git repository discovery, branch/HEAD/status snapshots, scoped diffs,
integrity-checked patch application, explicit commit metadata, and a fail-closed working-tree
safety gate. Every text patch consumes a Phase 15 Repair Gate authorization; unrelated changes
must be acknowledged path by path and are never included in the reviewed commit. The
human-controlled Repair Queue retains persistent review decisions, editable suggestions,
protected terms, and source context. The candidate-only Proofreading Engine validates
deterministic and semantic findings against exact immutable UTF-8 source spans; no detector can
mutate source text. The bounded Context Engine, schema v6, persistent Story
Memory, evidence-backed analysis, segmentation, inference snapshots, importers, and asynchronous
Qwen transport remain covered by deterministic tests.

## Requirements

- CMake 3.25 or newer
- Visual Studio 2022 with the C++ desktop workload
- Qt 6.5 or newer for MSVC 2022, including Qt Network, Qt PDF, Qt SQL, and Qt Test
  (CI pins Qt 6.8.3)

## Build and test

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

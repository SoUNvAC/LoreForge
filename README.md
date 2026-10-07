# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 17 adds feature-branch contribution delivery, fast-forward remote synchronization,
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

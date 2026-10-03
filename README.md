# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 15 adds a human-controlled Repair Queue with persistent review decisions, editable
suggestions, protected terms, source context, and an authorization gate that accepts only approved
candidates or explicit audited manual edits. The candidate-only Proofreading Engine validates
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

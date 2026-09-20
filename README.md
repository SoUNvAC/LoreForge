# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 12 adds persistent Story Memory. Validated chapter analyses are stored as structured
records and deterministically rebuilt into character memory, event memory, co-participation
relationships, a timeline, open threads, and content-addressed story-state snapshots. Snapshot
inputs come only from persisted records, never previous model chat messages. Schema v5,
Phase 11 evidence-backed analysis, Phase 10 segmentation, inference snapshots, importers, and
asynchronous Qwen transport remain covered by deterministic tests.

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

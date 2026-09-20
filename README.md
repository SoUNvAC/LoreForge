# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 11 adds chapter-local semantic analysis for characters, aliases, locations, events,
summaries, important facts, and open threads. Every direct claim requires source evidence;
claims without evidence must be explicitly marked inferred. Evidence contains only absolute
UTF-8 byte offsets, and LoreForge derives its text from the immutable chapter source. Phase 10
segmentation, Phase 9 inference snapshots, schema v4, the stored-data desktop workspace,
importers, and asynchronous Qwen transport remain covered by deterministic tests.

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

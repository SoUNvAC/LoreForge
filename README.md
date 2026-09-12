# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 10 adds dialogue/narration segmentation without allowing model output to rewrite source
text. The narrative contract accepts only segment type, speaker, confidence, and byte
boundaries; each segment's text is sliced from immutable UTF-8 source. Exact contiguous
coverage is mandatory, and golden annotated chapters measure coverage, boundary quality,
speaker correctness, and unknown-speaker handling. Phase 9 inference snapshots, schema v4,
the stored-data desktop workspace, importers, and asynchronous Qwen transport remain covered
by deterministic tests.

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

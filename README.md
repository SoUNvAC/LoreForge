# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 5 adds page-aware PDF import with geometric single/two-column reading order, visible
failure for ambiguous or OCR-only pages, manual correction hooks, extraction confidence,
and page-text provenance. EPUB 2/3, plain-text import, and SQLite persistence remain covered
by deterministic round-trip and golden-fixture tests. MOBI and narrative-analysis features
intentionally belong to later phases.

## Requirements

- CMake 3.25 or newer
- Visual Studio 2022 with the C++ desktop workload
- Qt 6.5 or newer for MSVC 2022, including Qt PDF, Qt SQL, and Qt Test (CI pins Qt 6.8.3)

## Build and test

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

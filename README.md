# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 4 adds guarded EPUB 2/3 import with OPF spine ordering, EPUB 3 and NCX navigation,
XHTML block extraction, multi-file chapter mapping, golden fixtures, and byte-level source
provenance. SQLite persistence and plain-text import remain covered by deterministic
round-trip and fixture tests. PDF and narrative-analysis features intentionally belong to
later phases.

## Requirements

- CMake 3.25 or newer
- Visual Studio 2022 with the C++ desktop workload
- Qt 6.5 or newer for MSVC 2022, including Qt SQL and Qt Test (CI pins Qt 6.8.3)

## Build and test

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

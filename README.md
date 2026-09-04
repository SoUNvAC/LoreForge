# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 3 adds migration-managed SQLite project persistence with typed storage errors,
transactional project/book/chapter repositories, deterministic document round trips,
rollback guarantees, and corruption detection. Plain-text import keeps original source
bytes immutable and every imported block retains its source byte span. EPUB and
narrative features intentionally belong to later phases.

## Requirements

- CMake 3.25 or newer
- Visual Studio 2022 with the C++ desktop workload
- Qt 6.5 or newer for MSVC 2022, including Qt SQL and Qt Test

## Build and test

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

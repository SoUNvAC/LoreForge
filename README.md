# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 9 makes model runs inspectable through immutable prompt versions, output-schema
versions, and context snapshots. The inference contract validates a strict, documented
subset of JSON Schema Draft 2020-12; SQLite schema v4 preserves the exact raw request,
raw response, parsed response, and validation result for each run. Credentials remain
outside the database, and narrative extraction is not yet implemented. The stored-data
desktop workspace, importers, and asynchronous Qwen transport remain covered by
deterministic tests.

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

# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 8 adds an asynchronous LLM transport boundary shared by mock and real clients.
The Qwen adapter provides a FIFO request queue, per-attempt timeouts, bounded retry,
explicit cancellation, structured JSON responses, and token usage. SQLite schema v3
persists LLM run status and transport metrics without storing credentials or implementing
narrative analysis. The stored-data desktop workspace and all importers remain covered by
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

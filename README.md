# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 6 adds MOBI7 import for unencrypted UTF-8 and Windows-1252 books using either
uncompressed or PalmDOC-compressed text records. The importer preserves decoded-text
provenance and fails visibly for encrypted, HUFF/CDIC-compressed, corrupt, and KF8-only
books. PDF, EPUB 2/3, plain-text import, and SQLite persistence remain covered by
deterministic round-trip and golden-fixture tests. Narrative-analysis features
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

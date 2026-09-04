# LoreForge

LoreForge is a C++20 and Qt 6 desktop application for maintaining, analyzing,
versioning, and proofreading community-maintained novels.

The project is developed one verified phase at a time. Its engineering contract is
maintained with the project planning materials.

## Current baseline

Phase 7 adds a stored-data desktop workspace with a Project Explorer, chapter tree,
reader, word counts, chapter metadata, status indicators, and source information.
The UI opens LoreForge SQLite project files and renders repository-loaded domain data;
loading and metrics stay outside the widgets. TXT, EPUB 2/3, PDF, and MOBI7 importers
remain independently covered by deterministic fixtures. Narrative-analysis features
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

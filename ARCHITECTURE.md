# Architecture

LoreForge follows a downward-only dependency direction:

```text
UI
 ↓
Pipeline / Use Cases
 ↓
Narrative / Proofreading / Context
 ↓
LLM / Git / Storage / Parser adapters
 ↓
Document + Core
```

Phase 0 contains only an application shell and the dependency-free project foundation.
Domain and document contracts begin in Phase 1.

Defects are repaired in the module that owns them. Upper layers must not compensate for
known lower-layer defects. Original imported text is immutable, and derived artifacts
must carry provenance.

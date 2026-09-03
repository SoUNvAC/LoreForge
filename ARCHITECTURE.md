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

The Phase 0 application shell depends on the core foundation. Phase 1 freezes the first
`core` and `document` contracts. Phase 2 adds deterministic `text` utilities and a
`parser` adapter that produces the Document model. The Widgets UI requests an import and
renders that domain result; it does not repair parser output or detect chapters itself.

Defects are repaired in the module that owns them. Upper layers must not compensate for
known lower-layer defects. Original imported text is immutable, and derived artifacts
must carry provenance.

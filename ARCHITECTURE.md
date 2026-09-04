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
Phase 3 adds a `storage` adapter beneath use-case/UI code. Its repositories validate
domain objects, enforce explicit ordering, and apply versioned SQLite migrations; upper
layers never compensate for stored-data defects.

The Phase 7 desktop UI opens an existing project database through a workspace loader.
Widgets render the resulting stored project, book, chapter, block, and provenance data.
Presentation metrics call the shared deterministic text utilities, so widgets do not
reimplement word-count or persistence rules.

Phase 8 introduces `ILLMClient` as the asynchronous model-transport boundary. `QwenClient`
implements that contract with Qt Network and owns request ordering, timeout, retry,
cancellation, response decoding, and token accounting. Neither the UI nor narrative code
depends on Qwen-specific HTTP details. Storage schema v3 records run lifecycle and metrics;
prompt/schema snapshots and raw payload persistence remain Phase 9 responsibilities.

Defects are repaired in the module that owns them. Upper layers must not compensate for
known lower-layer defects. Original imported text is immutable, and derived artifacts
must carry provenance.

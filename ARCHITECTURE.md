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
depends on Qwen-specific HTTP details. Storage schema v3 records run lifecycle and metrics.

Phase 9 adds the provider-independent `inference` contract below future narrative use cases.
It owns immutable prompt, output-schema, and context-snapshot types plus deterministic output
validation. The Qwen transport exposes the exact request and response bytes, while the
storage adapter owns their durable association with an LLM run in schema v4. This keeps HTTP,
validation, and persistence responsibilities separate while allowing a completed run to be
inspected from one stable snapshot.

Phase 10 introduces the `narrative` domain module. `DialogueExtractor` consumes structured
annotations, but never accepts generated segment text: it derives text exclusively from the
annotated UTF-8 source spans. It rejects gaps, overlaps, out-of-range offsets, character-splitting
boundaries, invalid confidence, and speaker attribution on narration. `ExtractionEvaluator`
keeps golden-fixture quality metrics deterministic and independent of model transport.

Defects are repaired in the module that owns them. Upper layers must not compensate for
known lower-layer defects. Original imported text is immutable, and derived artifacts
must carry provenance.

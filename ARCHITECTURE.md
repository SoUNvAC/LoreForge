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

Phase 11 extends `narrative` with `ChapterAnalyzer`, which converts validated structured output
into chapter-local characters, aliases, locations, events, a summary, important facts, and open
threads. Every claim carries confidence and an explicit evidence-or-inference basis. Evidence
text is reconstructed from immutable source byte spans, so model output cannot invent a quote.
This layer does not persist facts or resolve identities across chapters; those responsibilities
belong to the later Story Memory phase.

Phase 12 adds `StoryStateRebuilder` and `StoryStateRepository`. The rebuilder consumes only a
contiguous sequence of persisted `ChapterMemoryRecord` values and produces deterministic,
content-addressed character, event, co-participation relationship, timeline, and open-thread
memory. Exact canonical names merge across chapters; aliases resolve participants only within
their own chapter and never silently merge global identities. SQLite schema v5 stores both the
structured chapter inputs and verifiable snapshots, and invalidates dependent snapshots when an
input record changes. Chat history is not a memory source.

Phase 13 adds the provider-neutral `context` module between narrative state and model transport.
`ContextBuilder` renders mandatory project/task sections, ranks individual Story Memory entries,
and admits each only while the prompt remains inside `ContextBudget`. The deterministic token
estimate is planning data; provider usage remains authoritative. `ContextEngine` persists the
content-addressed `loreforge-context-v1` snapshot before reconstructing a production
`LLMRequest` from its stored ID. The UI can display the same validated snapshot through the
Context Inspector, while `ILLMClient` remains only a generic transport boundary.

Phase 14 adds the `proofreading` domain module. Deterministic detectors consume exact immutable
UTF-8 chapter projections and emit stable, source-hash-bound candidates for duplicated text,
spacing, punctuation, terminology, and name anomalies. `ProtectedTermRegistry` suppresses
deterministic suggestions over intentional language. `SemanticProofreader` defines and validates
structured model output, derives original text from source bytes, and rejects invalid boundaries
or protected-term edits. It does not call the transport directly: production semantic requests
still pass through the stored Context Engine gate. No Phase 14 API mutates source or creates a
patch; review and repair state belong to Phase 15.

Phase 15 adds the Repair Queue and `RepairGate`. Review decisions are durable and an edit can
cross the patch boundary only after a human approves a candidate or records an explicit manual
reason. Phase 16 adds the local `git` adapter. It invokes Git without a shell, exposes repository,
branch, HEAD, status, and diff state, and applies only text patches whose full-file hash and exact
UTF-8 byte span still match their authorization. Commit author identity is always supplied by the
caller. The commit gate fails closed when any unrelated working-tree path has not been explicitly
acknowledged, and `git commit --only` keeps acknowledged-but-unreviewed paths out of the commit.

Defects are repaired in the module that owns them. Upper layers must not compensate for
known lower-layer defects. Original imported text is immutable, and derived artifacts
must carry provenance.

Phase 17 extends the `git` adapter with contribution branches, fetch, fast-forward sync,
non-forced branch push, and ahead/behind commit tracking. `IGitHubAuthentication` and
`IGitHubClient` isolate credentials and GitHub operations from callers. `GitHubClient` uses
asynchronous Qt Network requests; PR creation first verifies that the remote branch SHA matches
the reviewed commit. PR preparation binds the description to the exact committed repair paths,
includes audit reasons and source hashes, and defaults to a draft. Network errors never expose
tokens or raw server diagnostics. No remote API changes canonical source or merges a PR.

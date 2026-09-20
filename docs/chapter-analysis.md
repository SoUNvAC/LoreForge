# Chapter semantic analysis

Phase 11 extracts structured, chapter-local facts without changing source text or creating
persistent story memory.

## Output contract

`ChapterAnalyzer::outputSchema()` accepts one chapter ID and these semantic groups:

- characters and their aliases;
- locations;
- events, with participant names and an optional location;
- one chapter summary;
- important facts;
- open threads.

Every semantic item is a claim with an `inferred` flag, a confidence from zero through one,
and an evidence array. Evidence entries contain only absolute `source_start` and `source_end`
UTF-8 byte offsets. Supplying evidence text is a schema violation.

## Grounding invariant

A direct claim (`inferred: false`) must contain at least one valid, non-empty evidence span.
An inferred claim (`inferred: true`) may have no evidence, but remains explicitly labeled as
inference. Evidence spans must be unique within a claim, stay inside the chapter source span,
and begin and end on complete UTF-8 character boundaries.

LoreForge derives every `SourceEvidence::text` value directly from the immutable source bytes.
The model therefore chooses source ranges, but cannot provide or alter quoted text.

## Validation boundary

The analyzer rejects schema violations, chapter mismatches, invalid source spans or UTF-8,
empty or untrimmed text, duplicate names within their local collection, invalid confidence,
and invalid or duplicate evidence. Any error prevents construction of a `ChapterAnalysis`.

All identities and references in this phase are chapter-local. Phase 12 Story Memory persists
these structured records and applies its separate deterministic cross-chapter rules.

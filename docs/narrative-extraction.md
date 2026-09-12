# Dialogue and narration extraction

Phase 10 classifies immutable source text. It does not rewrite the chapter, infer broader
chapter facts, or update story memory.

## Output contract

`DialogueExtractor::outputSchema()` accepts one chapter ID and an ordered segment array. Each
segment contains only:

- `type`: `dialogue` or `narration`;
- optional `speaker`: a trimmed name or `null` for unknown;
- absolute `source_start` and `source_end` UTF-8 byte offsets;
- `confidence`: a finite value from zero through one.

The schema deliberately rejects a `text` field. After validating the structured output,
LoreForge slices every `NarrativeSegment::text` directly from the supplied source bytes.
Narration cannot have a speaker; dialogue without a speaker remains explicitly unknown.

## Coverage invariant

Segments must be non-empty, ordered, non-overlapping, and contiguous across the complete
chapter source span. The first segment starts at the chapter's first byte and the final segment
ends at its last byte. Every boundary must also fall between complete UTF-8 characters.

Consequently, concatenating `ChapterSegmentation::reconstructedSource()` must reproduce the
exact original bytes. A gap is a failed extraction, not a partial success.

## Golden evaluation

The sanitized `tests/fixtures/dialogue_samples.json` chapter freezes attributed dialogue,
unknown dialogue, narration, confidence, and byte boundaries. `ExtractionEvaluator` reports:

- segment coverage: the fraction of expected source bytes covered at least once;
- boundary correctness: F1 over expected and actual internal byte boundaries;
- speaker correctness: exact attribution accuracy for expected dialogue spans;
- unknown-speaker handling: accuracy on dialogue whose expected speaker is unknown.

Results from different chapters or different chapter source spans are not comparable and
return zero scores. These deterministic metrics test the extraction contract; live-model
quality evaluation can reuse them later without changing the domain rules.

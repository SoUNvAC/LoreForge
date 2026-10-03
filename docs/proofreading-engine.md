# Proofreading Engine

Phase 14 detects possible source defects and returns review candidates. It has no API that edits
source bytes, writes patches, or changes canonical project data.

## Exact source contract

Each `ProofreadingSource` contains a chapter ID, an absolute half-open `SourceSpan`, the exact
UTF-8 bytes covered by that span, and the source hash. Detection is rejected when the byte count,
UTF-8 encoding, ID, span, or hash is invalid. Every candidate reconstructs `originalText` from
those bytes; character offsets are converted back to absolute UTF-8 byte offsets without
splitting a code point. A multi-chapter deterministic analysis accepts only chapters belonging
to the same source hash.

This explicit source projection is important for container formats. A caller must provide exact
chapter text bytes and may not pretend that rendered text offsets map directly onto unrelated
HTML or archive bytes.

## Deterministic checks

`DeterministicProofreader` currently emits candidates for:

- consecutive duplicate tokens and adjacent repeated lines;
- repeated horizontal whitespace, whitespace before punctuation, and likely missing ASCII
  punctuation spacing;
- repeated punctuation, damaged two-dot ellipses, and unmatched brackets;
- configured noncanonical terminology and case variants;
- rare one-edit spellings near a frequently observed configured name.

The report also records configured name frequencies. These checks are intentionally
conservative candidates, not proof that the source is wrong. Candidate IDs are content-derived,
results are sorted deterministically, and every candidate carries category, confidence,
evidence, semantic impact, detector version, origin, source hash, and exact source span.

## Protected terms

`ProtectedTermRegistry` supports canonical spellings, allowed variants, notes, and optional
chapter scope. Deterministic candidates that overlap a protected occurrence are suppressed.
Semantic candidates that overlap one are rejected. A misspelling near a protected canonical
term is not itself protected unless it was explicitly registered as an allowed variant.

## Semantic checks

`SemanticProofreader::outputSchema` defines the machine-verifiable JSON contract for an LLM
semantic pass. `extract` validates that contract, chapter ownership, confidence, evidence,
category, semantic-impact classification, absolute source bounds, UTF-8 boundaries, uniqueness,
and protected terms. It ignores any imagined replacement source and derives the original text
from immutable bytes.

Production orchestration must still use Phase 13's stored `ContextSnapshot` gate before sending
this schema to a model. Phase 14 only validates structured output and creates candidates. Human
review state, persistence, patch generation, and source mutation belong to later phases.

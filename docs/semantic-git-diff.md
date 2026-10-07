# Semantic Git Diff

Phase 18 understands revision impact through deterministic source comparison and explicit
review/analysis evidence. It does not ask an LLM to guess whether a textual change is harmless.

## Inputs and provenance

`GitRepository::readCommittedFile()` reads exact bytes from a resolved Git commit and validated
relative path, independently of working-tree edits. `SemanticDiffer::compare()` takes two
`SourceRevision` values with the same source ID, full-file SHA-256 hashes, UTF-8 bytes, and ordered
chapter projections. IDs and sequences must be unique, source spans must stay inside the file,
and boundaries must not split UTF-8 characters. Added/deleted chapters may exist on only one side.

Optional chapter analyses must match their chapter ID/span, satisfy the Story Memory domain
contract, and have evidence whose text exactly matches revision bytes. Reviews must be approved,
non-overlapping, and tied to the before revision's hash, chapter, and original byte span.

## Changed spans and classification

When approved reviews reconstruct the complete after revision, the report preserves each repair's
original span, relocated replacement span, candidate ID, and classification. Otherwise a bounded
line LCS identifies edit hunks and trims shared bytes at complete UTF-8 boundaries. Insertions and
deletions use zero-length spans on the unchanged side. If the line matrix would exceed one million
cells, a conservative changed envelope is returned with `conservativeSpanFallback` set.

The revision categories are:

- `NO_CHANGE`: bytes, chapter projections, and supplied semantic analysis content are unchanged.
- `TYPO_ONLY`: all changes are covered by approved typo candidates with text-only impact.
- `TEXT_ONLY`: fully reviewed formatting/punctuation or other explicitly text-only changes.
- `SEMANTIC_CHANGE`: known entity/dialogue/action/timeline/story changes, chapter identity/order
  changes, changed semantic analysis content, or changed directly grounded entity names.
- `UNKNOWN`: unreviewed, partly reviewed, ambiguous, or insufficiently mapped changes.

Name/terminology repairs cannot certify `TYPO_ONLY`. Direct entity evidence can override a
misclassified typo review. A mixed revision containing a known semantic change is semantic,
while individual span/chapter classifications preserve remaining uncertainty.

## Chapter, entity, and story impact

The report compares chapter content separately from byte locations. An unchanged chapter shifted
by an earlier insertion is excluded from content-change results but included in provenance and
analysis invalidation. Potentially affected entities are the union of known character names,
aliases, and locations in affected chapters; this is a conservative candidate list, not proof
that every listed entity changed. Missing before/after analyses set `entityImpactUncertain`.

Source-backed chapter analyses compare names/aliases, locations, events/participants, summaries,
facts, and open threads. Evidence offsets and confidence are not semantic content. Unknown changes
may affect Story State. Proven typo/text-only changes do not imply a new story meaning, but
hash-bound snapshots and source-backed analysis still become stale.

Unmapped edits or ambiguous chapter-boundary changes require chapter remapping and conservatively
invalidate the supplied source's chapter projections. The report does not claim that other books
or unrelated source parsing became dirty.

## Applying invalidation

Comparison is read-only. `AnalysisInvalidationPlan` returns affected chapter IDs and the earliest
sequence whose dependent Story State snapshots become stale. A caller can execute:

```cpp
memory.invalidateChapterRecords(projectId, plan.analysisChapters,
                                *plan.storyStateSnapshotsDirtyFrom);
```

Call only when a cutoff is present and after resolving any `requiresChapterRemap` result against
the canonical revision. Storage checks project/chapter ownership and cutoff consistency before
deleting any derived records, then removes those chapter analyses and later story snapshots in
one transaction. Earlier snapshots, unrelated chapter records, original document blocks, and
immutable imported metadata remain intact. Missing analysis prevents rebuilding a later snapshot
until the affected chapter has been analyzed again. No schema migration is required.

Dependency-graph persistence, dirty-state scheduling, automatic evidence rebasing, and selective
rebuild orchestration are Phase 19 work. This phase provides the explicit impact and invalidation
contract those operations will consume.

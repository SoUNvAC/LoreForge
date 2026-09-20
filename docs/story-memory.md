# Story Memory

Phase 12 creates persistent narrative state from stored structured records. It never treats a
previous LLM conversation or transient chat message as memory.

## Input boundary

Each `ChapterMemoryRecord` binds a validated `ChapterAnalysis` to a project and an explicit
zero-based chapter sequence. Before persistence, every claim and evidence span is revalidated.
Rebuilding requires an exact contiguous sequence from chapter 0 through chapter N, with no gaps,
duplicate chapters, or mixed projects.

The canonical JSON bytes of those ordered records produce a source hash. Changing any stored
chapter analysis changes that hash and invalidates snapshots at or after the changed chapter.

## Deterministic state

`StoryStateRebuilder` produces:

- `CharacterMemory`, merged only by Unicode-normalized, case-folded canonical names;
- `EventMemory`, with deterministic IDs derived from chapter and event position;
- `RelationshipMemory`, representing only participant co-occurrence in an event;
- a chapter-ordered timeline;
- open threads merged by normalized text;
- a content-addressed `StoryStateSnapshot`.

Aliases are collected into character memory and may resolve event participants within the same
chapter when unambiguous. They do not automatically merge characters across chapters. This avoids
turning pronouns or repeated nicknames into unsupported global identity claims.

Relationship memory is likewise structural: it records shared events and does not claim that two
characters are friends, enemies, relatives, or otherwise semantically related. Such labels require
future evidence-backed extraction.

Open threads remain open because Phase 11 does not emit a resolution operation. Repeated matching
threads accumulate chapter mentions and evidence without guessing that a differently worded fact
closes them.

## Snapshot verification

The state hash covers the deterministic memory payload. The snapshot ID covers the project,
chapter boundary, source hash, and state hash. Loading a stored snapshot rebuilds it from the
persisted chapter records and compares its ID, hashes, and canonical JSON bytes. A stale or altered
snapshot is rejected rather than repaired silently.

Context selection, relevance ranking, token budgets, and prompt construction belong to Phase 13.

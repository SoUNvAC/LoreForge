# Storage schema v5

Schema version 5 adds persistent Story Memory through migration
`005_story_memory.sql`.

## Tables

- `chapter_memory_records` stores one canonical, hashed `ChapterAnalysis` JSON document for each
  explicit project chapter sequence. Project/sequence and project/chapter uniqueness prevent
  ambiguous ordering or duplicate chapter inputs.
- `story_state_snapshots` stores the deterministic snapshot ID, source hash, state hash, and
  canonical snapshot JSON for a project through chapter N.

Both tables cascade with their owning project. Chapter-memory records also reference stored
chapters, so removing a chapter removes its structured memory input.

`StoryStateRepository::saveChapterRecord()` invalidates snapshots at or after the updated chapter
inside the same transaction. `loadSnapshot()` independently rebuilds state from the ordered input
records and requires the stored metadata and bytes to match exactly.

Migration 005 is transactional, recorded in `schema_migrations`, and mirrored as SQLite
`user_version = 5`. Opening a project verifies both new table layouts in addition to the existing
schema and integrity checks.

# Storage schema v6

Schema version 6 adds the persistent Repair Queue through migration `006_repair_queue.sql`.

## Tables

- `proofreading_candidates` stores the complete immutable detector result, source provenance,
  review context, editable current suggestion, lifecycle status, and timestamps. Candidates belong
  to both a project and one of that project's stored chapters.
- `protected_terms` stores project-wide or chapter-scoped spellings that proofreading must not
  change. A partial unique index also enforces uniqueness for project-wide terms, where
  `chapter_scope` is null.

`RepairQueueRepository` verifies project/chapter ownership before enqueueing. Decisions use the
previous status as an optimistic concurrency guard. Ignore-and-protect inserts the protected term
and rejects the candidate in one transaction, so neither half can be committed alone.

Migration 006 is transactional, recorded in `schema_migrations`, and mirrored as SQLite
`user_version = 6`. Database opening verifies both table layouts and all foreign keys.

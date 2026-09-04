# Storage schema v2

Schema version 2 extends the Phase 3 project database without changing its ownership or
transaction boundaries.

Migration `002_extraction_confidence.sql` adds nullable `blocks.extraction_confidence`. When
present, the value must be finite and between `0.0` and `1.0`. Existing version 1 rows migrate to
`NULL`, preserving their meaning and all prior document content hashes.

Repository writes and reads preserve the optional value. Domain validation, JSON decoding, and
database loading independently reject an out-of-range confidence. The migration is transactional,
recorded in `schema_migrations`, and mirrored as SQLite `user_version = 2`.

All tables, foreign keys, deterministic ordering rules, and rollback guarantees from
`storage-schema-v1.md` remain unchanged.

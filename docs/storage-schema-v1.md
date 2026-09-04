# Storage schema v1

Phase 3 stores each LoreForge project in one SQLite database. Opening a project always
enables foreign-key enforcement and applies pending migrations before repositories are
made available.

## Migration contract

- Applied versions are recorded in `schema_migrations` and mirrored in SQLite's
  `user_version`.
- Migration `001_initial.sql` creates schema version 1 inside a transaction.
- A migration failure rolls back all statements in that migration.
- A database newer than the application is rejected without modification.
- Future destructive migrations must create a backup before changing data.

## Tables

- `projects` stores stable project IDs, names, and UTC creation timestamps.
- `books` stores normalized book metadata, source locators, and SHA-256 source hashes.
- `chapters` stores stable chapter IDs and a unique zero-based order within each book.
- `blocks` stores ordered normalized blocks and half-open byte spans into immutable source
  files.

Books reference projects, chapters reference books, and blocks reference chapters. All
relationships use foreign keys with cascading deletion. Repository reads always order
chapters and blocks explicitly; the UI must not repair database ordering.

## Transaction contract

Saving a `Document` writes its book metadata, chapters, and blocks in one transaction.
An error result or exception rolls the entire operation back. Documents are validated
before writes and again after reads. Malformed identifiers, hashes, ordering, block
types, JSON author data, or source spans are returned as typed corruption errors.

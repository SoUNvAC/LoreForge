# Storage schema v3

Schema version 3 adds transport-level LLM run history through migration
`003_llm_runs.sql`.

The `llm_runs` table stores:

- stable run, project, and transport request identities;
- provider, model, and lifecycle status;
- attempt and token counts;
- UTC start/completion timestamps and elapsed milliseconds;
- terminal transport error code and message.

Run rows reference projects with cascading deletion and are listed deterministically by
start time and stable ID. `LLMRunRepository` supports lifecycle updates through an upsert and
rejects invalid identifiers, negative metrics, invalid timestamps, and inconsistent terminal
state.

Credentials, prompt content, raw request/response payloads, parsed narrative data, and schema
versions are not part of v3. Migration application remains transactional, is recorded in
`schema_migrations`, and is mirrored as SQLite `user_version = 3`.

Schema v4 extends this run history with immutable inference inputs and response artifacts;
see `storage-schema-v4.md`.

# Storage schema v4

Schema version 4 adds reproducible inference artifacts through migration
`004_inference_artifacts.sql`.

The new tables are:

- `prompt_templates`: stable prompt identities and names;
- `prompt_versions`: immutable numbered text versions with content hashes;
- `output_schemas`: immutable numbered JSON schemas with content hashes;
- `context_snapshots`: immutable project-owned context JSON with content hashes;
- `llm_run_artifacts`: the exact request/response bytes, parsed JSON, validation result, and
  references to the prompt, schema, context, and transport run.

Prompt and schema versions use composite identities. Context snapshots have stable unique
identities and must belong to the same project as their LLM run. Artifact rows begin in the
`pending` state and are finalized only once. Foreign keys prevent referenced immutable inputs
from being removed while a run still uses them; deleting a project still cascades through its
run history and snapshots.

Migration application is transactional, recorded as `004_inference_artifacts` in
`schema_migrations`, and mirrored as SQLite `user_version = 4`. Opening a project verifies all
required v4 tables and the complete `llm_run_artifacts` column set.

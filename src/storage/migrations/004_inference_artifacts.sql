CREATE TABLE prompt_templates (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT NOT NULL
);

CREATE TABLE prompt_versions (
    template_id TEXT NOT NULL,
    version INTEGER NOT NULL CHECK (version > 0),
    template_text TEXT NOT NULL,
    content_hash TEXT NOT NULL,
    created_at TEXT NOT NULL,
    PRIMARY KEY (template_id, version),
    FOREIGN KEY (template_id) REFERENCES prompt_templates(id) ON DELETE RESTRICT
);

CREATE TABLE output_schemas (
    id TEXT NOT NULL,
    version INTEGER NOT NULL CHECK (version > 0),
    name TEXT NOT NULL,
    schema_json TEXT NOT NULL,
    content_hash TEXT NOT NULL,
    created_at TEXT NOT NULL,
    PRIMARY KEY (id, version)
);

CREATE TABLE context_snapshots (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL,
    content_json TEXT NOT NULL,
    content_hash TEXT NOT NULL,
    created_at TEXT NOT NULL,
    FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE
);

CREATE TABLE llm_run_artifacts (
    run_id TEXT PRIMARY KEY NOT NULL,
    prompt_template_id TEXT NOT NULL,
    prompt_version INTEGER NOT NULL,
    output_schema_id TEXT NOT NULL,
    output_schema_version INTEGER NOT NULL,
    context_snapshot_id TEXT NOT NULL,
    raw_request BLOB NOT NULL,
    raw_response BLOB,
    parsed_response_json TEXT,
    validation_status TEXT NOT NULL CHECK (validation_status IN ('pending', 'valid', 'invalid', 'unavailable')),
    validation_errors_json TEXT NOT NULL,
    FOREIGN KEY (run_id) REFERENCES llm_runs(id) ON DELETE CASCADE,
    FOREIGN KEY (prompt_template_id, prompt_version) REFERENCES prompt_versions(template_id, version) ON DELETE RESTRICT,
    FOREIGN KEY (output_schema_id, output_schema_version) REFERENCES output_schemas(id, version) ON DELETE RESTRICT,
    FOREIGN KEY (context_snapshot_id) REFERENCES context_snapshots(id) ON DELETE RESTRICT
);

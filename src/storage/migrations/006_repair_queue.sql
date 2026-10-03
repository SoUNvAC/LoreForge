CREATE TABLE proofreading_candidates (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL,
    chapter_id TEXT NOT NULL,
    source_id TEXT NOT NULL,
    start_byte INTEGER NOT NULL CHECK (start_byte >= 0),
    end_byte INTEGER NOT NULL CHECK (end_byte >= start_byte),
    original_text TEXT NOT NULL,
    detected_suggestion TEXT NOT NULL,
    current_suggestion TEXT NOT NULL,
    category TEXT NOT NULL,
    confidence REAL NOT NULL CHECK (confidence >= 0.0 AND confidence <= 1.0),
    evidence TEXT NOT NULL,
    source_context TEXT NOT NULL,
    semantic_impact TEXT NOT NULL,
    origin TEXT NOT NULL CHECK (origin IN ('DETERMINISTIC', 'SEMANTIC')),
    status TEXT NOT NULL CHECK (status IN (
        'DETECTED', 'REVIEW_REQUIRED', 'APPROVED', 'REJECTED',
        'PATCHED', 'SUBMITTED', 'MERGED', 'STALE'
    )),
    detector_version TEXT NOT NULL,
    source_hash TEXT NOT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE,
    FOREIGN KEY (chapter_id) REFERENCES chapters(id) ON DELETE CASCADE
);

CREATE INDEX proofreading_candidates_queue
    ON proofreading_candidates (project_id, status, created_at, id);

CREATE TABLE protected_terms (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL,
    canonical_spelling TEXT NOT NULL,
    allowed_variants_json TEXT NOT NULL,
    notes TEXT NOT NULL,
    chapter_scope TEXT,
    created_at TEXT NOT NULL,
    FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE,
    FOREIGN KEY (chapter_scope) REFERENCES chapters(id) ON DELETE CASCADE,
    UNIQUE (project_id, canonical_spelling, chapter_scope)
);

CREATE UNIQUE INDEX protected_terms_project_wide
    ON protected_terms (project_id, canonical_spelling)
    WHERE chapter_scope IS NULL;

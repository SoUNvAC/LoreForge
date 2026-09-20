CREATE TABLE chapter_memory_records (
    project_id TEXT NOT NULL,
    chapter_sequence INTEGER NOT NULL CHECK (chapter_sequence >= 0),
    chapter_id TEXT NOT NULL,
    analysis_json TEXT NOT NULL,
    analysis_hash TEXT NOT NULL,
    PRIMARY KEY (project_id, chapter_sequence),
    UNIQUE (project_id, chapter_id),
    FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE,
    FOREIGN KEY (chapter_id) REFERENCES chapters(id) ON DELETE CASCADE
);

CREATE TABLE story_state_snapshots (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL,
    through_chapter_sequence INTEGER NOT NULL CHECK (through_chapter_sequence >= 0),
    source_hash TEXT NOT NULL,
    state_hash TEXT NOT NULL,
    state_json TEXT NOT NULL,
    UNIQUE (project_id, through_chapter_sequence),
    FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE
);

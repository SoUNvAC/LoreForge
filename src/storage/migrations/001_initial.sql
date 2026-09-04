CREATE TABLE projects (
    id TEXT PRIMARY KEY NOT NULL,
    name TEXT NOT NULL,
    created_at TEXT NOT NULL
);

CREATE TABLE books (
    id TEXT PRIMARY KEY NOT NULL,
    project_id TEXT NOT NULL,
    title TEXT NOT NULL,
    authors_json TEXT NOT NULL,
    language TEXT NOT NULL,
    source_format TEXT NOT NULL,
    source_locator TEXT NOT NULL,
    source_hash TEXT NOT NULL,
    FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE,
    UNIQUE (project_id, source_locator)
);

CREATE TABLE chapters (
    id TEXT PRIMARY KEY NOT NULL,
    book_id TEXT NOT NULL,
    chapter_index INTEGER NOT NULL CHECK (chapter_index >= 0),
    title TEXT NOT NULL,
    FOREIGN KEY (book_id) REFERENCES books(id) ON DELETE CASCADE,
    UNIQUE (book_id, chapter_index)
);

CREATE TABLE blocks (
    chapter_id TEXT NOT NULL,
    block_index INTEGER NOT NULL CHECK (block_index >= 0),
    block_type TEXT NOT NULL CHECK (block_type IN ('paragraph', 'heading', 'scene_break', 'unknown')),
    text TEXT NOT NULL,
    source_id TEXT NOT NULL,
    start_byte INTEGER NOT NULL CHECK (start_byte >= 0),
    end_byte INTEGER NOT NULL CHECK (end_byte >= start_byte),
    PRIMARY KEY (chapter_id, block_index),
    FOREIGN KEY (chapter_id) REFERENCES chapters(id) ON DELETE CASCADE
);

CREATE INDEX blocks_source_span ON blocks (source_id, start_byte, end_byte);

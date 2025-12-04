-- =============================================================================
-- Documentation System Schema
-- =============================================================================
-- A unified schema for tutorials, command reference, guides, and FAQs.
-- Designed for:
--   - Fast lookups by slug, category, type
--   - Full-text search across all content
--   - Linking commands to tutorials
--   - Runnable code examples with expected outputs
--
-- Usage:
--   sqlite3 docs.db < schema.sql
--   OR from Tcl: source the init script
-- =============================================================================

PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;

-- =============================================================================
-- Categories
-- =============================================================================
-- Hierarchical organization for all content types.
-- Examples: "basics", "dlsh", "dg", "gpio", "getting-started"

CREATE TABLE IF NOT EXISTS categories (
    id              INTEGER PRIMARY KEY,
    slug            TEXT UNIQUE NOT NULL,       -- URL-friendly: "dlsh-lists"
    name            TEXT NOT NULL,              -- Display: "DLSH Lists"
    description     TEXT DEFAULT '',
    parent_id       INTEGER REFERENCES categories(id) ON DELETE SET NULL,
    sort_order      INTEGER DEFAULT 0,
    icon            TEXT DEFAULT '',            -- Optional: emoji or icon class
    created_at      TEXT DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_categories_parent ON categories(parent_id);
CREATE INDEX IF NOT EXISTS idx_categories_sort ON categories(sort_order);


-- =============================================================================
-- Entries (Unified Content)
-- =============================================================================
-- All documentation lives here: tutorials, commands, guides, FAQs.
-- The entry_type determines how it's displayed and what fields matter.

CREATE TABLE IF NOT EXISTS entries (
    id              INTEGER PRIMARY KEY,
    slug            TEXT UNIQUE NOT NULL,       -- URL-friendly identifier
    entry_type      TEXT NOT NULL,              -- 'command', 'tutorial', 'guide', 'faq', 'example'
    category_id     INTEGER REFERENCES categories(id) ON DELETE SET NULL,
    
    -- Common fields
    title           TEXT NOT NULL,              -- Display title
    summary         TEXT DEFAULT '',            -- Short description (1-2 sentences)
    content         TEXT DEFAULT '',            -- Main content (markdown supported)
    
    -- Command-specific fields (entry_type = 'command')
    namespace       TEXT DEFAULT '',            -- e.g., 'dl', 'dg', 'ess'
    syntax          TEXT DEFAULT '',            -- e.g., 'dl_fromto start end'
    return_type     TEXT DEFAULT '',            -- e.g., 'dlsh_list', 'int', 'string'
    see_also        TEXT DEFAULT '',            -- Comma-separated related commands
    
    -- Tutorial-specific fields
    difficulty      TEXT DEFAULT 'beginner',    -- 'beginner', 'intermediate', 'advanced'
    estimated_time  INTEGER DEFAULT 0,          -- Minutes to complete
    
    -- Status and metadata
    stability       TEXT DEFAULT 'stable',      -- 'stable', 'experimental', 'deprecated', 'undocumented'
    deprecated      INTEGER DEFAULT 0,          -- Boolean: is this deprecated?
    deprecated_msg  TEXT DEFAULT '',            -- Why deprecated, what to use instead
    published       INTEGER DEFAULT 1,          -- Boolean: visible to users?
    sort_order      INTEGER DEFAULT 0,
    
    -- Timestamps
    created_at      TEXT DEFAULT CURRENT_TIMESTAMP,
    updated_at      TEXT DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_entries_type ON entries(entry_type);
CREATE INDEX IF NOT EXISTS idx_entries_category ON entries(category_id);
CREATE INDEX IF NOT EXISTS idx_entries_namespace ON entries(namespace);
CREATE INDEX IF NOT EXISTS idx_entries_published ON entries(published);
CREATE INDEX IF NOT EXISTS idx_entries_sort ON entries(category_id, sort_order);


-- =============================================================================
-- Code Examples
-- =============================================================================
-- Runnable code blocks associated with any entry.
-- Commands might have usage examples; tutorials have starter code + solutions.

CREATE TABLE IF NOT EXISTS examples (
    id              INTEGER PRIMARY KEY,
    entry_id        INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
    
    title           TEXT DEFAULT '',            -- Optional label: "Basic usage", "With options"
    description     TEXT DEFAULT '',            -- What this example demonstrates
    code            TEXT NOT NULL,              -- The actual code
    expected_output TEXT DEFAULT '',            -- What running it should produce
    output_type     TEXT DEFAULT 'text',        -- 'text', 'dg', 'image', 'none'
    
    -- For tutorials: distinguish starter from solution
    example_type    TEXT DEFAULT 'example',     -- 'example', 'starter', 'solution'
    
    -- Execution context
    setup_code      TEXT DEFAULT '',            -- Code to run before (hidden from user)
    teardown_code   TEXT DEFAULT '',            -- Code to run after (cleanup)
    
    sort_order      INTEGER DEFAULT 0,
    created_at      TEXT DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_examples_entry ON examples(entry_id);
CREATE INDEX IF NOT EXISTS idx_examples_type ON examples(example_type);


-- =============================================================================
-- Parameters (for commands)
-- =============================================================================
-- Detailed parameter documentation for command entries.

CREATE TABLE IF NOT EXISTS parameters (
    id              INTEGER PRIMARY KEY,
    entry_id        INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
    
    name            TEXT NOT NULL,
    param_type      TEXT DEFAULT '',            -- 'int', 'string', 'list', 'dlsh_list', etc.
    description     TEXT DEFAULT '',
    is_optional     INTEGER DEFAULT 0,
    default_value   TEXT DEFAULT '',
    
    sort_order      INTEGER DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_parameters_entry ON parameters(entry_id);


-- =============================================================================
-- Hints (for tutorials)
-- =============================================================================
-- Progressive hints for tutorial exercises.

CREATE TABLE IF NOT EXISTS hints (
    id              INTEGER PRIMARY KEY,
    entry_id        INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
    
    hint_text       TEXT NOT NULL,
    sort_order      INTEGER DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_hints_entry ON hints(entry_id);


-- =============================================================================
-- Tags
-- =============================================================================
-- Cross-cutting concerns that span categories.
-- Examples: "memory-management", "real-time", "gpio", "beginner-friendly"

CREATE TABLE IF NOT EXISTS tags (
    id              INTEGER PRIMARY KEY,
    name            TEXT UNIQUE NOT NULL,
    description     TEXT DEFAULT ''
);

CREATE TABLE IF NOT EXISTS entry_tags (
    entry_id        INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
    tag_id          INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
    PRIMARY KEY (entry_id, tag_id)
);

CREATE INDEX IF NOT EXISTS idx_entry_tags_tag ON entry_tags(tag_id);


-- =============================================================================
-- Entry Links
-- =============================================================================
-- Explicit relationships between entries.
-- Examples: command → tutorial that uses it, guide → related commands

CREATE TABLE IF NOT EXISTS entry_links (
    from_entry_id   INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
    to_entry_id     INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
    link_type       TEXT DEFAULT 'related',     -- 'related', 'teaches', 'uses', 'prerequisite'
    PRIMARY KEY (from_entry_id, to_entry_id)
);

CREATE INDEX IF NOT EXISTS idx_entry_links_to ON entry_links(to_entry_id);


-- =============================================================================
-- Full-Text Search
-- =============================================================================
-- FTS5 virtual tables for fast searching.

CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5(
    title,
    summary,
    content,
    namespace,
    content='entries',
    content_rowid='id',
    tokenize='porter unicode61'
);

-- Triggers to keep FTS in sync
CREATE TRIGGER IF NOT EXISTS entries_ai AFTER INSERT ON entries BEGIN
    INSERT INTO entries_fts(rowid, title, summary, content, namespace)
    VALUES (new.id, new.title, new.summary, new.content, new.namespace);
END;

CREATE TRIGGER IF NOT EXISTS entries_ad AFTER DELETE ON entries BEGIN
    INSERT INTO entries_fts(entries_fts, rowid, title, summary, content, namespace)
    VALUES ('delete', old.id, old.title, old.summary, old.content, old.namespace);
END;

CREATE TRIGGER IF NOT EXISTS entries_au AFTER UPDATE ON entries BEGIN
    INSERT INTO entries_fts(entries_fts, rowid, title, summary, content, namespace)
    VALUES ('delete', old.id, old.title, old.summary, old.content, old.namespace);
    INSERT INTO entries_fts(rowid, title, summary, content, namespace)
    VALUES (new.id, new.title, new.summary, new.content, new.namespace);
END;


-- =============================================================================
-- Auto-update timestamps
-- =============================================================================

CREATE TRIGGER IF NOT EXISTS entries_update_timestamp 
AFTER UPDATE ON entries
BEGIN
    UPDATE entries SET updated_at = CURRENT_TIMESTAMP WHERE id = new.id;
END;


-- =============================================================================
-- Views for common queries
-- =============================================================================

-- Commands with their category info
CREATE VIEW IF NOT EXISTS v_commands AS
SELECT 
    e.id,
    e.slug,
    e.title,
    e.summary,
    e.namespace,
    e.syntax,
    e.return_type,
    e.stability,
    e.deprecated,
    e.see_also,
    c.slug as category_slug,
    c.name as category_name
FROM entries e
LEFT JOIN categories c ON e.category_id = c.id
WHERE e.entry_type = 'command' AND e.published = 1;

-- Tutorials with category and difficulty
CREATE VIEW IF NOT EXISTS v_tutorials AS
SELECT 
    e.id,
    e.slug,
    e.title,
    e.summary,
    e.difficulty,
    e.estimated_time,
    c.slug as category_slug,
    c.name as category_name,
    e.sort_order
FROM entries e
LEFT JOIN categories c ON e.category_id = c.id
WHERE e.entry_type = 'tutorial' AND e.published = 1
ORDER BY c.sort_order, e.sort_order;

-- Entry with example count (useful for listings)
CREATE VIEW IF NOT EXISTS v_entries_summary AS
SELECT 
    e.id,
    e.slug,
    e.entry_type,
    e.title,
    e.summary,
    e.namespace,
    e.difficulty,
    e.published,
    c.slug as category_slug,
    c.name as category_name,
    (SELECT COUNT(*) FROM examples WHERE entry_id = e.id) as example_count,
    (SELECT COUNT(*) FROM hints WHERE entry_id = e.id) as hint_count
FROM entries e
LEFT JOIN categories c ON e.category_id = c.id;

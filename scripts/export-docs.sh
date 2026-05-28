#!/usr/bin/env bash
# Dump db/docs.db to db/docs.sql for reviewable diffs.
#
# The dump contains ONLY the real tables (schema + data). The FTS5 index
# (entries_fts + its shadow tables) is NOT dumped — its blob segments are
# large and churn opaquely, which defeats the point of a reviewable text
# source. Instead a small footer recreates the FTS virtual table, its
# triggers, and runs a 'rebuild' so the index is regenerated from the
# entries content when the .db is built from this .sql.
#
# Run from the repo root after authoring changes, then commit db/docs.sql.
# The binary db/docs.db is a build artifact (gitignored); CMake builds it
# from this .sql at configure/build time.
#
# Usage:
#   scripts/export-docs.sh                 # writes db/docs.sql
#   scripts/export-docs.sh path/to/out.sql # custom output path
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB="${REPO_ROOT}/db/docs.db"
OUT="${1:-${REPO_ROOT}/db/docs.sql}"

if [[ ! -f "$DB" ]]; then
    echo "error: $DB not found" >&2
    exit 1
fi
if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "error: sqlite3 not on PATH" >&2
    exit 1
fi

# Discover real tables (exclude the FTS virtual table, its shadow tables,
# and sqlite internals). Dumping by explicit table name also omits triggers,
# which we recreate in the footer.
tables=$(sqlite3 "$DB" \
    "SELECT name FROM sqlite_master
      WHERE type='table'
        AND name NOT LIKE 'entries_fts%'
        AND name NOT LIKE 'sqlite_%'
      ORDER BY name;")

# shellcheck disable=SC2086
sqlite3 "$DB" ".dump ${tables//$'\n'/ }" > "$OUT"

# Append the FTS recreate + rebuild footer.
cat >> "$OUT" <<'EOF'

-- ---------------------------------------------------------------------------
-- Full-text search index (regenerated from entries content; not dumped so
-- that db/docs.sql stays a clean, reviewable text source).
-- ---------------------------------------------------------------------------
CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5(
    title, summary, content, namespace,
    content='entries', content_rowid='id',
    tokenize='porter unicode61'
);
CREATE TRIGGER entries_ai AFTER INSERT ON entries BEGIN
    INSERT INTO entries_fts(rowid, title, summary, content, namespace)
    VALUES (new.id, new.title, new.summary, new.content, new.namespace);
END;
CREATE TRIGGER entries_ad AFTER DELETE ON entries BEGIN
    INSERT INTO entries_fts(entries_fts, rowid, title, summary, content, namespace)
    VALUES ('delete', old.id, old.title, old.summary, old.content, old.namespace);
END;
CREATE TRIGGER entries_au AFTER UPDATE ON entries BEGIN
    INSERT INTO entries_fts(entries_fts, rowid, title, summary, content, namespace)
    VALUES ('delete', old.id, old.title, old.summary, old.content, old.namespace);
    INSERT INTO entries_fts(rowid, title, summary, content, namespace)
    VALUES (new.id, new.title, new.summary, new.content, new.namespace);
END;
CREATE TRIGGER entries_update_timestamp AFTER UPDATE ON entries BEGIN
    UPDATE entries SET updated_at = CURRENT_TIMESTAMP WHERE id = new.id;
END;
INSERT INTO entries_fts(entries_fts) VALUES('rebuild');
EOF

bytes=$(wc -c < "$OUT" | tr -d ' ')
echo "wrote $OUT ($bytes bytes, FTS rebuilt on load)"

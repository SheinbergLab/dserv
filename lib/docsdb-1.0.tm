# docsdb-1.0.tm
# =============================================================================
# Documentation Database Module
# =============================================================================
# Provides database initialization, seeding, and content management.
#
# First-time setup (run once):
#   package require docsdb
#   docsdb::init "/usr/local/dserv/db/docs.db"
#   docsdb::setup   ;# Creates schema, seeds categories/tags
#
# Normal usage (in subprocess):
#   package require docsdb
#   docsdb::init "/usr/local/dserv/db/docs.db"
#   # Database ready - use with docsapi
#
# Command ingestion (run from live dserv with commands loaded):
#   docsdb::ingest_commands "dl_*" "dl"
#   docsdb::ingest_commands "dg_*" "dg"
#
# =============================================================================

package require sqlite3

namespace eval ::docsdb {
    variable db ""
    variable db_path ""
    variable readonly 0
    variable version "1.0"
    variable setup_complete 0
}

# =============================================================================
# Database Initialization
# =============================================================================

proc ::docsdb::init {path {mode "readwrite"}} {
    variable db
    variable db_path
    variable readonly
    
    # Close existing connection if any
    if {$db ne ""} {
        catch {$db close}
    }
    
    set db_path $path
    set readonly [expr {$mode eq "readonly"}]
    
    # Create sqlite3 handle in this namespace
    if {$readonly} {
        # Open read-only - no risk of corruption, no shutdown concerns
        sqlite3 ::docsdb::db $path -readonly true
    } else {
        sqlite3 ::docsdb::db $path
    }
    set db ::docsdb::db
    
    # Enable foreign keys
    $db eval {PRAGMA foreign_keys = ON}
    
    if {!$readonly} {
        # Use DELETE journal mode (not WAL) for read-write
        $db eval {PRAGMA journal_mode = DELETE}
        # Create schema if needed
        create_schema
    }
    
    return $db
}

proc ::docsdb::close {} {
    variable db
    if {$db ne ""} {
        # Checkpoint WAL to main database before closing
        catch {$db eval {PRAGMA wal_checkpoint(TRUNCATE)}}
        catch {$db close}
        set db ""
    }
}

proc ::docsdb::checkpoint {} {
    # Force WAL checkpoint - call periodically or before expected shutdown
    variable db
    if {$db ne ""} {
        $db eval {PRAGMA wal_checkpoint(TRUNCATE)}
    }
}

# =============================================================================
# First-Time Setup (call once to initialize a new database)
# =============================================================================

proc ::docsdb::setup {} {
    # Complete first-time setup: schema + seed data
    # Safe to call multiple times - uses IF NOT EXISTS throughout
    
    variable setup_complete
    
    puts "Setting up documentation database..."
    
    create_schema
    puts "  Schema created"
    
    seed_categories
    puts "  Categories seeded"
    
    seed_tags
    puts "  Tags seeded"
    
    set setup_complete 1
    
    # Show stats
    set s [stats]
    puts "Setup complete:"
    dict for {key val} $s {
        puts "  $key: $val"
    }
    
    return 1
}

proc ::docsdb::is_setup {} {
    # Check if database has been fully set up (all tables exist and have seed data)
    variable db
    
    if {$db eq ""} {
        return 0
    }
    
    # Required tables
    set required_tables {categories entries examples parameters hints tags entry_tags entry_links}
    
    foreach tbl $required_tables {
        if {[catch {
            set exists [$db eval "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='$tbl'"]
        }]} {
            return 0
        }
        if {$exists == 0} {
            return 0
        }
    }
    
    # Also check that categories have been seeded
    set cat_count [$db eval {SELECT COUNT(*) FROM categories}]
    return [expr {$cat_count > 0}]
}

proc ::docsdb::check_db {} {
    # Diagnostic: check database integrity and report status
    variable db
    
    set result [dict create ok 1 issues {}]
    
    # Check required tables
    set required_tables {categories entries examples parameters hints tags entry_tags entry_links}
    set missing {}
    
    foreach tbl $required_tables {
        set exists [$db eval "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='$tbl'"]
        if {$exists == 0} {
            lappend missing $tbl
        }
    }
    
    if {[llength $missing] > 0} {
        dict set result ok 0
        dict lappend result issues "Missing tables: [join $missing {, }]"
    }
    
    # Check FTS table
    set fts_exists [$db eval {SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='entries_fts'}]
    if {$fts_exists == 0} {
        dict lappend result issues "FTS index missing"
    }
    
    # Check for seed data
    catch {
        set cat_count [$db eval {SELECT COUNT(*) FROM categories}]
        if {$cat_count == 0} {
            dict lappend result issues "No categories (run docsdb::setup)"
        }
        dict set result categories $cat_count
    }
    
    # Basic integrity check
    catch {
        set integrity [$db eval {PRAGMA integrity_check}]
        if {$integrity ne "ok"} {
            dict set result ok 0
            dict lappend result issues "Integrity check failed: $integrity"
        }
    }
    
    return $result
}

proc ::docsdb::create_schema {} {
    variable db
    
    # Categories
    $db eval "
        CREATE TABLE IF NOT EXISTS categories (
            id              INTEGER PRIMARY KEY,
            slug            TEXT UNIQUE NOT NULL,
            name            TEXT NOT NULL,
            description     TEXT DEFAULT '',
            parent_id       INTEGER REFERENCES categories(id) ON DELETE SET NULL,
            sort_order      INTEGER DEFAULT 0,
            icon            TEXT DEFAULT '',
            created_at      TEXT DEFAULT CURRENT_TIMESTAMP
        )
    "
    
    # Entries (unified content)
    $db eval "
        CREATE TABLE IF NOT EXISTS entries (
            id              INTEGER PRIMARY KEY,
            slug            TEXT UNIQUE NOT NULL,
            entry_type      TEXT NOT NULL,
            category_id     INTEGER REFERENCES categories(id) ON DELETE SET NULL,
            title           TEXT NOT NULL,
            summary         TEXT DEFAULT '',
            content         TEXT DEFAULT '',
            namespace       TEXT DEFAULT '',
            syntax          TEXT DEFAULT '',
            return_type     TEXT DEFAULT '',
            see_also        TEXT DEFAULT '',
            difficulty      TEXT DEFAULT 'beginner',
            estimated_time  INTEGER DEFAULT 0,
            stability       TEXT DEFAULT 'stable',
            deprecated      INTEGER DEFAULT 0,
            deprecated_msg  TEXT DEFAULT '',
            published       INTEGER DEFAULT 1,
            sort_order      INTEGER DEFAULT 0,
            created_at      TEXT DEFAULT CURRENT_TIMESTAMP,
            updated_at      TEXT DEFAULT CURRENT_TIMESTAMP
        )
    "
    
    # Examples
    $db eval "
        CREATE TABLE IF NOT EXISTS examples (
            id              INTEGER PRIMARY KEY,
            entry_id        INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
            title           TEXT DEFAULT '',
            description     TEXT DEFAULT '',
            code            TEXT NOT NULL,
            expected_output TEXT DEFAULT '',
            output_type     TEXT DEFAULT 'text',
            example_type    TEXT DEFAULT 'example',
            setup_code      TEXT DEFAULT '',
            teardown_code   TEXT DEFAULT '',
            sort_order      INTEGER DEFAULT 0,
            created_at      TEXT DEFAULT CURRENT_TIMESTAMP
        )
    "
    
    # Parameters
    $db eval "
        CREATE TABLE IF NOT EXISTS parameters (
            id              INTEGER PRIMARY KEY,
            entry_id        INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
            name            TEXT NOT NULL,
            param_type      TEXT DEFAULT '',
            description     TEXT DEFAULT '',
            is_optional     INTEGER DEFAULT 0,
            default_value   TEXT DEFAULT '',
            sort_order      INTEGER DEFAULT 0
        )
    "
    
    # Hints
    $db eval "
        CREATE TABLE IF NOT EXISTS hints (
            id              INTEGER PRIMARY KEY,
            entry_id        INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
            hint_text       TEXT NOT NULL,
            sort_order      INTEGER DEFAULT 0
        )
    "
    
    # Tags
    $db eval "
        CREATE TABLE IF NOT EXISTS tags (
            id              INTEGER PRIMARY KEY,
            name            TEXT UNIQUE NOT NULL,
            description     TEXT DEFAULT ''
        )
    "
    
    $db eval "
        CREATE TABLE IF NOT EXISTS entry_tags (
            entry_id        INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
            tag_id          INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
            PRIMARY KEY (entry_id, tag_id)
        )
    "
    
    # Entry links
    $db eval "
        CREATE TABLE IF NOT EXISTS entry_links (
            from_entry_id   INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
            to_entry_id     INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE,
            link_type       TEXT DEFAULT 'related',
            PRIMARY KEY (from_entry_id, to_entry_id)
        )
    "
    
    # Indexes
    $db eval {CREATE INDEX IF NOT EXISTS idx_entries_type ON entries(entry_type)}
    $db eval {CREATE INDEX IF NOT EXISTS idx_entries_category ON entries(category_id)}
    $db eval {CREATE INDEX IF NOT EXISTS idx_entries_namespace ON entries(namespace)}
    $db eval {CREATE INDEX IF NOT EXISTS idx_entries_published ON entries(published)}
    $db eval {CREATE INDEX IF NOT EXISTS idx_examples_entry ON examples(entry_id)}
    $db eval {CREATE INDEX IF NOT EXISTS idx_parameters_entry ON parameters(entry_id)}
    $db eval {CREATE INDEX IF NOT EXISTS idx_hints_entry ON hints(entry_id)}
    
    # Full-text search
    $db eval "
        CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5(
            title, summary, content, namespace,
            content='entries', content_rowid='id',
            tokenize='porter unicode61'
        )
    "
    
    # FTS triggers
    catch {
        $db eval "
            CREATE TRIGGER entries_ai AFTER INSERT ON entries BEGIN
                INSERT INTO entries_fts(rowid, title, summary, content, namespace)
                VALUES (new.id, new.title, new.summary, new.content, new.namespace);
            END
        "
    }
    
    catch {
        $db eval "
            CREATE TRIGGER entries_ad AFTER DELETE ON entries BEGIN
                INSERT INTO entries_fts(entries_fts, rowid, title, summary, content, namespace)
                VALUES ('delete', old.id, old.title, old.summary, old.content, old.namespace);
            END
        "
    }
    
    catch {
        $db eval "
            CREATE TRIGGER entries_au AFTER UPDATE ON entries BEGIN
                INSERT INTO entries_fts(entries_fts, rowid, title, summary, content, namespace)
                VALUES ('delete', old.id, old.title, old.summary, old.content, old.namespace);
                INSERT INTO entries_fts(rowid, title, summary, content, namespace)
                VALUES (new.id, new.title, new.summary, new.content, new.namespace);
            END
        "
    }
    
    # Update timestamp trigger
    catch {
        $db eval "
            CREATE TRIGGER entries_update_timestamp AFTER UPDATE ON entries BEGIN
                UPDATE entries SET updated_at = CURRENT_TIMESTAMP WHERE id = new.id;
            END
        "
    }
}

# =============================================================================
# Seed Data
# =============================================================================

proc ::docsdb::seed_categories {} {
    variable db
    
    # Define initial categories
    set categories {
        {basics         "Basics"            "Fundamental Tcl concepts"              10  "📚"}
        {dlsh           "DLSH Lists"        "High-performance list operations"      20  "📊"}
        {dg             "Data Grids"        "Tabular data structures"               30  "🗃️"}
        {ess            "State Systems"     "Experimental state machine control"    40  "⚙️"}
        {gpio           "GPIO"              "Hardware pin control"                  50  "🔌"}
        {sound          "Sound"             "Audio playback and generation"         60  "🔊"}
        {loaders        "Loaders"           "Stimulus and data loading"             70  "📥"}
        {getting-started "Getting Started"  "Introduction and setup guides"         5   "🚀"}
        {faq            "FAQ"               "Frequently asked questions"            100 "❓"}
    }
    
    foreach cat $categories {
        lassign $cat slug name desc sort_order icon
        
        # Insert or update
        $db eval {
            INSERT OR REPLACE INTO categories (slug, name, description, sort_order, icon)
            VALUES ($slug, $name, $desc, $sort_order, $icon)
        }
    }
    
    puts "Seeded [llength $categories] categories"
}

proc ::docsdb::seed_tags {} {
    variable db
    
    set tags {
        {memory-management  "Topics related to memory allocation and cleanup"}
        {real-time          "Real-time and timing-critical operations"}
        {beginner-friendly  "Good for newcomers"}
        {advanced           "Requires deeper understanding"}
        {deprecated         "Features that are being phased out"}
        {experimental       "New features that may change"}
        {performance        "Performance-sensitive operations"}
        {hardware           "Hardware interaction"}
    }
    
    foreach tag $tags {
        lassign $tag name desc
        $db eval {
            INSERT OR IGNORE INTO tags (name, description) VALUES ($name, $desc)
        }
    }
    
    puts "Seeded [llength $tags] tags"
}

# =============================================================================
# Command Ingestion (from live interpreter)
# =============================================================================

proc ::docsdb::ingest_commands {pattern {namespace_hint ""}} {
    variable db
    
    set commands [info commands $pattern]
    set imported 0
    set skipped 0
    
    # Get or create category for this namespace
    set category_id [get_or_create_category $namespace_hint]
    
    foreach cmd $commands {
        set result [ingest_single_command $cmd $namespace_hint $category_id]
        if {$result} {
            incr imported
        } else {
            incr skipped
        }
    }
    
    puts "Ingested commands matching '$pattern': $imported imported, $skipped skipped"
    return [dict create imported $imported skipped $skipped]
}

proc ::docsdb::ingest_single_command {cmd namespace_hint category_id} {
    variable db
    
    # Generate slug from command name
    set slug "cmd-[string map {:: - _ -} $cmd]"
    
    # Check if already exists
    set exists 0
    $db eval {SELECT 1 FROM entries WHERE slug = $slug} {
        set exists 1
    }
    
    if {$exists} {
        return 0
    }
    
    # Infer namespace
    set ns $namespace_hint
    if {$ns eq ""} {
        if {[regexp {^(\w+)_} $cmd -> prefix]} {
            set ns $prefix
        } elseif {[regexp {^(\w+)::} $cmd -> prefix]} {
            set ns $prefix
        } else {
            set ns "core"
        }
    }
    
    # Build syntax and parameters
    set syntax "$cmd"
    set params_list [list]
    
    if {[info procs $cmd] ne ""} {
        # It's a Tcl proc - can introspect
        set args [info args $cmd]
        foreach arg $args {
            set is_optional 0
            set default_val ""
            
            if {[info default $cmd $arg dv]} {
                set is_optional 1
                set default_val $dv
                append syntax " ?$arg?"
            } else {
                append syntax " $arg"
            }
            
            lappend params_list [list $arg "" "" $is_optional $default_val]
        }
    } else {
        # C command or built-in
        append syntax " ..."
    }
    
    # Insert the entry
    $db eval {
        INSERT INTO entries (
            slug, entry_type, category_id, title, summary, 
            namespace, syntax, stability, published
        ) VALUES (
            $slug, 'command', $category_id, $cmd, 
            'TODO: Add description',
            $ns, $syntax, 'undocumented', 1
        )
    }
    
    set entry_id [$db last_insert_rowid]
    
    # Insert parameters
    set sort_order 0
    foreach param $params_list {
        lassign $param name type desc is_optional default_val
        $db eval {
            INSERT INTO parameters (entry_id, name, param_type, description, is_optional, default_value, sort_order)
            VALUES ($entry_id, $name, $type, $desc, $is_optional, $default_val, $sort_order)
        }
        incr sort_order
    }
    
    return 1
}

proc ::docsdb::get_or_create_category {namespace} {
    variable db
    
    if {$namespace eq ""} {
        set namespace "core"
    }
    
    set slug $namespace
    set name [string toupper $namespace 0 0]
    
    set category_id ""
    $db eval {SELECT id FROM categories WHERE slug = $slug} {
        set category_id $id
    }
    
    if {$category_id eq ""} {
        $db eval {
            INSERT INTO categories (slug, name, description, sort_order)
            VALUES ($slug, $name, '', 50)
        }
        set category_id [$db last_insert_rowid]
    }
    
    return $category_id
}

# =============================================================================
# Utility: Add content programmatically
# =============================================================================

proc ::docsdb::add_entry {args} {
    variable db
    
    # Parse arguments into dict
    set data [dict create]
    foreach {key val} $args {
        dict set data [string trimleft $key -] $val
    }
    
    # Required fields
    if {![dict exists $data slug] || ![dict exists $data title] || ![dict exists $data type]} {
        error "Required: -slug, -title, -type"
    }
    
    set slug [dict get $data slug]
    set title [dict get $data title]
    set type [dict get $data type]
    
    # Optional fields with defaults
    set summary [expr {[dict exists $data summary] ? [dict get $data summary] : ""}]
    set content [expr {[dict exists $data content] ? [dict get $data content] : ""}]
    set namespace [expr {[dict exists $data namespace] ? [dict get $data namespace] : ""}]
    set syntax [expr {[dict exists $data syntax] ? [dict get $data syntax] : ""}]
    set difficulty [expr {[dict exists $data difficulty] ? [dict get $data difficulty] : "beginner"}]
    set category_slug [expr {[dict exists $data category] ? [dict get $data category] : ""}]
    
    # Get category ID
    set category_id ""
    if {$category_slug ne ""} {
        $db eval {SELECT id FROM categories WHERE slug = $category_slug} {
            set category_id $id
        }
    }
    
    $db eval {
        INSERT INTO entries (slug, entry_type, category_id, title, summary, content, namespace, syntax, difficulty)
        VALUES ($slug, $type, $category_id, $title, $summary, $content, $namespace, $syntax, $difficulty)
    }
    
    return [$db last_insert_rowid]
}

proc ::docsdb::add_example {entry_slug args} {
    variable db
    
    # Get entry ID
    set entry_id ""
    $db eval {SELECT id FROM entries WHERE slug = $entry_slug} {
        set entry_id $id
    }
    
    if {$entry_id eq ""} {
        error "Entry not found: $entry_slug"
    }
    
    # Parse arguments
    set data [dict create]
    foreach {key val} $args {
        dict set data [string trimleft $key -] $val
    }
    
    set code [expr {[dict exists $data code] ? [dict get $data code] : ""}]
    set expected [expr {[dict exists $data expected] ? [dict get $data expected] : ""}]
    set title [expr {[dict exists $data title] ? [dict get $data title] : ""}]
    set desc [expr {[dict exists $data description] ? [dict get $data description] : ""}]
    set type [expr {[dict exists $data type] ? [dict get $data type] : "example"}]
    set output_type [expr {[dict exists $data output_type] ? [dict get $data output_type] : "text"}]
    
    $db eval {
        INSERT INTO examples (entry_id, title, description, code, expected_output, output_type, example_type)
        VALUES ($entry_id, $title, $desc, $code, $expected, $output_type, $type)
    }
    
    return [$db last_insert_rowid]
}

proc ::docsdb::add_hint {entry_slug hint_text {sort_order 0}} {
    variable db
    
    set entry_id ""
    $db eval {SELECT id FROM entries WHERE slug = $entry_slug} {
        set entry_id $id
    }
    
    if {$entry_id eq ""} {
        error "Entry not found: $entry_slug"
    }
    
    $db eval {
        INSERT INTO hints (entry_id, hint_text, sort_order)
        VALUES ($entry_id, $hint_text, $sort_order)
    }
    
    return [$db last_insert_rowid]
}

# =============================================================================
# Stats and info
# =============================================================================

proc ::docsdb::stats {} {
    variable db
    
    set result [dict create]
    
    dict set result commands [$db eval {SELECT COUNT(*) FROM entries WHERE entry_type = 'command'}]
    dict set result tutorials [$db eval {SELECT COUNT(*) FROM entries WHERE entry_type = 'tutorial'}]
    dict set result guides [$db eval {SELECT COUNT(*) FROM entries WHERE entry_type = 'guide'}]
    dict set result categories [$db eval {SELECT COUNT(*) FROM categories}]
    dict set result examples [$db eval {SELECT COUNT(*) FROM examples}]
    
    return $result
}

# =============================================================================
# Export
# =============================================================================

namespace eval ::docsdb {
    namespace export init close setup is_setup check_db \
                     seed_categories seed_tags ingest_commands \
                     add_entry add_example add_hint stats
}

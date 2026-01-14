# -*- mode: tcl -*-
#
# ess_configs-1_0.tm
#
# Configuration library for ESS - provides persistent storage, tagging,
# and retrieval of experiment configurations.
#
# This module is loaded by the configs subprocess and provides:
#   - SQLite database for config storage
#   - CRUD operations (save, load, list, get, update, delete)
#   - Tag-based filtering
#   - Usage tracking
#   - Cross-rig export/import
#   - Datapoint publishing for reactive UIs
#   - Schema versioning and migrations
#

package require sqlite3
package require dlsh
package require tcljson
package require yajltcl
package provide ess_configs 1.0

namespace eval ess::configs {
    
    #=========================================================================
    # Module State
    #=========================================================================
    
    variable db ""              ;# SQLite database command
    variable db_path ""         ;# Path to database file
    variable initialized 0      ;# Whether init has been called
    
    # Current schema version - increment when adding migrations
    variable SCHEMA_VERSION 2
    
    #=========================================================================
    # Initialization
    #=========================================================================
    
    # Return the database handle (for sharing with other modules like ess_queues)
    proc get_db {} {
        variable db
        return $db
    }
    
    proc init {path} {
        variable db
        variable db_path
        variable initialized
        
        if {$initialized} {
            log warning "ess::configs already initialized"
            return
        }
        
        set db_path $path
        
        # Create database connection
        sqlite3 configdb $path
        set db "configdb"
        
        # Enable WAL mode for better concurrency
        configdb eval { PRAGMA journal_mode=WAL }
        configdb eval { PRAGMA busy_timeout=5000 }
        configdb eval { PRAGMA foreign_keys=ON }
        
        # Create base schema if needed, then run migrations
        init_schema
        migrate
        
        set initialized 1
        log info "ess::configs initialized: $path"
    }
    
    proc init_schema {} {
        # Base schema - version 1
        # This creates tables if they don't exist
        configdb eval {
            -- Main configs table
            CREATE TABLE IF NOT EXISTS configs (
                id INTEGER PRIMARY KEY,
                name TEXT NOT NULL,
                description TEXT DEFAULT '',
                
                -- ESS config fields (the loadable part)
                script_source TEXT DEFAULT '',
                system TEXT NOT NULL,
                protocol TEXT NOT NULL,
                variant TEXT NOT NULL,
                subject TEXT DEFAULT '',
                variant_args TEXT DEFAULT '{}',    -- JSON dict
                params TEXT DEFAULT '{}',          -- JSON dict
                
                -- Metadata
                tags TEXT DEFAULT '[]',            -- JSON array
                created_by TEXT DEFAULT '',
                created_at INTEGER DEFAULT (strftime('%s', 'now')),
                updated_at INTEGER DEFAULT (strftime('%s', 'now')),
                last_used_at INTEGER,
                use_count INTEGER DEFAULT 0,
                archived INTEGER DEFAULT 0
            );
            
            -- Unique name only for active (non-archived) configs
            -- This allows reusing names after deletion
            CREATE UNIQUE INDEX IF NOT EXISTS configs_name_active 
                ON configs(name) WHERE archived = 0;
            
            -- Indexes for common queries
            CREATE INDEX IF NOT EXISTS idx_configs_name ON configs(name);
            CREATE INDEX IF NOT EXISTS idx_configs_system ON configs(system, protocol, variant);
            CREATE INDEX IF NOT EXISTS idx_configs_archived ON configs(archived);
            CREATE INDEX IF NOT EXISTS idx_configs_use_count ON configs(use_count DESC);
            CREATE INDEX IF NOT EXISTS idx_configs_last_used ON configs(last_used_at DESC);
        }
        
        # Set version 1 if this is a fresh database
        if {[schema_version] == 0} {
            set_schema_version 1
        }
    }
    
    proc close {} {
        variable db
        variable initialized
        
        if {$initialized} {
            configdb close
            set initialized 0
        }
    }
    
    #=========================================================================
    # Schema Migration System
    #=========================================================================
    
    # Get current schema version from database
    proc schema_version {} {
        configdb eval {PRAGMA user_version}
    }
    
    # Set schema version
    proc set_schema_version {v} {
        configdb eval "PRAGMA user_version = $v"
    }
    
    # Check if a column exists in a table
    proc column_exists {table column} {
        set cols [configdb eval "PRAGMA table_info($table)"]
        # table_info returns: cid name type notnull dflt_value pk (6 items per column)
        foreach {cid name type notnull dflt pk} $cols {
            if {$name eq $column} {
                return 1
            }
        }
        return 0
    }
    
    # Check if a table exists
    proc table_exists {table} {
        set count [configdb eval {
            SELECT COUNT(*) FROM sqlite_master 
            WHERE type='table' AND name=:table
        }]
        return [expr {$count > 0}]
    }
    
    # Run all pending migrations
    proc migrate {} {
        variable SCHEMA_VERSION
        
        set current [schema_version]
        
        if {$current >= $SCHEMA_VERSION} {
            log info "Schema up to date (version $current)"
            return
        }
        
        log info "Migrating schema from version $current to $SCHEMA_VERSION"
        
        # Migration 2: Add short_name to configs for filename generation
        if {$current < 2} {
            migrate_to_v2
        }
        
        # Future migrations go here:
        # if {$current < 3} { migrate_to_v3 }
        # if {$current < 4} { migrate_to_v4 }
        
        log info "Schema migration complete (now at version [schema_version])"
    }
    
    # Migration 2: Add short_name column to configs
    proc migrate_to_v2 {} {
        log info "Migration 2: Adding short_name column to configs"
        
        if {![column_exists configs short_name]} {
            configdb eval {
                ALTER TABLE configs ADD COLUMN short_name TEXT DEFAULT ''
            }
            log info "  Added configs.short_name"
        } else {
            log info "  configs.short_name already exists, skipping"
        }
        
        set_schema_version 2
    }
    
    # Get schema information for diagnostics
    proc schema_info {} {
        variable SCHEMA_VERSION
        
        set info [dict create]
        dict set info current_version [schema_version]
        dict set info target_version $SCHEMA_VERSION
        dict set info needs_migration [expr {[schema_version] < $SCHEMA_VERSION}]
        
        # List tables
        set tables [configdb eval {
            SELECT name FROM sqlite_master WHERE type='table' ORDER BY name
        }]
        dict set info tables $tables
        
        # Get column info for main tables
        foreach table {configs queues queue_items} {
            if {[table_exists $table]} {
                set cols {}
                configdb eval "PRAGMA table_info($table)" row {
                    lappend cols $row(name)
                }
                dict set info columns $table $cols
            }
        }
        
        return $info
    }
    
    # Backup database before destructive operations
    proc backup {{suffix ""}} {
        variable db_path
        
        if {$suffix eq ""} {
            set suffix [clock format [clock seconds] -format "%Y%m%d_%H%M%S"]
        }
        
        set backup_path "${db_path}.backup_${suffix}"
        
        # Use SQLite's backup API via VACUUM INTO (SQLite 3.27+)
        # Fall back to file copy if not available
        if {[catch {
            configdb eval "VACUUM INTO '$backup_path'"
        }]} {
            # Fallback: close, copy, reopen
            file copy -force $db_path $backup_path
        }
        
        log info "Database backed up to: $backup_path"
        return $backup_path
    }
    
    # Reset database (dangerous - requires confirmation token)
    proc reset {confirm_token} {
        variable db_path
        variable initialized
        
        # Require specific confirmation to prevent accidents
        if {$confirm_token ne "YES_DELETE_ALL_DATA"} {
            error "Reset requires confirm_token 'YES_DELETE_ALL_DATA'"
        }
        
        # Backup first
        set backup [backup "pre_reset"]
        
        log warning "Resetting database - all data will be deleted"
        
        # Drop all tables
        set tables [configdb eval {
            SELECT name FROM sqlite_master WHERE type='table'
        }]
        foreach table $tables {
            if {$table ne "sqlite_sequence"} {
                configdb eval "DROP TABLE IF EXISTS $table"
            }
        }
        
        # Reset version
        set_schema_version 0
        
        # Reinitialize
        init_schema
        migrate
        
        log info "Database reset complete. Backup at: $backup"
        return $backup
    }
    
    # Rebuild indexes (maintenance operation)
    proc rebuild_indexes {} {
        log info "Rebuilding database indexes"
        configdb eval {REINDEX}
        log info "Index rebuild complete"
    }
    
    # Optimize database (maintenance operation)
    proc optimize {} {
        log info "Optimizing database"
        configdb eval {VACUUM}
        configdb eval {ANALYZE}
        log info "Optimization complete"
    }
    
    #=========================================================================
    # Logging Helper
    #=========================================================================
    
    proc log {level msg} {
        if {[catch {dservSet configs/log "\[$level\] $msg"}]} {
            puts "ess::configs $level: $msg"
        }
    }
    
    #=========================================================================
    # Save Operations
    #=========================================================================
    
    # Save current ESS state as a named config
    proc save_current {name args} {
        # Parse options
        set description ""
        set tags {}
        set created_by ""
        set short_name ""
        
        foreach {key value} $args {
            switch -- $key {
                -description { set description $value }
                -tags        { set tags $value }
                -created_by  { set created_by $value }
                -short_name  { set short_name $value }
            }
        }
        
        if {$created_by eq ""} {
            set created_by [get_current_user]
        }
        
        # Capture current config from ESS via datapoints
        set script_source [dservGet ess/project]
        set system        [dservGet ess/system]
        set protocol      [dservGet ess/protocol]
        set variant       [dservGet ess/variant]
        set subject       [dservGet ess/subject]
        
        if {$system eq ""} {
            error "No system loaded - cannot save config"
        }
        
        # Get variant_args from variant_info
        set variant_info [dservGet ess/variant_info]
        set variant_args {}
        if {$variant_info ne ""} {
            if {[dict exists $variant_info loader_arg_names] && 
                [dict exists $variant_info loader_args]} {
                set names [dict get $variant_info loader_arg_names]
                set values [dict get $variant_info loader_args]
                foreach n $names v $values {
                    dict set variant_args $n $v
                }
            }
        }
        
        # Get params
        set params [dservGet ess/params]
        if {$params eq ""} {
            set params {}
        }
        
        # Convert to JSON for storage
        set variant_args_json [dict_to_json $variant_args]
        set params_json [dict_to_json $params]
        set tags_json [tags_to_json $tags]
        
        # Check if name exists (only check active configs, not archived)
        set existing_id [configdb eval { 
            SELECT id FROM configs WHERE name = :name AND archived = 0
        }]
        
        if {[llength $existing_id] > 0} {
            # Update existing
            set config_id [lindex $existing_id 0]
            configdb eval {
                UPDATE configs SET
                    description = :description,
                    script_source = :script_source,
                    system = :system,
                    protocol = :protocol,
                    variant = :variant,
                    subject = :subject,
                    variant_args = :variant_args_json,
                    params = :params_json,
                    tags = :tags_json,
                    short_name = :short_name,
                    updated_at = strftime('%s', 'now')
                WHERE id = :config_id
            }
            log info "Updated config: $name (id=$config_id)"
        } else {
            # Insert new
            configdb eval {
                INSERT INTO configs 
                    (name, description, script_source, system, protocol, variant,
                     subject, variant_args, params, tags, created_by, short_name)
                VALUES 
                    (:name, :description, :script_source, :system, :protocol, :variant,
                     :subject, :variant_args_json, :params_json, :tags_json, :created_by, :short_name)
            }
            set config_id [configdb last_insert_rowid]
            log info "Created config: $name (id=$config_id)"
        }
        
        # Publish updated list
        publish_list
        
        return $config_id
    }
    
    # Create config from explicit values
    proc create {name system protocol variant args} {
        set description ""
        set script_source ""
        set subject ""
        set variant_args {}
        set params {}
        set tags {}
        set created_by ""
        set short_name ""
        
        foreach {key value} $args {
            switch -- $key {
                -description   { set description $value }
                -script_source { set script_source $value }
                -subject       { set subject $value }
                -variant_args  { set variant_args $value }
                -params        { set params $value }
                -tags          { set tags $value }
                -created_by    { set created_by $value }
                -short_name    { set short_name $value }
            }
        }
        
        if {$created_by eq ""} {
            set created_by [get_current_user]
        }
        
        set variant_args_json [dict_to_json $variant_args]
        set params_json [dict_to_json $params]
        set tags_json [tags_to_json $tags]
        
        configdb eval {
            INSERT INTO configs 
                (name, description, script_source, system, protocol, variant,
                 subject, variant_args, params, tags, created_by, short_name)
            VALUES 
                (:name, :description, :script_source, :system, :protocol, :variant,
                 :subject, :variant_args_json, :params_json, :tags_json, :created_by, :short_name)
        }
        
        set config_id [configdb last_insert_rowid]
        log info "Created config: $name (id=$config_id)"
        
        publish_list
        return $config_id
    }
    
    #=========================================================================
    # Load Operations
    #=========================================================================
    
    # Load a config into ESS
    proc load {name_or_id} {
        set config [get $name_or_id]
        
        if {$config eq ""} {
            error "Config not found: $name_or_id"
        }
        
        # Build ESS config dict (just the fields ESS needs)
        set ess_config [dict create \
            script_source [dict get $config script_source] \
            system        [dict get $config system] \
            protocol      [dict get $config protocol] \
            variant       [dict get $config variant] \
            subject       [dict get $config subject] \
            variant_args  [dict get $config variant_args] \
            params        [dict get $config params]]

        # Send to ESS thread to load
        log info "Loading config: [dict get $config name]"
        set result [send ess "::ess::load_config {$ess_config}"]
        
        # Update usage stats
        set config_id [dict get $config id]
        configdb eval {
            UPDATE configs SET 
                use_count = use_count + 1,
                last_used_at = strftime('%s', 'now')
            WHERE id = :config_id
        }
        
        # Publish current active config
        dservSet configs/current [dict_to_json [dict create \
            id   $config_id \
            name [dict get $config name]]]
        
        publish_list
        
        return $result
    }
    
    #=========================================================================
    # Query Operations
    #=========================================================================
    
    # Get full config by name or id
    proc get {name_or_id} {
        if {[string is integer $name_or_id]} {
            set where_clause "id = :name_or_id"
        } else {
            set where_clause "name = :name_or_id"
        }
        
        set result ""
        configdb eval "
            SELECT id, name, description, script_source, system, protocol, variant,
                   subject, variant_args, params, tags, created_by, created_at,
                   updated_at, last_used_at, use_count, archived, short_name
            FROM configs 
            WHERE $where_clause AND archived = 0
        " row {
            set result [dict create \
                id            $row(id) \
                name          $row(name) \
                description   $row(description) \
                script_source $row(script_source) \
                system        $row(system) \
                protocol      $row(protocol) \
                variant       $row(variant) \
                subject       $row(subject) \
                variant_args  [json_to_dict $row(variant_args)] \
                params        [json_to_dict $row(params)] \
                tags          [json_to_dict $row(tags)] \
                created_by    $row(created_by) \
                created_at    $row(created_at) \
                updated_at    $row(updated_at) \
                last_used_at  $row(last_used_at) \
                use_count     $row(use_count) \
                archived      $row(archived) \
                short_name    $row(short_name)]
        }
        
        return $result
    }
    
    # Check if config exists
    proc exists {name_or_id} {
        if {[string is integer $name_or_id]} {
            set count [configdb eval { 
                SELECT COUNT(*) FROM configs WHERE id = :name_or_id AND archived = 0 
            }]
        } else {
            set count [configdb eval { 
                SELECT COUNT(*) FROM configs WHERE name = :name_or_id AND archived = 0 
            }]
        }
        return [expr {$count > 0}]
    }
    
    # List configs with optional filtering
    proc list {args} {
        set tags {}
        set system ""
        set protocol ""
        set subject ""
        set search ""
        set include_archived 0
        set limit 100
        set order "use_count DESC, last_used_at DESC"
        
        foreach {key value} $args {
            switch -- $key {
                -tags     { set tags $value }
                -system   { set system $value }
                -protocol { set protocol $value }
                -subject  { set subject $value }
                -search   { set search $value }
                -archived { set include_archived $value }
                -limit    { set limit $value }
                -order    { set order $value }
            }
        }
        
        set where_clauses {}
        
        if {!$include_archived} {
            lappend where_clauses "archived = 0"
        }
        
        if {$system ne ""} {
            lappend where_clauses "system = :system"
        }
        
        if {$protocol ne ""} {
            lappend where_clauses "protocol = :protocol"
        }
        
        if {$subject ne ""} {
            lappend where_clauses "subject = :subject"
        }
        
        if {$search ne ""} {
            lappend where_clauses "(name LIKE '%' || :search || '%' OR description LIKE '%' || :search || '%')"
        }
        
        # Tag filtering - check if all requested tags are present
        foreach tag $tags {
            lappend where_clauses "tags LIKE '%\"$tag\"%'"
        }
        
        set where_sql ""
        if {[llength $where_clauses] > 0} {
            set where_sql "WHERE [join $where_clauses { AND }]"
        }
        
        set results {}
        configdb eval "
            SELECT id, name, description, system, protocol, variant, subject,
                   tags, use_count, last_used_at, short_name
            FROM configs
            $where_sql
            ORDER BY $order
            LIMIT :limit
        " row {
            lappend results [dict create \
                id          $row(id) \
                name        $row(name) \
                description $row(description) \
                system      $row(system) \
                protocol    $row(protocol) \
                variant     $row(variant) \
                subject     $row(subject) \
                tags        [json_to_dict $row(tags)] \
                use_count   $row(use_count) \
                last_used_at $row(last_used_at) \
                short_name  $row(short_name)]
        }
        
        return $results
    }
    
    # Get all unique tags
    proc get_all_tags {} {
        set all_tags {}
        configdb eval {
            SELECT DISTINCT tags FROM configs WHERE archived = 0
        } row {
            foreach tag [json_to_dict $row(tags)] {
                if {$tag ni $all_tags} {
                    lappend all_tags $tag
                }
            }
        }
        return [lsort $all_tags]
    }
    
    # Quick picks - most frequently used configs
    proc quick_picks {{limit 5}} {
        set results {}
        configdb eval {
            SELECT id, name, description, system, protocol, variant, use_count
            FROM configs
            WHERE archived = 0 AND use_count > 0
            ORDER BY use_count DESC, last_used_at DESC
            LIMIT :limit
        } row {
            lappend results [dict create \
                id          $row(id) \
                name        $row(name) \
                description $row(description) \
                system      $row(system) \
                protocol    $row(protocol) \
                variant     $row(variant) \
                use_count   $row(use_count)]
        }
        return $results
    }
    
    #=========================================================================
    # Update Operations
    #=========================================================================
    
    proc update {name_or_id args} {
        set config [get $name_or_id]
        if {$config eq ""} {
            error "Config not found: $name_or_id"
        }
        
        set config_id [dict get $config id]
        set updates {}
        
        foreach {key value} $args {
            switch -- $key {
                -name {
                    lappend updates "name = :new_name"
                    set new_name $value
                }
                -description {
                    lappend updates "description = :new_description"
                    set new_description $value
                }
                -tags {
                    set new_tags_json [tags_to_json $value]
                    lappend updates "tags = :new_tags_json"
                }
                -subject {
                    lappend updates "subject = :new_subject"
                    set new_subject $value
                }
                -variant_args {
                    set new_variant_args_json [dict_to_json $value]
                    lappend updates "variant_args = :new_variant_args_json"
                }
                -params {
                    set new_params_json [dict_to_json $value]
                    lappend updates "params = :new_params_json"
                }
                -short_name {
                    lappend updates "short_name = :new_short_name"
                    set new_short_name $value
                }
            }
        }
        
        if {[llength $updates] == 0} {
            return $config_id
        }
        
        lappend updates "updated_at = strftime('%s', 'now')"
        
        set sql "UPDATE configs SET [join $updates {, }] WHERE id = :config_id"
        configdb eval $sql
        
        log info "Updated config: [dict get $config name]"
        publish_list
        
        # If tags changed, refresh the tags list too
        if {[info exists new_tags_json]} {
            publish_tags
        }
        
        return $config_id
    }
    
    # Clone a config
    proc clone {source_name_or_id new_name args} {
        set source [get $source_name_or_id]
        if {$source eq ""} {
            error "Source config not found: $source_name_or_id"
        }
        
        # Start with source values
        set description [dict get $source description]
        set tags [dict get $source tags]
        set variant_args [dict get $source variant_args]
        set params [dict get $source params]
        set short_name [dict get $source short_name]
        
        # Apply overrides
        foreach {key value} $args {
            switch -- $key {
                -description  { set description $value }
                -tags         { set tags $value }
                -variant_args { set variant_args [dict merge $variant_args $value] }
                -params       { set params [dict merge $params $value] }
                -short_name   { set short_name $value }
            }
        }
        
        # Create new config
        set new_id [create $new_name \
            [dict get $source system] \
            [dict get $source protocol] \
            [dict get $source variant] \
            -description $description \
            -script_source [dict get $source script_source] \
            -subject [dict get $source subject] \
            -variant_args $variant_args \
            -params $params \
            -tags $tags \
            -short_name $short_name]
        
        log info "Cloned config: [dict get $source name] -> $new_name"
        
        return $new_id
    }
    
    #=========================================================================
    # Delete Operations
    #=========================================================================
    
    # Soft delete (archive)
    proc archive {name_or_id} {
        if {[string is integer $name_or_id]} {
            configdb eval { 
                UPDATE configs SET archived = 1, updated_at = strftime('%s', 'now')
                WHERE id = :name_or_id 
            }
        } else {
            configdb eval { 
                UPDATE configs SET archived = 1, updated_at = strftime('%s', 'now')
                WHERE name = :name_or_id 
            }
        }
        log info "Archived config: $name_or_id"
        publish_list
        publish_archived
    }
    
    # Hard delete
    proc delete {name_or_id} {
        if {[string is integer $name_or_id]} {
            configdb eval { DELETE FROM configs WHERE id = :name_or_id }
        } else {
            configdb eval { DELETE FROM configs WHERE name = :name_or_id }
        }
        log info "Deleted config: $name_or_id"
        publish_list
        publish_archived
    }
    
    # Restore archived config
    proc restore {name_or_id} {
        if {[string is integer $name_or_id]} {
            configdb eval { 
                UPDATE configs SET archived = 0, updated_at = strftime('%s', 'now')
                WHERE id = :name_or_id 
            }
        } else {
            configdb eval { 
                UPDATE configs SET archived = 0, updated_at = strftime('%s', 'now')
                WHERE name = :name_or_id 
            }
        }
        log info "Restored config: $name_or_id"
        publish_list
        publish_archived
    }
    
    #=========================================================================
    # Export/Import for Cross-Rig Sync
    #=========================================================================
    
    # Export config as JSON string
    proc export {name_or_id} {
        set config [get $name_or_id]
        if {$config eq ""} {
            error "Config not found: $name_or_id"
        }
        
        # Remove local metadata that shouldn't transfer
        dict unset config id
        dict unset config created_at
        dict unset config updated_at
        dict unset config last_used_at
        dict unset config use_count
        dict unset config archived
        
        # Use -deep since config contains nested dicts (variant_args, params, tags)
        return [dict_to_json $config -deep]
    }
    
    # Import config from JSON string
    proc import {json args} {
        set config [json_to_dict $json]
        
        set remap_script_source ""
        foreach {key value} $args {
            switch -- $key {
                -remap_script_source { set remap_script_source $value }
            }
        }
        
        # Remap script_source if requested
        if {$remap_script_source ne "" && [dict exists $config script_source]} {
            dict set config script_source $remap_script_source
        }
        
        # Check if name already exists
        set name [dict get $config name]
        if {[exists $name]} {
            error "Config already exists: $name (use update or delete first)"
        }
        
        # Extract fields with defaults
        set tags {}
        if {[dict exists $config tags]} {
            set tags [dict get $config tags]
        }
        
        set short_name ""
        if {[dict exists $config short_name]} {
            set short_name [dict get $config short_name]
        }
        
        set new_id [create $name \
            [dict get $config system] \
            [dict get $config protocol] \
            [dict get $config variant] \
            -description [expr {[dict exists $config description] ? [dict get $config description] : ""}] \
            -script_source [expr {[dict exists $config script_source] ? [dict get $config script_source] : ""}] \
            -subject [expr {[dict exists $config subject] ? [dict get $config subject] : ""}] \
            -variant_args [expr {[dict exists $config variant_args] ? [dict get $config variant_args] : {}}] \
            -params [expr {[dict exists $config params] ? [dict get $config params] : {}}] \
            -tags $tags \
            -short_name $short_name \
            -created_by [expr {[dict exists $config created_by] ? [dict get $config created_by] : ""}]]
        
        log info "Imported config: $name"
        
        return $new_id
    }
    
    #=========================================================================
    # Publishing for Reactive UIs
    #=========================================================================
    
    proc publish_list {} {
        set obj [yajl create #auto]
        $obj array_open
        
        configdb eval {
            SELECT id, name, description, system, protocol, variant,
                   subject, tags, use_count, last_used_at, short_name
            FROM configs
            WHERE archived = 0
            ORDER BY use_count DESC, last_used_at DESC
            LIMIT 100
        } row {
            $obj map_open
            $obj string "id" number $row(id)
            $obj string "name" string $row(name)
            $obj string "description" string $row(description)
            $obj string "system" string $row(system)
            $obj string "protocol" string $row(protocol)
            $obj string "variant" string $row(variant)
            $obj string "subject" string $row(subject)
            $obj string "use_count" number $row(use_count)
            $obj string "short_name" string $row(short_name)
            if {$row(last_used_at) ne ""} {
                $obj string "last_used_at" number $row(last_used_at)
            } else {
                $obj string "last_used_at" null
            }
            
            # Tags - parse JSON array from DB and rebuild as proper array
            $obj string "tags" array_open
            set tags_json $row(tags)
            if {$tags_json ne "" && $tags_json ne {[]}} {
                foreach tag [json_to_dict $tags_json] {
                    $obj string $tag
                }
            }
            $obj array_close
            
            $obj map_close
        }
        
        $obj array_close
        set result [$obj get]
        $obj delete
        
        dservSet configs/list $result
    }
    
    proc publish_archived {} {
        set obj [yajl create #auto]
        $obj array_open
        
        configdb eval {
            SELECT id, name, description, system, protocol, variant,
                   subject, tags, updated_at
            FROM configs
            WHERE archived = 1
            ORDER BY updated_at DESC
            LIMIT 50
        } row {
            $obj map_open
            $obj string "id" number $row(id)
            $obj string "name" string $row(name)
            $obj string "description" string $row(description)
            $obj string "system" string $row(system)
            $obj string "protocol" string $row(protocol)
            $obj string "variant" string $row(variant)
            $obj string "subject" string $row(subject)
            if {$row(updated_at) ne ""} {
                $obj string "updated_at" number $row(updated_at)
            } else {
                $obj string "updated_at" null
            }
            
            # Tags - parse JSON array from DB and rebuild as proper array
            $obj string "tags" array_open
            set tags_json $row(tags)
            if {$tags_json ne "" && $tags_json ne {[]}} {
                foreach tag [json_to_dict $tags_json] {
                    $obj string $tag
                }
            }
            $obj array_close
            
            $obj map_close
        }
        
        $obj array_close
        set result [$obj get]
        $obj delete
        
        dservSet configs/archived $result
    }
    
    proc publish_tags {} {
        set tags [get_all_tags]
        dservSet configs/tags [tags_to_json $tags]
    }
    
    proc publish_quick_picks {} {
        set picks [quick_picks]
        # Build JSON array of dicts
        set json_items {}
        foreach pick $picks {
            lappend json_items [dict_to_json $pick]
        }
        set json_array "\[[join $json_items ,]\]"
        dservSet configs/quick_picks $json_array
    }
    
    proc publish_all {} {
        publish_list
        publish_archived
        publish_tags
        publish_quick_picks
    }
    
    #=========================================================================
    # Automatic Cleanup
    #=========================================================================
    
    # Purge configs archived more than N days ago
    proc purge_old_archived {{days 30}} {
        set cutoff [expr {[clock seconds] - ($days * 86400)}]
        set count [configdb onecolumn {
            SELECT COUNT(*) FROM configs 
            WHERE archived = 1 AND updated_at < :cutoff
        }]
        if {$count > 0} {
            configdb eval {
                DELETE FROM configs 
                WHERE archived = 1 AND updated_at < :cutoff
            }
            log info "Purged $count archived configs older than $days days"
            publish_archived
        }
        return $count
    }
    
    #=========================================================================
    # Helpers
    #=========================================================================
    
    # Convert a Tcl list of tags to a JSON array using yajltcl
    # This ensures proper array format even for single-element or empty lists
    proc tags_to_json {tags} {
        set obj [yajl create #auto]
        $obj array_open
        foreach tag $tags {
            $obj string $tag
        }
        $obj array_close
        set result [$obj get]
        $obj delete
        return $result
    }
    
    proc get_current_user {} {
        if {[info exists ::env(USER)]} {
            return $::env(USER)
        } elseif {[info exists ::env(ESS_USER)]} {
            return $::env(ESS_USER)
        } else {
            return "unknown"
        }
    }

}

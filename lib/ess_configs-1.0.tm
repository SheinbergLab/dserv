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
    
    #=========================================================================
    # Initialization
    #=========================================================================
    
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
        
        # Create schema
        init_schema
        
        set initialized 1
        log info "ess::configs initialized: $path"
    }
    
    proc init_schema {} {
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
        
        foreach {key value} $args {
            switch -- $key {
                -description { set description $value }
                -tags        { set tags $value }
                -created_by  { set created_by $value }
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
        # variant_args and params are flat dicts - shallow is fine
        # tags is a simple list
        set variant_args_json [dict_to_json $variant_args]
        set params_json [dict_to_json $params]
        set tags_json [list_to_json $tags]
        
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
                    updated_at = strftime('%s', 'now')
                WHERE id = :config_id
            }
            log info "Updated config: $name (id=$config_id)"
        } else {
            # Insert new
            configdb eval {
                INSERT INTO configs 
                    (name, description, script_source, system, protocol, variant,
                     subject, variant_args, params, tags, created_by)
                VALUES 
                    (:name, :description, :script_source, :system, :protocol, :variant,
                     :subject, :variant_args_json, :params_json, :tags_json, :created_by)
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
        
        foreach {key value} $args {
            switch -- $key {
                -description   { set description $value }
                -script_source { set script_source $value }
                -subject       { set subject $value }
                -variant_args  { set variant_args $value }
                -params        { set params $value }
                -tags          { set tags $value }
                -created_by    { set created_by $value }
            }
        }
        
        if {$created_by eq ""} {
            set created_by [get_current_user]
        }
        
        set variant_args_json [dict_to_json $variant_args]
        set params_json [dict_to_json $params]
        set tags_json [list_to_json $tags]
        
        configdb eval {
            INSERT INTO configs 
                (name, description, script_source, system, protocol, variant,
                 subject, variant_args, params, tags, created_by)
            VALUES 
                (:name, :description, :script_source, :system, :protocol, :variant,
                 :subject, :variant_args_json, :params_json, :tags_json, :created_by)
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
                   updated_at, last_used_at, use_count, archived
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
                archived      $row(archived)]
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
        
        # Build WHERE clauses
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
        
        # Tag filtering - check if tags JSON array contains each tag
        foreach tag $tags {
            lappend where_clauses "tags LIKE '%\"$tag\"%'"
        }
        
        set where_sql ""
        if {[llength $where_clauses] > 0} {
            set where_sql "WHERE [join $where_clauses { AND }]"
        }
        
        set sql "
            SELECT id, name, description, script_source, system, protocol, variant,
                   subject, tags, created_by, created_at, updated_at, 
                   last_used_at, use_count
            FROM configs
            $where_sql
            ORDER BY $order
            LIMIT :limit
        "
        
        set results {}
        configdb eval $sql row {
            lappend results [dict create \
                id            $row(id) \
                name          $row(name) \
                description   $row(description) \
                script_source $row(script_source) \
                system        $row(system) \
                protocol      $row(protocol) \
                variant       $row(variant) \
                subject       $row(subject) \
                tags          [json_to_dict $row(tags)] \
                created_by    $row(created_by) \
                created_at    $row(created_at) \
                updated_at    $row(updated_at) \
                last_used_at  $row(last_used_at) \
                use_count     $row(use_count)]
        }
        
        return $results
    }
    
    # Get quick picks - most used configs
    proc quick_picks {{limit 5}} {
        set results {}
        configdb eval {
            SELECT id, name, system, protocol, variant, use_count
            FROM configs
            WHERE archived = 0 AND use_count > 0
            ORDER BY use_count DESC, last_used_at DESC
            LIMIT :limit
        } row {
            lappend results [dict create \
                id        $row(id) \
                name      $row(name) \
                system    $row(system) \
                protocol  $row(protocol) \
                variant   $row(variant) \
                use_count $row(use_count)]
        }
        return $results
    }
    
    # Get all unique tags in use
    proc get_all_tags {} {
        set all_tags {}
        configdb eval {
            SELECT DISTINCT tags FROM configs WHERE archived = 0 AND tags != '[]'
        } row {
            # tags is stored as JSON array, parse it
            foreach tag [json_to_dict $row(tags)] {
                if {[lsearch $all_tags $tag] < 0} {
                    lappend all_tags $tag
                }
            }
        }
        return [lsort $all_tags]
    }
    
    #=========================================================================
    # Update Operations
    #=========================================================================
    
    # Update config metadata and/or values
    proc update {name_or_id args} {
        set config [get $name_or_id]
        if {$config eq ""} {
            error "Config not found: $name_or_id"
        }
        
        set config_id [dict get $config id]
        
        # Parse what to update
        set updates {}
        foreach {key value} $args {
            switch -- $key {
                -name        { lappend updates "name = :new_name"; set new_name $value }
                -description { lappend updates "description = :new_desc"; set new_desc $value }
                -subject     { lappend updates "subject = :new_subj"; set new_subj $value }
                -tags        { 
                    set new_tags_json [list_to_json $value]
                    lappend updates "tags = :new_tags_json" 
                }
                -variant_args {
                    set new_vargs_json [dict_to_json $value]
                    lappend updates "variant_args = :new_vargs_json"
                }
                -params {
                    set new_params_json [dict_to_json $value]
                    lappend updates "params = :new_params_json"
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
        
        # Apply overrides
        foreach {key value} $args {
            switch -- $key {
                -description  { set description $value }
                -tags         { set tags $value }
                -variant_args { set variant_args [dict merge $variant_args $value] }
                -params       { set params [dict merge $params $value] }
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
            -tags $tags]
        
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
            -created_by [expr {[dict exists $config created_by] ? [dict get $config created_by] : ""}]]
        
        log info "Imported config: $name"
        
        return $new_id
    }
    
    #=========================================================================
    # Publishing for Reactive UIs
    #=========================================================================
    
    proc publish_list {} {
        # Build list of config summaries as JSON array
        set json_items {}
        configdb eval {
            SELECT id, name, description, system, protocol, variant,
                   subject, tags, use_count, last_used_at
            FROM configs
            WHERE archived = 0
            ORDER BY use_count DESC, last_used_at DESC
            LIMIT 100
        } row {
            # Build each item as a dict, then convert to JSON
            set item [dict create \
                id           $row(id) \
                name         $row(name) \
                description  $row(description) \
                system       $row(system) \
                protocol     $row(protocol) \
                variant      $row(variant) \
                subject      $row(subject) \
                tags         [json_to_dict $row(tags)] \
                use_count    $row(use_count) \
                last_used_at $row(last_used_at)]
            lappend json_items [dict_to_json $item -deep]
        }
        
        # Build JSON array manually
        set json_array "\[[join $json_items ,]\]"
        dservSet configs/list $json_array
    }
    
    proc publish_archived {} {
        # Build list of archived config summaries as JSON array
        set json_items {}
        configdb eval {
            SELECT id, name, description, system, protocol, variant,
                   subject, tags, updated_at
            FROM configs
            WHERE archived = 1
            ORDER BY updated_at DESC
            LIMIT 50
        } row {
            set item [dict create \
                id           $row(id) \
                name         $row(name) \
                description  $row(description) \
                system       $row(system) \
                protocol     $row(protocol) \
                variant      $row(variant) \
                subject      $row(subject) \
                tags         [json_to_dict $row(tags)] \
                updated_at   $row(updated_at)]
            lappend json_items [dict_to_json $item -deep]
        }
        
        set json_array "\[[join $json_items ,]\]"
        dservSet configs/archived $json_array
    }
    
    proc publish_tags {} {
        set tags [get_all_tags]
        dservSet configs/tags [list_to_json $tags]
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

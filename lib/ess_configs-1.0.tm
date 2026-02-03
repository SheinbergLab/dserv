# -*- mode: tcl -*-
#
# ess_configs-1_0.tm
#
# Configuration library for ESS - provides persistent storage and retrieval
# of experiment configurations ("snapshots" of system/protocol/variant setups).
#
# Key concepts:
#   - Configs belong to exactly one Project
#   - Configs include file_template for datafile naming
#   - Configs can be "loaded" (setup only) or "run" (setup + file + start)
#
# This module is loaded by the configs subprocess and provides:
#   - SQLite database for config storage
#   - CRUD operations (save, load, run, list, get, update, delete)
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
    
    # Current schema version - increment when schema changes incompatibly
    # Version 2: Project-based configs/queues structure (Feb 2026)
    variable SCHEMA_VERSION 2
    
    #=========================================================================
    # Initialization
    #=========================================================================
    
    proc get_db {} {
        variable db
        return $db
    }
    
    proc init {path} {
        variable db
        variable db_path
        variable initialized
        variable SCHEMA_VERSION
        
        if {$initialized} {
            log warning "ess::configs already initialized"
            return
        }
        
        set db_path $path
        
        # Check if database exists and has outdated schema
        if {[file exists $path]} {
            sqlite3 configdb_check $path
            set db_version [configdb_check eval {PRAGMA user_version}]
            configdb_check close
            
            if {$db_version > 0 && $db_version < $SCHEMA_VERSION} {
                log info "Database schema version $db_version is outdated (current: $SCHEMA_VERSION), resetting database"
                file delete $path
                # Also delete WAL and SHM files if they exist
                catch { file delete "${path}-wal" }
                catch { file delete "${path}-shm" }
            }
        }
        
        sqlite3 configdb $path
        set db "configdb"
        
        configdb eval { PRAGMA journal_mode=WAL }
        configdb eval { PRAGMA busy_timeout=5000 }
        configdb eval { PRAGMA foreign_keys=ON }
        
        init_schema
        
        set initialized 1
        log info "ess::configs initialized: $path (schema v$SCHEMA_VERSION)"
    }
    
    proc init_schema {} {
        variable SCHEMA_VERSION
        
        configdb eval {
            -- Projects table
            CREATE TABLE IF NOT EXISTS projects (
                id INTEGER PRIMARY KEY,
                name TEXT UNIQUE NOT NULL,
                description TEXT DEFAULT '',
                systems TEXT DEFAULT '[]',
                registry_url TEXT DEFAULT '',
                last_sync_at INTEGER,
                created_at INTEGER DEFAULT (strftime('%s', 'now')),
                updated_at INTEGER DEFAULT (strftime('%s', 'now'))
            );
            
            -- Configs table - belongs to exactly one project
            CREATE TABLE IF NOT EXISTS configs (
                id INTEGER PRIMARY KEY,
                project_id INTEGER NOT NULL REFERENCES projects(id),
                
                name TEXT NOT NULL,
                description TEXT DEFAULT '',
                
                -- ESS setup fields
                script_source TEXT DEFAULT '',
                system TEXT NOT NULL,
                protocol TEXT NOT NULL,
                variant TEXT NOT NULL,
                subject TEXT DEFAULT '',
                variant_args TEXT DEFAULT '{}',
                params TEXT DEFAULT '{}',
                
                -- File naming template
                file_template TEXT DEFAULT '',
                
                -- Metadata
                tags TEXT DEFAULT '[]',
                created_by TEXT DEFAULT '',
                created_at INTEGER DEFAULT (strftime('%s', 'now')),
                updated_at INTEGER DEFAULT (strftime('%s', 'now')),
                last_used_at INTEGER,
                use_count INTEGER DEFAULT 0,
                archived INTEGER DEFAULT 0,
                
                -- Unique name within project for active configs
                UNIQUE(project_id, name) 
            );
            
            CREATE INDEX IF NOT EXISTS idx_configs_project ON configs(project_id);
            CREATE INDEX IF NOT EXISTS idx_configs_system ON configs(system, protocol, variant);
            CREATE INDEX IF NOT EXISTS idx_configs_archived ON configs(archived);
            
            -- Queues table - belongs to exactly one project
            CREATE TABLE IF NOT EXISTS queues (
                id INTEGER PRIMARY KEY,
                project_id INTEGER NOT NULL REFERENCES projects(id),
                
                name TEXT NOT NULL,
                description TEXT DEFAULT '',
                
                auto_start INTEGER DEFAULT 1,
                auto_advance INTEGER DEFAULT 1,
                auto_datafile INTEGER DEFAULT 1,
                
                created_at INTEGER DEFAULT (strftime('%s', 'now')),
                created_by TEXT DEFAULT '',
                updated_at INTEGER DEFAULT (strftime('%s', 'now')),
                
                UNIQUE(project_id, name)
            );
            
            CREATE INDEX IF NOT EXISTS idx_queues_project ON queues(project_id);
            
            -- Queue items - references config by ID
            CREATE TABLE IF NOT EXISTS queue_items (
                id INTEGER PRIMARY KEY,
                queue_id INTEGER NOT NULL REFERENCES queues(id) ON DELETE CASCADE,
                config_id INTEGER NOT NULL REFERENCES configs(id),
                
                position INTEGER NOT NULL,
                repeat_count INTEGER DEFAULT 1,
                pause_after INTEGER DEFAULT 0,
                notes TEXT DEFAULT '',
                
                UNIQUE(queue_id, position)
            );
            
            CREATE INDEX IF NOT EXISTS idx_queue_items_queue ON queue_items(queue_id, position);
            CREATE INDEX IF NOT EXISTS idx_queue_items_config ON queue_items(config_id);
        }
        
        if {[schema_version] == 0} {
            set_schema_version $SCHEMA_VERSION
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
    # Schema Helpers
    #=========================================================================
    
    proc schema_version {} {
        configdb eval {PRAGMA user_version}
    }
    
    proc set_schema_version {v} {
        configdb eval "PRAGMA user_version = $v"
    }
    
    proc table_exists {table} {
        set count [configdb eval {
            SELECT COUNT(*) FROM sqlite_master 
            WHERE type='table' AND name=:table
        }]
        return [expr {$count > 0}]
    }
    
    #=========================================================================
    # Project Operations
    #=========================================================================
    
    variable active_project ""
    
    proc project_create {name args} {
        set description ""
        set systems {}
        
        foreach {k v} $args {
            switch -- $k {
                -description { set description $v }
                -systems { set systems $v }
            }
        }
        
        set systems_json [tags_to_json $systems]
        
        configdb eval {
            INSERT INTO projects (name, description, systems)
            VALUES (:name, :description, :systems_json)
        }
        
        set id [configdb last_insert_rowid]
        log info "Created project: $name (id=$id)"
        
        publish_projects
        return $id
    }
    
    proc project_get {name} {
        set result ""
        configdb eval {
            SELECT id, name, description, systems, registry_url,
                   last_sync_at, created_at, updated_at
            FROM projects WHERE name = :name
        } row {
            # Count configs and queues
            set config_count [configdb onecolumn {
                SELECT COUNT(*) FROM configs 
                WHERE project_id = :row(id) AND archived = 0
            }]
            set queue_count [configdb onecolumn {
                SELECT COUNT(*) FROM queues WHERE project_id = :row(id)
            }]
            
            set result [dict create \
                id $row(id) \
                name $row(name) \
                description $row(description) \
                systems [json_to_dict $row(systems)] \
                registry_url $row(registry_url) \
                last_sync_at $row(last_sync_at) \
                config_count $config_count \
                queue_count $queue_count \
                created_at $row(created_at) \
                updated_at $row(updated_at)]
        }
        return $result
    }
    
    proc project_list {} {
        set results {}
        configdb eval {
            SELECT id, name, description, systems
            FROM projects ORDER BY name
        } row {
            set config_count [configdb onecolumn {
                SELECT COUNT(*) FROM configs 
                WHERE project_id = :row(id) AND archived = 0
            }]
            set queue_count [configdb onecolumn {
                SELECT COUNT(*) FROM queues WHERE project_id = :row(id)
            }]
            
            lappend results [dict create \
                id $row(id) \
                name $row(name) \
                description $row(description) \
                systems [json_to_dict $row(systems)] \
                config_count $config_count \
                queue_count $queue_count]
        }
        return $results
    }
    
    proc project_update {name args} {
        set id [project_get_id $name]
        
        foreach {k v} $args {
            switch -- $k {
                -description {
                    configdb eval {
                        UPDATE projects SET description = :v, 
                        updated_at = strftime('%s','now') WHERE id = :id
                    }
                }
                -systems {
                    set json [tags_to_json $v]
                    configdb eval {
                        UPDATE projects SET systems = :json,
                        updated_at = strftime('%s','now') WHERE id = :id
                    }
                }
                -name {
                    configdb eval {
                        UPDATE projects SET name = :v,
                        updated_at = strftime('%s','now') WHERE id = :id
                    }
                    set name $v
                }
                -registry_url {
                    configdb eval {
                        UPDATE projects SET registry_url = :v,
                        updated_at = strftime('%s','now') WHERE id = :id
                    }
                }
            }
        }
        
        publish_projects
        return $name
    }

    proc project_delete {name} {
	set id [project_get_id $name]
	
	# Delete in order: queue_items -> queues -> configs -> project
	configdb eval {DELETE FROM queue_items WHERE queue_id IN (SELECT id FROM queues WHERE project_id = :id)}
	configdb eval {DELETE FROM queues WHERE project_id = :id}
	configdb eval {DELETE FROM configs WHERE project_id = :id}
	configdb eval {DELETE FROM projects WHERE id = :id}
	
	# Clear active if this was it
	variable active_project
	if {$active_project eq $name} {
	    set active_project ""
	    dservSet projects/active ""
	}
	
	log info "Deleted project: $name"
	publish_projects
    }
    
    proc project_exists {name} {
        set count [configdb onecolumn {
            SELECT COUNT(*) FROM projects WHERE name = :name
        }]
        return [expr {$count > 0}]
    }
    
    proc project_get_id {name} {
        set id [configdb onecolumn {SELECT id FROM projects WHERE name = :name}]
        if {$id eq ""} {
            error "Project not found: $name"
        }
        return $id
    }
    
    proc project_activate {name} {
        variable active_project
        
        if {$name ne "" && ![project_exists $name]} {
            error "Project not found: $name"
        }
        
        set active_project $name
        dservSet projects/active $name
        
        log info "Activated project: $name"
        publish_configs
        publish_queues
    }
    
    proc project_deactivate {} {
        project_activate ""
    }
    
    proc project_active {} {
        variable active_project
        return $active_project
    }
    
    #=========================================================================
    # Config Save Operations
    #=========================================================================
    
    # Save current ESS state as a named config
    proc save_current {name args} {
        variable active_project
        
        # Parse options
        set description ""
        set tags {}
        set created_by ""
        set file_template ""
        set project ""
        
        foreach {key value} $args {
            switch -- $key {
                -description { set description $value }
                -tags        { set tags $value }
                -created_by  { set created_by $value }
                -file_template { set file_template $value }
                -project     { set project $value }
            }
        }
        
        # Determine project
        if {$project eq ""} {
            set project $active_project
        }
        if {$project eq ""} {
            error "No project specified and no active project"
        }
        
        set project_id [project_get_id $project]
        
        if {$created_by eq ""} {
            set created_by [get_current_user]
        }
        
        # Capture current config from ESS
        set script_source [dservGet ess/project]
        set system        [dservGet ess/system]
        set protocol      [dservGet ess/protocol]
        set variant       [dservGet ess/variant]
        set subject       [dservGet ess/subject]
        
        if {$system eq ""} {
            error "No system loaded - cannot save config"
        }
        
        # Get variant_args
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
        
        # Convert to JSON
        set variant_args_json [dict_to_json $variant_args]
        set params_json [dict_to_json $params]
        set tags_json [tags_to_json $tags]
        
        # Check if name exists in this project
        set existing_id [configdb onecolumn { 
            SELECT id FROM configs 
            WHERE project_id = :project_id AND name = :name AND archived = 0
        }]
        
        if {$existing_id ne ""} {
            # Update existing
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
                    file_template = :file_template,
                    updated_at = strftime('%s', 'now')
                WHERE id = :existing_id
            }
            set config_id $existing_id
            log info "Updated config: $name (id=$config_id)"
        } else {
            # Insert new
            configdb eval {
                INSERT INTO configs 
                    (project_id, name, description, script_source, system, protocol, 
                     variant, subject, variant_args, params, tags, file_template, created_by)
                VALUES 
                    (:project_id, :name, :description, :script_source, :system, :protocol,
                     :variant, :subject, :variant_args_json, :params_json, :tags_json, 
                     :file_template, :created_by)
            }
            set config_id [configdb last_insert_rowid]
            log info "Created config: $name (id=$config_id) in project $project"
        }
        
        publish_configs
        return $config_id
    }
    
    # Create config from explicit values
    proc create {project name system protocol variant args} {
        set description ""
        set script_source ""
        set subject ""
        set variant_args {}
        set params {}
        set tags {}
        set created_by ""
        set file_template ""
        
        foreach {key value} $args {
            switch -- $key {
                -description   { set description $value }
                -script_source { set script_source $value }
                -subject       { set subject $value }
                -variant_args  { set variant_args $value }
                -params        { set params $value }
                -tags          { set tags $value }
                -created_by    { set created_by $value }
                -file_template { set file_template $value }
            }
        }
        
        set project_id [project_get_id $project]
        
        if {$created_by eq ""} {
            set created_by [get_current_user]
        }
        
        set variant_args_json [dict_to_json $variant_args]
        set params_json [dict_to_json $params]
        set tags_json [tags_to_json $tags]
        
        configdb eval {
            INSERT INTO configs 
                (project_id, name, description, script_source, system, protocol, 
                 variant, subject, variant_args, params, tags, file_template, created_by)
            VALUES 
                (:project_id, :name, :description, :script_source, :system, :protocol,
                 :variant, :subject, :variant_args_json, :params_json, :tags_json,
                 :file_template, :created_by)
        }
        
        set config_id [configdb last_insert_rowid]
        log info "Created config: $name (id=$config_id)"
        
        publish_configs
        return $config_id
    }
    
    #=========================================================================
    # Config Load and Run Operations
    #=========================================================================
    
    # Load a config into ESS (setup only, no file, no start)
    proc load {name_or_id args} {
        set project ""
        foreach {k v} $args {
            switch -- $k {
                -project { set project $v }
            }
        }
        
        set config [get $name_or_id -project $project]
        
        if {$config eq ""} {
            error "Config not found: $name_or_id"
        }
        
        # Build ESS config dict
        set ess_config [dict create \
            script_source [dict get $config script_source] \
            system        [dict get $config system] \
            protocol      [dict get $config protocol] \
            variant       [dict get $config variant] \
            subject       [dict get $config subject] \
            variant_args  [dict get $config variant_args] \
            params        [dict get $config params]]

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
        
        # Publish current config
        dservSet configs/current [dict_to_json [dict create \
            id   $config_id \
            name [dict get $config name] \
            project [dict get $config project_name]]]
        
        publish_configs
        return $result
    }
    
    # Run a config (load + open file + start)
    proc run {name_or_id args} {
        set project ""
        set auto_start 1
        
        foreach {k v} $args {
            switch -- $k {
                -project { set project $v }
                -auto_start { set auto_start $v }
            }
        }
        
        set config [get $name_or_id -project $project]
        if {$config eq ""} {
            error "Config not found: $name_or_id"
        }
        
        # Load the config
        load $name_or_id -project $project
        
        # Generate and open datafile
        set basename [generate_file_basename $config]
        log info "Opening datafile: $basename"
        
        set result [send ess "::ess::file_open {$basename}"]
        if {$result == 0} {
            error "Datafile already exists: $basename"
        } elseif {$result == -1} {
            error "Another datafile is already open"
        }
        
        dservSet configs/datafile $basename
        
        # Start if requested
        if {$auto_start} {
            log info "Starting ESS"
            send ess "ess::start"
        }
        
        return $basename
    }
    
    # Generate filename from config's template
    proc generate_file_basename {config} {
        set template [dict get $config file_template]
        
        # If empty, use ESS default
        if {$template eq ""} {
            set template "{subject}_{config}_{date_short}{time}"
        }
        
        # Get values
        set subject [dict get $config subject]
        if {$subject eq ""} { set subject "unknown" }
        
        set system [dict get $config system]
        set protocol [dict get $config protocol]
        set variant [dict get $config variant]
        set config_name [dict get $config name]
        set project_name [dict get $config project_name]
        
        # Timestamps
        set now [clock seconds]
        set date [clock format $now -format "%Y%m%d"]
        set date_short [clock format $now -format "%y%m%d"]
        set time [clock format $now -format "%H%M%S"]
        set time_short [clock format $now -format "%H%M"]
        
        # Clean for filename
        set config_clean [regsub -all {[^a-zA-Z0-9_-]} $config_name "_"]
        set subject_clean [regsub -all {[^a-zA-Z0-9_-]} $subject "_"]
        set project_clean [regsub -all {[^a-zA-Z0-9_-]} $project_name "_"]

        # Substitution map
        set subst_map     "{{subject}}    $subject_clean \
    		     	   {{system}}     $system \
			   {{protocol}}   $protocol \
			   {{variant}}    $variant \
			   {{config}}     $config_clean \
			   {{project}}    $project_clean \
			   {{date}}       $date \
			   {{date_short}} $date_short \
			   {{time}}       $time \
			   {{time_short}} $time_short \
			   {{timestamp}}  $now"
		       
	set name [string map $subst_map $template]
        return $name
    }

    
    #=========================================================================
    # Config Query Operations
    #=========================================================================
    
    proc get {name_or_id args} {
        variable active_project
        
        set project ""
        foreach {k v} $args {
            switch -- $k {
                -project { set project $v }
            }
        }
        
        # Build query based on whether we have ID or name
        if {[string is integer $name_or_id]} {
            set where_clause "c.id = :name_or_id"
        } else {
            # Name lookup - need project context
            if {$project eq ""} {
                set project $active_project
            }
            if {$project eq ""} {
                error "Config lookup by name requires project context"
            }
            set project_id [project_get_id $project]
            set where_clause "c.name = :name_or_id AND c.project_id = :project_id"
        }
        
        set result ""
        configdb eval "
            SELECT c.id, c.project_id, p.name as project_name,
                   c.name, c.description, c.script_source, 
                   c.system, c.protocol, c.variant, c.subject,
                   c.variant_args, c.params, c.file_template,
                   c.tags, c.created_by, c.created_at, c.updated_at,
                   c.last_used_at, c.use_count, c.archived
            FROM configs c
            JOIN projects p ON p.id = c.project_id
            WHERE $where_clause AND c.archived = 0
        " row {
            set result [dict create \
                id            $row(id) \
                project_id    $row(project_id) \
                project_name  $row(project_name) \
                name          $row(name) \
                description   $row(description) \
                script_source $row(script_source) \
                system        $row(system) \
                protocol      $row(protocol) \
                variant       $row(variant) \
                subject       $row(subject) \
                variant_args  [json_to_dict $row(variant_args)] \
                params        [json_to_dict $row(params)] \
                file_template $row(file_template) \
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
    
    proc exists {name_or_id args} {
        set config [get $name_or_id {*}$args]
        return [expr {$config ne ""}]
    }
    
    proc list {args} {
        variable active_project
        
        set project ""
        set tags {}
        set system ""
        set search ""
        set limit 100
        set all 0
        
        foreach {key value} $args {
            switch -- $key {
                -project  { set project $value }
                -tags     { set tags $value }
                -system   { set system $value }
                -search   { set search $value }
                -limit    { set limit $value }
                -all      { set all $value }
            }
        }
        
        set where_clauses {"c.archived = 0"}
        
        # Project filter
        if {!$all} {
            if {$project eq ""} {
                set project $active_project
            }
            if {$project ne ""} {
                set project_id [project_get_id $project]
                lappend where_clauses "c.project_id = :project_id"
            }
        }
        
        if {$system ne ""} {
            lappend where_clauses "c.system = :system"
        }
        
        if {$search ne ""} {
            lappend where_clauses "(c.name LIKE '%' || :search || '%' OR c.description LIKE '%' || :search || '%')"
        }
        
        foreach tag $tags {
            lappend where_clauses "c.tags LIKE '%\"$tag\"%'"
        }
        
        set where_sql "WHERE [join $where_clauses { AND }]"
        
        set results {}
        configdb eval "
            SELECT c.id, c.project_id, p.name as project_name,
                   c.name, c.description, c.system, c.protocol, c.variant,
                   c.subject, c.tags, c.file_template, c.use_count, c.last_used_at
            FROM configs c
            JOIN projects p ON p.id = c.project_id
            $where_sql
            ORDER BY c.use_count DESC, c.last_used_at DESC
            LIMIT :limit
        " row {
            lappend results [dict create \
                id           $row(id) \
                project_id   $row(project_id) \
                project_name $row(project_name) \
                name         $row(name) \
                description  $row(description) \
                system       $row(system) \
                protocol     $row(protocol) \
                variant      $row(variant) \
                subject      $row(subject) \
                tags         [json_to_dict $row(tags)] \
                file_template $row(file_template) \
                use_count    $row(use_count) \
                last_used_at $row(last_used_at)]
        }
        
        return $results
    }
    
    #=========================================================================
    # Config Update/Delete Operations
    #=========================================================================
    
    proc update {name_or_id args} {
        # Extract project option first
        set project ""
        set update_args {}
        foreach {k v} $args {
            if {$k eq "-project"} {
                set project $v
            } else {
                lappend update_args $k $v
            }
        }
        
        set config [get $name_or_id -project $project]
        if {$config eq ""} {
            error "Config not found: $name_or_id"
        }
        
        set config_id [dict get $config id]
        
        foreach {key value} $update_args {
            switch -- $key {
                -name {
                    configdb eval {
                        UPDATE configs SET name = :value, 
                        updated_at = strftime('%s','now') WHERE id = :config_id
                    }
                }
                -description {
                    configdb eval {
                        UPDATE configs SET description = :value,
                        updated_at = strftime('%s','now') WHERE id = :config_id
                    }
                }
                -subject {
                    configdb eval {
                        UPDATE configs SET subject = :value,
                        updated_at = strftime('%s','now') WHERE id = :config_id
                    }
                }
                -tags {
                    set json [tags_to_json $value]
                    configdb eval {
                        UPDATE configs SET tags = :json,
                        updated_at = strftime('%s','now') WHERE id = :config_id
                    }
                }
                -file_template {
                    configdb eval {
                        UPDATE configs SET file_template = :value,
                        updated_at = strftime('%s','now') WHERE id = :config_id
                    }
                }
                -variant_args {
                    set json [dict_to_json $value]
                    configdb eval {
                        UPDATE configs SET variant_args = :json,
                        updated_at = strftime('%s','now') WHERE id = :config_id
                    }
                }
                -params {
                    set json [dict_to_json $value]
                    configdb eval {
                        UPDATE configs SET params = :json,
                        updated_at = strftime('%s','now') WHERE id = :config_id
                    }
                }
            }
        }
        
        publish_configs
        return $config_id
    }
    
    proc archive {name_or_id args} {
        set project ""
        foreach {k v} $args {
            if {$k eq "-project"} { set project $v }
        }
        
        set config [get $name_or_id -project $project]
        if {$config eq ""} {
            error "Config not found: $name_or_id"
        }
        
        set config_id [dict get $config id]
        
        # Check if in any queue
        set in_queues [configdb onecolumn {
            SELECT COUNT(*) FROM queue_items WHERE config_id = :config_id
        }]
        if {$in_queues > 0} {
            error "Cannot archive config that is in $in_queues queue(s)"
        }

	set old_name [dict get $config name]
        set archived_name "${old_name}_archived_[clock seconds]"
        
        configdb eval {
            UPDATE configs SET archived = 1, name = :archived_name,
            updated_at = strftime('%s','now') WHERE id = :config_id
        }	
        
        log info "Archived config: [dict get $config name]"
        publish_configs
        return $config_id
    }
    
    proc delete {name_or_id args} {
        set project ""
        foreach {k v} $args {
            if {$k eq "-project"} { set project $v }
        }
        
        set config [get $name_or_id -project $project]
        if {$config eq ""} {
            error "Config not found: $name_or_id"
        }
        
        set config_id [dict get $config id]
        
        # Check if in any queue
        set in_queues [configdb onecolumn {
            SELECT COUNT(*) FROM queue_items WHERE config_id = :config_id
        }]
        if {$in_queues > 0} {
            error "Cannot delete config that is in $in_queues queue(s)"
        }
        
        configdb eval {DELETE FROM configs WHERE id = :config_id}
        
        log info "Deleted config: [dict get $config name]"
        publish_configs
        return $config_id
    }
    
    proc clone {name_or_id new_name args} {
        set project ""
        set to_project ""
        set overrides {}
        
        foreach {k v} $args {
            switch -- $k {
                -project { set project $v }
                -to_project { set to_project $v }
                default { lappend overrides $k $v }
            }
        }
        
        set config [get $name_or_id -project $project]
        if {$config eq ""} {
            error "Config not found: $name_or_id"
        }
        
        # Target project
        if {$to_project eq ""} {
            set to_project [dict get $config project_name]
        }
        
        # Start with source values
        set new_config $config
        dict set new_config name $new_name
        dict unset new_config id
        dict unset new_config project_id
        dict unset new_config project_name
        dict unset new_config created_at
        dict unset new_config updated_at
        dict unset new_config last_used_at
        dict unset new_config use_count
        dict unset new_config archived
        
        # Apply overrides
        foreach {k v} $overrides {
            set field [string range $k 1 end]  ;# Remove leading -
            if {$field in {description subject tags file_template variant_args params}} {
                dict set new_config $field $v
            }
        }
        
        # Create
        set new_id [create $to_project $new_name \
            [dict get $new_config system] \
            [dict get $new_config protocol] \
            [dict get $new_config variant] \
            -description [dict get $new_config description] \
            -script_source [dict get $new_config script_source] \
            -subject [dict get $new_config subject] \
            -variant_args [dict get $new_config variant_args] \
            -params [dict get $new_config params] \
            -tags [dict get $new_config tags] \
            -file_template [dict get $new_config file_template]]
        
        log info "Cloned config [dict get $config name] -> $new_name"
        return $new_id
    }
    
    #=========================================================================
    # Publishing for Reactive UIs
    #=========================================================================
    
    proc publish_projects {} {
        set obj [yajl create #auto]
        $obj array_open
        
        foreach proj [project_list] {
            $obj map_open
            $obj string "id" number [dict get $proj id]
            $obj string "name" string [dict get $proj name]
            $obj string "description" string [dict get $proj description]
            $obj string "config_count" number [dict get $proj config_count]
            $obj string "queue_count" number [dict get $proj queue_count]
            $obj map_close
        }
        
        $obj array_close
        set result [$obj get]
        $obj delete
        
        dservSet projects/list $result
    }
    
    proc publish_configs {} {
        variable active_project
        
        set obj [yajl create #auto]
        $obj array_open
        
        set configs [list -limit 100]
        if {$active_project ne ""} {
            set configs [list -project $active_project -limit 100]
        }
        
        foreach cfg $configs {
            $obj map_open
            $obj string "id" number [dict get $cfg id]
            $obj string "name" string [dict get $cfg name]
            $obj string "project" string [dict get $cfg project_name]
            $obj string "description" string [dict get $cfg description]
            $obj string "system" string [dict get $cfg system]
            $obj string "protocol" string [dict get $cfg protocol]
            $obj string "variant" string [dict get $cfg variant]
            $obj string "subject" string [dict get $cfg subject]
            $obj string "file_template" string [dict get $cfg file_template]
            $obj string "use_count" number [dict get $cfg use_count]
            
            $obj string "tags" array_open
            foreach tag [dict get $cfg tags] {
                $obj string $tag
            }
            $obj array_close
            
            $obj map_close
        }
        
        $obj array_close
        set result [$obj get]
        $obj delete
        
        dservSet configs/list $result
    }
    
    proc publish_queues {} {
        # Delegate to queue module if it exists
        if {[namespace exists ::ess_queues]} {
            ::ess_queues::publish_list
        }
    }
    
    proc publish_all {} {
        publish_projects
        publish_configs
        publish_queues
    }
    
    #=========================================================================
    # Helpers
    #=========================================================================
    
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
    
    proc log {level msg} {
        if {[catch {dservSet configs/log "\[$level\] $msg"}]} {
            puts "ess::configs $level: $msg"
        }
    }

}

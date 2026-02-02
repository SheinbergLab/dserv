# ess_projects-1_0.tm -- Project management for ESS
#
# Projects are organizational containers that group:
#   - Systems (references to what systems/protocols are used)
#   - Configs (saved parameter snapshots)
#   - Queues (run sequences)
#
# Projects enable:
#   - UI filtering to focus on relevant items
#   - Sync unit for pushing/pulling to registry or other rigs
#   - Documentation of what an experiment needs
#
# Part of the configs subprocess - shares database with ess_configs/ess_queues.
#

package require sqlite3
package require tcljson
package provide ess_projects 1.0

namespace eval ::ess_projects {
    
    # Database handle (shared with ess_configs)
    variable db ""
    variable initialized 0
    
    # Default project name - always exists, contains unassigned items
    variable DEFAULT_PROJECT "_default"
    
    #=========================================================================
    # Initialization
    #=========================================================================
    
    proc init {db_handle} {
        variable db
        variable initialized
        variable DEFAULT_PROJECT
        
        if {$initialized} {
            return
        }
        
        set db $db_handle
        create_tables
        
        # Ensure default project exists
        if {![exists $DEFAULT_PROJECT]} {
            create $DEFAULT_PROJECT \
                -description "Default project - unassigned configs and queues" \
                -systems {}
        }
        
        set initialized 1
        log info "ess_projects initialized"
    }
    
    proc create_tables {} {
        variable db
        
        $db eval {
            -- Projects table
            CREATE TABLE IF NOT EXISTS projects (
                id INTEGER PRIMARY KEY,
                name TEXT UNIQUE NOT NULL,
                description TEXT DEFAULT '',
                systems TEXT DEFAULT '[]',
                auto_sync INTEGER DEFAULT 0,
                sync_interval INTEGER DEFAULT 3600,
                last_sync_at INTEGER,
                registry_workgroup TEXT,
                registry_project TEXT,
                created_at INTEGER DEFAULT (strftime('%s', 'now')),
                updated_at INTEGER DEFAULT (strftime('%s', 'now'))
            );
            
            -- Project-config membership
            CREATE TABLE IF NOT EXISTS project_configs (
                project_id INTEGER NOT NULL,
                config_id INTEGER NOT NULL,
                added_at INTEGER DEFAULT (strftime('%s', 'now')),
                PRIMARY KEY (project_id, config_id),
                FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE,
                FOREIGN KEY (config_id) REFERENCES configs(id) ON DELETE CASCADE
            );
            
            -- Project-queue membership  
            CREATE TABLE IF NOT EXISTS project_queues (
                project_id INTEGER NOT NULL,
                queue_id INTEGER NOT NULL,
                added_at INTEGER DEFAULT (strftime('%s', 'now')),
                PRIMARY KEY (project_id, queue_id),
                FOREIGN KEY (project_id) REFERENCES projects(id) ON DELETE CASCADE,
                FOREIGN KEY (queue_id) REFERENCES queues(id) ON DELETE CASCADE
            );
            
            -- Indexes
            CREATE INDEX IF NOT EXISTS idx_project_configs_project 
                ON project_configs(project_id);
            CREATE INDEX IF NOT EXISTS idx_project_configs_config 
                ON project_configs(config_id);
            CREATE INDEX IF NOT EXISTS idx_project_queues_project 
                ON project_queues(project_id);
            CREATE INDEX IF NOT EXISTS idx_project_queues_queue 
                ON project_queues(queue_id);
        }
    }
    
    #=========================================================================
    # CRUD Operations
    #=========================================================================
    
    # Create a new project
    # Usage: create "name" ?-description "text"? ?-systems {sys1 sys2}?
    proc create {name args} {
        variable db
        
        set description ""
        set systems {}
        
        foreach {k v} $args {
            switch -- $k {
                -description { set description $v }
                -systems { set systems $v }
            }
        }
        
        set systems_json [list_to_json $systems]
        
        $db eval {
            INSERT INTO projects (name, description, systems)
            VALUES (:name, :description, :systems_json)
        }
        
        set id [$db last_insert_rowid]
        log info "Created project: $name (id=$id)"
        
        publish_list
        return $id
    }
    
    # Get project by name
    proc get {name} {
        variable db
        
        set id [get_id_safe $name]
        if {$id eq ""} {
            return ""
        }
        
        set result ""
        $db eval {
            SELECT id, name, description, systems, auto_sync, sync_interval,
                   last_sync_at, registry_workgroup, registry_project,
                   created_at, updated_at
            FROM projects WHERE id = :id
        } row {
            # Get associated config names
            set config_names [$db eval {
                SELECT c.name FROM configs c
                JOIN project_configs pc ON pc.config_id = c.id
                WHERE pc.project_id = :id AND c.archived = 0
                ORDER BY c.name
            }]
            
            # Get associated queue names
            set queue_names [$db eval {
                SELECT q.name FROM queues q
                JOIN project_queues pq ON pq.queue_id = q.id
                WHERE pq.project_id = :id
                ORDER BY q.name
            }]
            
            set result [dict create \
                id $row(id) \
                name $row(name) \
                description $row(description) \
                systems [json_to_list $row(systems)] \
                auto_sync $row(auto_sync) \
                sync_interval $row(sync_interval) \
                last_sync_at $row(last_sync_at) \
                registry_workgroup $row(registry_workgroup) \
                registry_project $row(registry_project) \
                configs $config_names \
                queues $queue_names \
                created_at $row(created_at) \
                updated_at $row(updated_at)]
        }
        
        return $result
    }
    
    # List all projects
    proc list {} {
        variable db
        
        set results {}
        $db eval {
            SELECT id, name, description, systems, auto_sync
            FROM projects 
            ORDER BY 
                CASE WHEN name = '_default' THEN 1 ELSE 0 END,
                name
        } row {
            # Count configs and queues
            set config_count [$db onecolumn {
                SELECT COUNT(*) FROM project_configs pc
                JOIN configs c ON c.id = pc.config_id
                WHERE pc.project_id = :row(id) AND c.archived = 0
            }]
            set queue_count [$db onecolumn {
                SELECT COUNT(*) FROM project_queues
                WHERE project_id = :row(id)
            }]
            
            lappend results [dict create \
                id $row(id) \
                name $row(name) \
                description $row(description) \
                systems [json_to_list $row(systems)] \
                auto_sync $row(auto_sync) \
                config_count $config_count \
                queue_count $queue_count]
        }
        
        return $results
    }
    
    # Update project
    # Usage: update "name" ?-description "text"? ?-systems {sys1 sys2}? ?-name "newname"?
    proc update {name args} {
        variable db
        variable DEFAULT_PROJECT
        
        set id [get_id $name]
        
        # Parse what to update
        set updates {}
        set new_name ""
        
        foreach {k v} $args {
            switch -- $k {
                -description { dict set updates description $v }
                -systems { dict set updates systems [list_to_json $v] }
                -auto_sync { dict set updates auto_sync $v }
                -sync_interval { dict set updates sync_interval $v }
                -registry_workgroup { dict set updates registry_workgroup $v }
                -registry_project { dict set updates registry_project $v }
                -name { 
                    if {$name eq $DEFAULT_PROJECT} {
                        error "Cannot rename default project"
                    }
                    set new_name $v 
                }
            }
        }
        
        # Build update query
        if {[dict size $updates] > 0} {
            set clauses {}
            foreach {col val} $updates {
                lappend clauses "$col = :val_$col"
                set val_$col $val
            }
            lappend clauses "updated_at = strftime('%s', 'now')"
            
            set sql "UPDATE projects SET [join $clauses ", "] WHERE id = :id"
            $db eval $sql
        }
        
        # Handle rename separately
        if {$new_name ne ""} {
            $db eval {
                UPDATE projects SET name = :new_name, updated_at = strftime('%s', 'now')
                WHERE id = :id
            }
        }
        
        log info "Updated project: $name"
        publish_list
    }
    
    # Delete project (moves configs/queues to default project)
    proc delete {name} {
        variable db
        variable DEFAULT_PROJECT
        
        if {$name eq $DEFAULT_PROJECT} {
            error "Cannot delete default project"
        }
        
        set id [get_id $name]
        set default_id [get_id $DEFAULT_PROJECT]
        
        # Move configs to default project
        $db eval {
            INSERT OR IGNORE INTO project_configs (project_id, config_id)
            SELECT :default_id, config_id FROM project_configs WHERE project_id = :id
        }
        
        # Move queues to default project
        $db eval {
            INSERT OR IGNORE INTO project_queues (project_id, queue_id)
            SELECT :default_id, queue_id FROM project_queues WHERE project_id = :id
        }
        
        # Delete the project (cascades to remove old memberships)
        $db eval {DELETE FROM projects WHERE id = :id}
        
        log info "Deleted project: $name (items moved to default)"
        publish_list
    }
    
    # Check if project exists
    proc exists {name} {
        variable db
        set count [$db onecolumn {SELECT COUNT(*) FROM projects WHERE name = :name}]
        return [expr {$count > 0}]
    }
    
    #=========================================================================
    # Membership Management
    #=========================================================================
    
    # Add config to project
    proc add_config {project_name config_name} {
        variable db
        
        set project_id [get_id $project_name]
        set config_id [get_config_id $config_name]
        
        $db eval {
            INSERT OR IGNORE INTO project_configs (project_id, config_id)
            VALUES (:project_id, :config_id)
        }
        
        log info "Added config '$config_name' to project '$project_name'"
        publish_list
    }
    
    # Add multiple configs to project
    proc add_configs {project_name config_names} {
        foreach name $config_names {
            add_config $project_name $name
        }
    }
    
    # Remove config from project (moves to default if not in any other project)
    proc remove_config {project_name config_name} {
        variable db
        variable DEFAULT_PROJECT
        
        set project_id [get_id $project_name]
        set config_id [get_config_id $config_name]
        
        $db eval {
            DELETE FROM project_configs 
            WHERE project_id = :project_id AND config_id = :config_id
        }
        
        # If config is now orphaned, add to default
        set in_any [$db onecolumn {
            SELECT COUNT(*) FROM project_configs WHERE config_id = :config_id
        }]
        if {$in_any == 0} {
            set default_id [get_id $DEFAULT_PROJECT]
            $db eval {
                INSERT INTO project_configs (project_id, config_id)
                VALUES (:default_id, :config_id)
            }
        }
        
        log info "Removed config '$config_name' from project '$project_name'"
        publish_list
    }
    
    # Move config between projects
    proc move_config {config_name from_project to_project} {
        variable db
        
        set from_id [get_id $from_project]
        set to_id [get_id $to_project]
        set config_id [get_config_id $config_name]
        
        $db eval {
            DELETE FROM project_configs 
            WHERE project_id = :from_id AND config_id = :config_id
        }
        $db eval {
            INSERT OR IGNORE INTO project_configs (project_id, config_id)
            VALUES (:to_id, :config_id)
        }
        
        log info "Moved config '$config_name' from '$from_project' to '$to_project'"
        publish_list
    }
    
    # Add queue to project
    proc add_queue {project_name queue_name} {
        variable db
        
        set project_id [get_id $project_name]
        set queue_id [get_queue_id $queue_name]
        
        $db eval {
            INSERT OR IGNORE INTO project_queues (project_id, queue_id)
            VALUES (:project_id, :queue_id)
        }
        
        log info "Added queue '$queue_name' to project '$project_name'"
        publish_list
    }
    
    # Remove queue from project
    proc remove_queue {project_name queue_name} {
        variable db
        variable DEFAULT_PROJECT
        
        set project_id [get_id $project_name]
        set queue_id [get_queue_id $queue_name]
        
        $db eval {
            DELETE FROM project_queues 
            WHERE project_id = :project_id AND queue_id = :queue_id
        }
        
        # If queue is now orphaned, add to default
        set in_any [$db onecolumn {
            SELECT COUNT(*) FROM project_queues WHERE queue_id = :queue_id
        }]
        if {$in_any == 0} {
            set default_id [get_id $DEFAULT_PROJECT]
            $db eval {
                INSERT INTO project_queues (project_id, queue_id)
                VALUES (:default_id, :queue_id)
            }
        }
        
        log info "Removed queue '$queue_name' from project '$project_name'"
        publish_list
    }
    
    # Get which project(s) a config belongs to
    proc config_projects {config_name} {
        variable db
        set config_id [get_config_id $config_name]
        
        return [$db eval {
            SELECT p.name FROM projects p
            JOIN project_configs pc ON pc.project_id = p.id
            WHERE pc.config_id = :config_id
            ORDER BY p.name
        }]
    }
    
    # Get which project(s) a queue belongs to  
    proc queue_projects {queue_name} {
        variable db
        set queue_id [get_queue_id $queue_name]
        
        return [$db eval {
            SELECT p.name FROM projects p
            JOIN project_queues pq ON pq.project_id = p.id
            WHERE pq.queue_id = :queue_id
            ORDER BY p.name
        }]
    }
    
    #=========================================================================
    # Activation (UI Filtering)
    #=========================================================================
    
    # Activate a project (sets filter context)
    proc activate {name} {
        if {$name ne "" && ![exists $name]} {
            error "Project not found: $name"
        }
        
        dservSet projects/active $name
        publish_active
        
        if {$name eq ""} {
            log info "Deactivated project filter"
        } else {
            log info "Activated project: $name"
        }
    }
    
    # Deactivate (show all)
    proc deactivate {} {
        activate ""
    }
    
    # Get currently active project name
    proc active {} {
        return [dservGet projects/active]
    }
    
    #=========================================================================
    # Validation
    #=========================================================================
    
    # Validate project - check that all required systems/configs/queues exist
    proc validate {name} {
        variable db
        
        set project [get $name]
        if {$project eq ""} {
            return [dict create valid 0 issues {"Project not found"}]
        }
        
        set issues {}
        
        # Check systems exist in ESS_SYSTEM_PATH
        set available_systems [get_available_systems]
        foreach sys [dict get $project systems] {
            if {$sys ni $available_systems} {
                lappend issues "Missing system: $sys"
            }
        }
        
        # Configs and queues are validated by their existence in the join tables
        # (if they're in project_configs/project_queues, they exist)
        
        # Check that configs reference valid systems
        foreach config_name [dict get $project configs] {
            set config [::ess::configs::get $config_name]
            if {$config eq ""} {
                lappend issues "Config missing: $config_name"
            } else {
                set sys [dict get $config system]
                if {$sys ni [dict get $project systems] && [llength [dict get $project systems]] > 0} {
                    lappend issues "Config '$config_name' uses system '$sys' not in project"
                }
            }
        }
        
        if {[llength $issues] == 0} {
            return [dict create valid 1 issues {}]
        } else {
            return [dict create valid 0 issues $issues]
        }
    }
    
    #=========================================================================
    # Export/Import
    #=========================================================================
    
    # Export project with all its configs and queues
    proc export {name} {
        variable db
        
        set project [get $name]
        if {$project eq ""} {
            error "Project not found: $name"
        }
        
        # Gather config exports
        set configs_export {}
        foreach config_name [dict get $project configs] {
            if {[catch {
                lappend configs_export [::ess::configs::export $config_name]
            } err]} {
                log warning "Could not export config '$config_name': $err"
            }
        }
        
        # Gather queue exports (without bundled configs - we have them above)
        set queues_export {}
        foreach queue_name [dict get $project queues] {
            if {[catch {
                lappend queues_export [::ess_queues::queue_export $queue_name]
            } err]} {
                log warning "Could not export queue '$queue_name': $err"
            }
        }
        
        # Build export structure
        set export [dict create \
            project [dict create \
                name [dict get $project name] \
                description [dict get $project description] \
                systems [dict get $project systems] \
                config_names [dict get $project configs] \
                queue_names [dict get $project queues]] \
            configs $configs_export \
            queues $queues_export]
        
        return [dict_to_json_deep $export]
    }
    
    # Import project from JSON
    # Usage: import {json} ?-skip_existing? ?-overwrite?
    proc import {json args} {
        variable db
        variable DEFAULT_PROJECT
        
        set skip_existing 0
        set overwrite 0
        
        foreach arg $args {
            switch -- $arg {
                -skip_existing { set skip_existing 1 }
                -overwrite { set overwrite 1 }
            }
        }
        
        set data [json_to_dict $json]
        
        if {![dict exists $data project]} {
            error "Invalid project export: missing 'project' key"
        }
        
        set proj_def [dict get $data project]
        set proj_name [dict get $proj_def name]
        
        if {$proj_name eq $DEFAULT_PROJECT} {
            error "Cannot import as default project"
        }
        
        # Import configs first
        if {[dict exists $data configs]} {
            foreach config_json [dict get $data configs] {
                if {[catch {
                    set config [json_to_dict $config_json]
                    set cname [dict get $config name]
                    
                    if {[::ess::configs::exists $cname]} {
                        if {$skip_existing} {
                            log info "Skipping existing config: $cname"
                            continue
                        } elseif {$overwrite} {
                            ::ess::configs::delete $cname
                        } else {
                            error "Config exists: $cname"
                        }
                    }
                    ::ess::configs::import $config_json
                    log info "Imported config: $cname"
                } err]} {
                    log warning "Failed to import config: $err"
                    if {!$skip_existing} {
                        error $err
                    }
                }
            }
        }
        
        # Import queues
        if {[dict exists $data queues]} {
            foreach queue_json [dict get $data queues] {
                if {[catch {
                    set queue [json_to_dict $queue_json]
                    set qname [dict get [dict get $queue queue] name]
                    
                    if {[::ess_queues::queue_exists $qname]} {
                        if {$skip_existing} {
                            log info "Skipping existing queue: $qname"
                            continue
                        } elseif {$overwrite} {
                            ::ess_queues::queue_delete $qname
                        } else {
                            error "Queue exists: $qname"
                        }
                    }
                    ::ess_queues::queue_import $queue_json
                    log info "Imported queue: $qname"
                } err]} {
                    log warning "Failed to import queue: $err"
                    if {!$skip_existing} {
                        error $err
                    }
                }
            }
        }
        
        # Create or update project
        if {[exists $proj_name]} {
            if {$overwrite} {
                delete $proj_name
            } elseif {!$skip_existing} {
                error "Project exists: $proj_name"
            } else {
                log info "Skipping existing project: $proj_name"
                return $proj_name
            }
        }
        
        set proj_id [create $proj_name \
            -description [dict_get_default $proj_def description ""] \
            -systems [dict_get_default $proj_def systems {}]]
        
        # Link configs to project
        foreach cname [dict_get_default $proj_def config_names {}] {
            if {[::ess::configs::exists $cname]} {
                add_config $proj_name $cname
            }
        }
        
        # Link queues to project
        foreach qname [dict_get_default $proj_def queue_names {}] {
            if {[::ess_queues::queue_exists $qname]} {
                add_queue $proj_name $qname
            }
        }
        
        log info "Imported project: $proj_name"
        publish_list
        
        return $proj_name
    }
    
    #=========================================================================
    # Publishing for Reactive UIs
    #=========================================================================
    
    proc publish_list {} {
        set projects [list]
        
        foreach p [list] {
            lappend projects [dict_to_json $p]
        }
        
        dservSet projects/list "\[[join $projects ,]\]"
    }
    
    proc publish_active {} {
        set active_name [dservGet projects/active]
        
        if {$active_name ne "" && [exists $active_name]} {
            set project [get $active_name]
            dservSet projects/active_detail [dict_to_json_deep $project]
        } else {
            dservSet projects/active_detail "{}"
        }
    }
    
    proc publish_all {} {
        publish_list
        publish_active
    }
    
    #=========================================================================
    # Auto-assignment of New Configs/Queues
    #=========================================================================
    
    # Call this when a new config is created to assign to active or default project
    proc on_config_created {config_id} {
        variable DEFAULT_PROJECT
        
        set active [active]
        if {$active ne "" && $active ne $DEFAULT_PROJECT} {
            # Add to active project
            set config_name [get_config_name_by_id $config_id]
            add_config $active $config_name
        } else {
            # Add to default project
            variable db
            set default_id [get_id $DEFAULT_PROJECT]
            $db eval {
                INSERT OR IGNORE INTO project_configs (project_id, config_id)
                VALUES (:default_id, :config_id)
            }
        }
    }
    
    # Call this when a new queue is created
    proc on_queue_created {queue_id} {
        variable DEFAULT_PROJECT
        
        set active [active]
        if {$active ne "" && $active ne $DEFAULT_PROJECT} {
            set queue_name [get_queue_name_by_id $queue_id]
            add_queue $active $queue_name
        } else {
            variable db
            set default_id [get_id $DEFAULT_PROJECT]
            $db eval {
                INSERT OR IGNORE INTO project_queues (project_id, queue_id)
                VALUES (:default_id, :queue_id)
            }
        }
    }
    
    #=========================================================================
    # Helper Procedures
    #=========================================================================
    
    proc get_id {name} {
        variable db
        set id [$db onecolumn {SELECT id FROM projects WHERE name = :name}]
        if {$id eq ""} {
            error "Project not found: $name"
        }
        return $id
    }
    
    proc get_id_safe {name} {
        variable db
        return [$db onecolumn {SELECT id FROM projects WHERE name = :name}]
    }
    
    proc get_config_id {name} {
        variable db
        set id [$db onecolumn {SELECT id FROM configs WHERE name = :name AND archived = 0}]
        if {$id eq ""} {
            error "Config not found: $name"
        }
        return $id
    }
    
    proc get_config_name_by_id {id} {
        variable db
        return [$db onecolumn {SELECT name FROM configs WHERE id = :id}]
    }
    
    proc get_queue_id {name} {
        variable db
        set id [$db onecolumn {SELECT id FROM queues WHERE name = :name}]
        if {$id eq ""} {
            error "Queue not found: $name"
        }
        return $id
    }
    
    proc get_queue_name_by_id {id} {
        variable db
        return [$db onecolumn {SELECT name FROM queues WHERE id = :id}]
    }
    
    proc get_available_systems {} {
        # Get systems from ESS_SYSTEM_PATH
        set system_path [dservGet ess/system_path]
        if {$system_path eq ""} {
            return {}
        }
        
        set project_dir [dservGet ess/project]
        if {$project_dir eq ""} {
            set project_dir "ess"
        }
        
        set base [file join $system_path $project_dir]
        if {![file isdirectory $base]} {
            return {}
        }
        
        set systems {}
        foreach dir [glob -nocomplain -type d -directory $base *] {
            set name [file tail $dir]
            # Skip libs and hidden dirs
            if {$name ne "libs" && [string index $name 0] ne "."} {
                lappend systems $name
            }
        }
        
        return [lsort $systems]
    }
    
    proc log {level msg} {
        if {[catch {dservSet projects/log "\[$level\] $msg"}]} {
            puts "ess_projects $level: $msg"
        }
    }
    
    # JSON helpers
    proc list_to_json {lst} {
        if {[llength $lst] == 0} {
            return {[]}
        }
        set items {}
        foreach item $lst {
            lappend items "\"[string map {\" \\\" \\ \\\\} $item]\""
        }
        return "\[[join $items ,]\]"
    }
    
    proc json_to_list {json} {
        if {$json eq "" || $json eq {[]} || $json eq "null"} {
            return {}
        }
        # Simple JSON array parse - assumes array of strings
        set json [string trim $json {[]}]
        if {$json eq ""} {
            return {}
        }
        set result {}
        foreach item [split $json ,] {
            set item [string trim $item]
            set item [string trim $item \"]
            if {$item ne ""} {
                lappend result $item
            }
        }
        return $result
    }
    
    proc dict_to_json {d} {
        set pairs {}
        dict for {k v} $d {
            if {[string is integer -strict $v]} {
                lappend pairs "\"$k\":$v"
            } elseif {$v eq "null"} {
                lappend pairs "\"$k\":null"
            } elseif {[string index $v 0] eq "\[" || [string index $v 0] eq "\{"} {
                # Already JSON
                lappend pairs "\"$k\":$v"
            } else {
                lappend pairs "\"$k\":\"[string map {\" \\\" \\ \\\\ \n \\n \r \\r \t \\t} $v]\""
            }
        }
        return "\{[join $pairs ,]\}"
    }
    
    proc dict_to_json_deep {d} {
        package require yajltcl
        set obj [yajl create #auto]
        dict_to_yajl $obj $d
        set result [$obj get]
        $obj delete
        return $result
    }
    
    proc dict_to_yajl {obj d} {
        $obj map_open
        dict for {k v} $d {
            $obj string $k
            if {$v eq "" || $v eq "null"} {
                $obj null
            } elseif {[string is integer -strict $v]} {
                $obj number $v
            } elseif {[string is double -strict $v]} {
                $obj number $v
            } elseif {$v eq "true" || $v eq "false"} {
                $obj bool [expr {$v eq "true"}]
            } elseif {[llength $v] > 1 && [catch {dict size $v}] == 0} {
                # Nested dict
                dict_to_yajl $obj $v
            } elseif {[llength $v] > 1 || ([llength $v] == 0 && $v eq {})} {
                # List
                $obj array_open
                foreach item $v {
                    if {[catch {dict size $item}] == 0 && [llength $item] > 1} {
                        dict_to_yajl $obj $item
                    } elseif {[string is integer -strict $item]} {
                        $obj number $item
                    } else {
                        $obj string $item
                    }
                }
                $obj array_close
            } else {
                $obj string $v
            }
        }
        $obj map_close
    }
    
    proc dict_get_default {d key default} {
        if {[dict exists $d $key]} {
            return [dict get $d $key]
        }
        return $default
    }
    
    # Check if queue exists (bridge to ess_queues)
    proc queue_exists {name} {
        variable db
        set count [$db onecolumn {SELECT COUNT(*) FROM queues WHERE name = :name}]
        return [expr {$count > 0}]
    }
}

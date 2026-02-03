# -*- mode: tcl -*-
#
# ess_registry_configs-1.0.tm - Project/Config/Queue sync extensions for ESS Registry
#
# Extends ess_registry with push/pull operations for projects, configs, and queues.
# Intended to be sourced after ess_registry-1.0.tm or merged into it.
#
# Usage:
#   package require ess_registry
#   source ess_registry_configs-1.0.tm  ;# Or merge into ess_registry-1.0.tm
#   
#   # Push a project to the registry
#   ess::registry::push_project "my-project"
#   
#   # Pull a project from the registry
#   ess::registry::pull_project "my-project"
#   
#   # List projects on registry
#   ess::registry::list_projects
#

namespace eval ess::registry {

    #=========================================================================
    # HTTP Helpers (extensions)
    #=========================================================================
    
    proc api_put {path body args} {
        variable config
        
        if {$config(url) eq ""} {
            error "Registry URL not configured"
        }
        
        set url "$config(url)/api/v1/ess$path"
        set timeout $config(timeout)
        
        foreach {opt val} $args {
            if {$opt eq "-timeout"} {
                set timeout $val
            }
        }
        
        # Encode body as JSON if it's a dict
        if {[string index $body 0] ne "\{"} {
            set body [json_encode $body]
        }
        
        set response [https_put $url $body -timeout $timeout]
        return [json_decode $response]
    }
    
    proc api_delete {path args} {
        variable config
        
        if {$config(url) eq ""} {
            error "Registry URL not configured"
        }
        
        set url "$config(url)/api/v1/ess$path"
        set timeout $config(timeout)
        
        foreach {opt val} $args {
            if {$opt eq "-timeout"} {
                set timeout $val
            }
        }
        
        set response [https_delete $url -timeout $timeout]
        return [json_decode $response]
    }

    #=========================================================================
    # Project Definition Operations
    #=========================================================================
    
    # List all projects in workgroup on registry
    proc list_projects {} {
        variable config
        return [api_get "/projectdefs?workgroup=$config(workgroup)"]
    }
    
    # Get a single project from registry
    proc get_project {name} {
        variable config
        return [api_get "/projectdef/$config(workgroup)/$name"]
    }
    
    # Create a new project on registry
    proc create_project {name args} {
        variable config
        
        set description ""
        set systems {}
        
        foreach {opt val} $args {
            switch -exact -- $opt {
                -description { set description $val }
                -systems { set systems $val }
            }
        }
        
        set body [dict create \
            name $name \
            description $description \
            systems $systems]
        
        return [api_post "/projectdef/$config(workgroup)" [json_encode $body]]
    }
    
    # Delete a project on registry
    proc delete_project {name} {
        variable config
        return [api_delete "/projectdef/$config(workgroup)/$name"]
    }

    #=========================================================================
    # Config Operations (on registry)
    #=========================================================================
    
    # List configs for a project on registry
    proc list_configs {project args} {
        variable config
        
        set include_archived ""
        foreach {opt val} $args {
            if {$opt eq "-archived"} {
                set include_archived "?archived=true"
            }
        }
        
        return [api_get "/configs/$config(workgroup)/$project$include_archived"]
    }
    
    # Get a single config from registry
    proc get_config {project name} {
        variable config
        return [api_get "/config/$config(workgroup)/$project/$name"]
    }
    
    #=========================================================================
    # Queue Operations (on registry)
    #=========================================================================
    
    # List queues for a project on registry
    proc list_queues {project} {
        variable config
        return [api_get "/queues/$config(workgroup)/$project"]
    }
    
    # Get a single queue from registry (includes items)
    proc get_queue {project name} {
        variable config
        return [api_get "/queue/$config(workgroup)/$project/$name"]
    }

    #=========================================================================
    # Bundle Operations - Push/Pull Complete Projects
    #=========================================================================
    
    # Pull a complete project bundle from registry and import locally
    proc pull_project {project args} {
        variable config
        
        set overwrite 0
        foreach {opt val} $args {
            if {$opt eq "-overwrite"} {
                set overwrite $val
            }
        }
        
        # Get bundle from registry
        set params "?exportedBy=$config(user)"
        set bundle [api_get "/bundle/$config(workgroup)/$project$params"]
        
        if {![dict exists $bundle project]} {
            error "Invalid bundle received from registry"
        }
        
        # Import into local database
        set result [import_bundle_local $bundle $overwrite]
        
        # Update datapoint for UI
        catch { dservSet ess/registry/last_pull [clock seconds] }
        
        return $result
    }
    
    # Push a local project to the registry
    proc push_project {project args} {
        variable config
        
        set source_rig ""
        catch { set source_rig [dservGet system/hostname] }
        
        foreach {opt val} $args {
            switch -exact -- $opt {
                -rig { set source_rig $val }
            }
        }
        
        # Export local project as bundle
        set bundle [export_bundle_local $project $config(user) $source_rig]
        
        # Push to registry
        set result [api_post "/bundle/$config(workgroup)/$project" [json_encode_bundle $bundle]]
        
        # Update sync timestamp in local project
        catch {
            set db [::ess::configs::get_db]
            $db eval {
                UPDATE projects SET last_sync_at = strftime('%s','now'),
                       registry_url = :config(url)
                WHERE name = :project
            }
        }
        
        # Update datapoint for UI
        catch { dservSet ess/registry/last_push [clock seconds] }
        
        return $result
    }
    
    #=========================================================================
    # Local Bundle Export/Import
    #=========================================================================
    
    # Export a local project as a bundle dict
    proc export_bundle_local {project_name exported_by source_rig} {
        set db [::ess::configs::get_db]
        
        # Get project
        set project [::ess::configs::project_get $project_name]
        if {$project eq ""} {
            error "Local project not found: $project_name"
        }
        
        set project_id [dict get $project id]
        
        # Ensure systems is always a list
        set systems_val [dict get $project systems]
        if {$systems_val eq "" || $systems_val eq "null"} {
            set systems_val [list]
        } elseif {[llength $systems_val] == 1 && [lindex $systems_val 0] eq $systems_val} {
            set systems_val [list $systems_val]
        }
        
        # Build project dict for bundle
        set project_data [dict create \
            name [dict get $project name] \
            description [dict get $project description] \
            systems $systems_val]
        
        # Get configs
        set configs {}
        $db eval {
            SELECT id, name, description, script_source, system, protocol, variant,
                   subject, variant_args, params, file_template, tags, created_by
            FROM configs
            WHERE project_id = :project_id AND archived = 0
            ORDER BY name
        } row {
            # Ensure tags is always a list
            set tags_val [json_decode $row(tags)]
            if {$tags_val eq "" || $tags_val eq "null"} {
                set tags_val [list]
            } elseif {[llength $tags_val] == 1 && [lindex $tags_val 0] eq $tags_val} {
                # Single item that isn't already a list structure - wrap it
                set tags_val [list $tags_val]
            }
            
            lappend configs [dict create \
                id $row(id) \
                name $row(name) \
                description $row(description) \
                scriptSource $row(script_source) \
                system $row(system) \
                protocol $row(protocol) \
                variant $row(variant) \
                subject $row(subject) \
                variantArgs [json_decode $row(variant_args)] \
                params [json_decode $row(params)] \
                fileTemplate $row(file_template) \
                tags $tags_val \
                createdBy $row(created_by)]
        }
        
        # Get queues with items
        set queues {}
        $db eval {
            SELECT id, name, description, auto_start, auto_advance, auto_datafile, created_by
            FROM queues
            WHERE project_id = :project_id
            ORDER BY name
        } qrow {
            set queue_id $qrow(id)
            
            # Get items for this queue
            set items {}
            $db eval {
                SELECT config_id, position, repeat_count, pause_after, notes
                FROM queue_items
                WHERE queue_id = :queue_id
                ORDER BY position
            } irow {
                lappend items [dict create \
                    configId $irow(config_id) \
                    position $irow(position) \
                    repeatCount $irow(repeat_count) \
                    pauseAfter $irow(pause_after) \
                    notes $irow(notes)]
            }
            
            lappend queues [dict create \
                id $qrow(id) \
                name $qrow(name) \
                description $qrow(description) \
                autoStart [expr {$qrow(auto_start) ? "true" : "false"}] \
                autoAdvance [expr {$qrow(auto_advance) ? "true" : "false"}] \
                autoDatafile [expr {$qrow(auto_datafile) ? "true" : "false"}] \
                createdBy $qrow(created_by) \
                items $items]
        }
        
        # Build complete bundle
        return [dict create \
            project $project_data \
            configs $configs \
            queues $queues \
            exportedAt [clock format [clock seconds] -format "%Y-%m-%dT%H:%M:%SZ" -gmt 1] \
            exportedBy $exported_by \
            sourceRig $source_rig]
    }
    
    # Import a bundle into the local database
    proc import_bundle_local {bundle overwrite} {
        set db [::ess::configs::get_db]
        
        set project_data [dict get $bundle project]
        set project_name [dict get $project_data name]
        
        set result [dict create \
            success 1 \
            projectName $project_name \
            created {} \
            updated {} \
            skipped {} \
            errors {}]
        
        # Start transaction
        $db eval {BEGIN TRANSACTION}
        
        if {[catch {
            # Create or update project
            set existing [::ess::configs::project_get $project_name]
            set project_id ""
            
            if {$existing eq ""} {
                # Create new project
                set project_id [::ess::configs::project_create $project_name \
                    -description [dict_get_default $project_data description ""] \
                    -systems [dict_get_default $project_data systems {}]]
                dict lappend result created "project:$project_name"
            } else {
                set project_id [dict get $existing id]
                if {$overwrite} {
                    ::ess::configs::project_update $project_name \
                        -description [dict_get_default $project_data description ""]
                    dict lappend result updated "project:$project_name"
                } else {
                    dict lappend result skipped "project:$project_name"
                }
            }
            
            # Build map of old config IDs to new config IDs
            set config_id_map [dict create]
            
            # Import configs (handle null/empty and single-item)
            set configs_list [dict_get_default $bundle configs {}]
            if {$configs_list eq "null"} { set configs_list {} }
            # If it's a single dict (not a list of dicts), wrap it
            if {[llength $configs_list] > 0 && [dict exists $configs_list id]} {
                set configs_list [list $configs_list]
            }
            
            foreach cfg $configs_list {
                set old_id [dict get $cfg id]
                set cfg_name [dict get $cfg name]
                
                set existing_cfg [::ess::configs::get $cfg_name -project $project_name]
                
                if {$existing_cfg eq ""} {
                    # Create new config
                    set new_id [::ess::configs::create $project_name $cfg_name \
                        [dict get $cfg system] \
                        [dict get $cfg protocol] \
                        [dict get $cfg variant] \
                        -description [dict_get_default $cfg description ""] \
                        -script_source [dict_get_default $cfg scriptSource ""] \
                        -subject [dict_get_default $cfg subject ""] \
                        -variant_args [dict_get_default $cfg variantArgs {}] \
                        -params [dict_get_default $cfg params {}] \
                        -file_template [dict_get_default $cfg fileTemplate ""] \
                        -tags [dict_get_default $cfg tags {}]]
                    dict set config_id_map $old_id $new_id
                    dict lappend result created "config:$cfg_name"
                } elseif {$overwrite} {
                    # Update existing
                    set new_id [dict get $existing_cfg id]
                    ::ess::configs::update $cfg_name \
                        -project $project_name \
                        -description [dict_get_default $cfg description ""] \
                        -script_source [dict_get_default $cfg scriptSource ""] \
                        -subject [dict_get_default $cfg subject ""] \
                        -variant_args [dict_get_default $cfg variantArgs {}] \
                        -params [dict_get_default $cfg params {}] \
                        -file_template [dict_get_default $cfg fileTemplate ""] \
                        -tags [dict_get_default $cfg tags {}]
                    dict set config_id_map $old_id $new_id
                    dict lappend result updated "config:$cfg_name"
                } else {
                    set new_id [dict get $existing_cfg id]
                    dict set config_id_map $old_id $new_id
                    dict lappend result skipped "config:$cfg_name"
                }
            }
            
            # Import queues (handle null/empty and single-item)
            set queues_list [dict_get_default $bundle queues {}]
            if {$queues_list eq "null"} { set queues_list {} }
            # If it's a single dict (not a list of dicts), wrap it
            if {[llength $queues_list] > 0 && [dict exists $queues_list id]} {
                set queues_list [list $queues_list]
            }
            
            foreach q $queues_list {
                set q_name [dict get $q name]
                
                # Check if queue exists
                set existing_q ""
                catch { set existing_q [::ess_queues::queue_get $q_name -project $project_name] }
                
                if {$existing_q eq ""} {
                    # Create new queue
                    ::ess_queues::queue_create $q_name \
                        -project $project_name \
                        -description [dict_get_default $q description ""] \
                        -auto_start [expr {[dict_get_default $q autoStart "true"] eq "true"}] \
                        -auto_advance [expr {[dict_get_default $q autoAdvance "true"] eq "true"}] \
                        -auto_datafile [expr {[dict_get_default $q autoDatafile "true"] eq "true"}]
                    dict lappend result created "queue:$q_name"
                } elseif {$overwrite} {
                    # Delete existing items and recreate
                    ::ess_queues::queue_clear_items $q_name -project $project_name
                    dict lappend result updated "queue:$q_name"
                } else {
                    dict lappend result skipped "queue:$q_name"
                    continue
                }
                
                # Add items with mapped config IDs
                set items_list [dict_get_default $q items {}]
                if {$items_list eq "null"} { set items_list {} }
                # If it's a single dict (not a list of dicts), wrap it
                if {[llength $items_list] > 0 && [dict exists $items_list id]} {
                    set items_list [list $items_list]
                }
                
                foreach item $items_list {
                    set old_config_id [dict get $item configId]
                    
                    if {![dict exists $config_id_map $old_config_id]} {
                        dict lappend result errors "queue:$q_name item: config ID $old_config_id not found"
                        continue
                    }
                    
                    set new_config_id [dict get $config_id_map $old_config_id]
                    
                    # Get config name for the queue item add
                    set cfg_name [$db onecolumn {SELECT name FROM configs WHERE id = :new_config_id}]
                    
                    ::ess_queues::queue_add_item $q_name $cfg_name \
                        -project $project_name \
                        -repeat [dict_get_default $item repeatCount 1] \
                        -pause_after [dict_get_default $item pauseAfter 0] \
                        -notes [dict_get_default $item notes ""]
                }
            }
            
            # Update project sync timestamp
            $db eval {
                UPDATE projects SET last_sync_at = strftime('%s','now')
                WHERE id = :project_id
            }
            
            $db eval {COMMIT}
            
        } err]} {
            $db eval {ROLLBACK}
            dict set result success 0
            dict lappend result errors $err
        }
        
        # Update counts (handle null)
        set cfg_list [dict_get_default $bundle configs {}]
        if {$cfg_list eq "null"} { set cfg_list {} }
        set q_list [dict_get_default $bundle queues {}]
        if {$q_list eq "null"} { set q_list {} }
        
        dict set result configsCount [llength $cfg_list]
        dict set result queuesCount [llength $q_list]
        
        # Publish updates for UI
        catch { ::ess::configs::publish_all }
        
        return $result
    }
    
    #=========================================================================
    # JSON Encoding for Bundles (using yajltcl primitives)
    #=========================================================================
    
    proc json_encode_bundle {bundle} {
        package require yajltcl
        
        set obj [yajl create #auto]
        
        $obj map_open
        
        # Project
        $obj string "project"
        encode_project $obj [dict get $bundle project]
        
        # Configs array
        $obj string "configs"
        $obj array_open
        foreach cfg [dict get $bundle configs] {
            encode_config $obj $cfg
        }
        $obj array_close
        
        # Queues array
        $obj string "queues"
        $obj array_open
        foreach q [dict get $bundle queues] {
            encode_queue $obj $q
        }
        $obj array_close
        
        # Metadata
        $obj string "exportedAt" string [dict get $bundle exportedAt]
        $obj string "exportedBy" string [dict get $bundle exportedBy]
        $obj string "sourceRig" string [dict get $bundle sourceRig]
        
        $obj map_close
        
        set result [$obj get]
        $obj delete
        return $result
    }
    
    proc encode_project {obj p} {
        $obj map_open
        $obj string "name" string [dict_get_default $p name ""]
        $obj string "description" string [dict_get_default $p description ""]
        
        # Systems is always an array of strings
        $obj string "systems"
        $obj array_open
        foreach s [dict_get_default $p systems {}] {
            $obj string $s
        }
        $obj array_close
        
        $obj map_close
    }
    
    proc encode_config {obj cfg} {
        $obj map_open
        
        $obj string "id" number [dict get $cfg id]
        $obj string "name" string [dict get $cfg name]
        $obj string "description" string [dict_get_default $cfg description ""]
        $obj string "scriptSource" string [dict_get_default $cfg scriptSource ""]
        $obj string "system" string [dict get $cfg system]
        $obj string "protocol" string [dict get $cfg protocol]
        $obj string "variant" string [dict get $cfg variant]
        $obj string "subject" string [dict_get_default $cfg subject ""]
        
        # variantArgs is an object
        $obj string "variantArgs"
        encode_string_map $obj [dict_get_default $cfg variantArgs {}]
        
        # params is an object
        $obj string "params"
        encode_string_map $obj [dict_get_default $cfg params {}]
        
        $obj string "fileTemplate" string [dict_get_default $cfg fileTemplate ""]
        
        # tags is always an array of strings
        $obj string "tags"
        $obj array_open
        foreach t [dict_get_default $cfg tags {}] {
            $obj string $t
        }
        $obj array_close
        
        $obj string "createdBy" string [dict_get_default $cfg createdBy ""]
        
        $obj map_close
    }
    
    proc encode_queue {obj q} {
        $obj map_open
        
        $obj string "id" number [dict get $q id]
        $obj string "name" string [dict get $q name]
        $obj string "description" string [dict_get_default $q description ""]
        $obj string "autoStart" bool [expr {[dict_get_default $q autoStart "true"] eq "true"}]
        $obj string "autoAdvance" bool [expr {[dict_get_default $q autoAdvance "true"] eq "true"}]
        $obj string "autoDatafile" bool [expr {[dict_get_default $q autoDatafile "true"] eq "true"}]
        $obj string "createdBy" string [dict_get_default $q createdBy ""]
        
        # items is always an array
        $obj string "items"
        $obj array_open
        foreach item [dict_get_default $q items {}] {
            encode_queue_item $obj $item
        }
        $obj array_close
        
        $obj map_close
    }
    
    proc encode_queue_item {obj item} {
        $obj map_open
        $obj string "configId" number [dict get $item configId]
        $obj string "position" number [dict get $item position]
        $obj string "repeatCount" number [dict_get_default $item repeatCount 1]
        $obj string "pauseAfter" number [dict_get_default $item pauseAfter 0]
        $obj string "notes" string [dict_get_default $item notes ""]
        $obj map_close
    }
    
    # Encode a dict as a JSON object with auto-typed values
    proc encode_string_map {obj d} {
        $obj map_open
        dict for {k v} $d {
            $obj string $k
            # Auto-detect type
            if {[string is integer -strict $v]} {
                $obj number $v
            } elseif {[string is double -strict $v]} {
                $obj number $v
            } elseif {$v eq "true"} {
                $obj bool 1
            } elseif {$v eq "false"} {
                $obj bool 0
            } else {
                $obj string $v
            }
        }
        $obj map_close
    }
    
    #=========================================================================
    # Helper: dict_get with default value
    #=========================================================================
    
    proc dict_get_default {d key default} {
        if {[dict exists $d $key]} {
            return [dict get $d $key]
        }
        return $default
    }

    #=========================================================================
    # Sync Status
    #=========================================================================
    
    # Check if local project is in sync with registry
    proc project_sync_status {project_name} {
        variable config
        
        # Get local project
        set local [::ess::configs::project_get $project_name]
        if {$local eq ""} {
            return [dict create status "local_only" message "Project not found locally"]
        }
        
        # Get remote project
        if {[catch {
            set remote [api_get "/projectdef/$config(workgroup)/$project_name"]
        } err]} {
            if {[string match "*404*" $err] || [string match "*not found*" $err]} {
                return [dict create status "local_only" message "Project not on registry"]
            }
            return [dict create status "error" message $err]
        }
        
        # Compare timestamps
        set local_updated [dict_get_default $local updated_at 0]
        set local_synced [dict_get_default $local last_sync_at 0]
        
        if {$local_synced == 0} {
            return [dict create status "never_synced" message "Project has never been synced"]
        }
        
        if {$local_updated > $local_synced} {
            return [dict create status "local_modified" \
                message "Local changes since last sync" \
                localUpdated $local_updated \
                lastSync $local_synced]
        }
        
        return [dict create status "synced" message "Project is in sync"]
    }
}

#
# configsconf.tcl - Configs Manager subprocess configuration
#
# Unified management for projects, configs, and queues.
#
# Architecture:
#   - Projects own configs and queues (single ownership)
#   - Configs are complete runnable units (include file_template)
#   - Queues are ordered sequences of config references
#
# Commands via "send configs ...":
#
#   Projects:
#     project_create, project_get, project_list, project_update, project_delete
#     project_activate, project_deactivate, project_active
#     project_export, project_import
#
#   Configs:
#     config_save, config_load, config_run, config_list, config_get
#     config_update, config_archive, config_delete, config_clone
#
#   Queues:
#     queue_create, queue_get, queue_list, queue_update, queue_delete
#     queue_add, queue_remove, queue_clear
#     queue_start, queue_stop, queue_pause, queue_resume
#     queue_skip, queue_next
#
#   Registry Sync:
#     registry_configure, registry_push, registry_pull
#     registry_list_projects, registry_sync_status
#

package require dlsh
package require qpcs
package require tcljson

tcl::tm::add $dspath/lib

errormon enable
proc exit {args} { error "exit not available for this subprocess" }

package require ess_configs
package require ess_queues
package require ess_registry
package require ess_registry_configs

load ${dspath}/modules/dserv_timer[info sharedlibextension]

#=========================================================================
# Initialize
#=========================================================================

set configs_db [file join $dspath db configs.db]
file mkdir [file dirname $configs_db]

ess::configs::init $configs_db
::ess_queues::init [ess::configs::get_db]

# Initialize registry from saved datapoints
ess::registry::init_from_dserv

ess::configs::publish_all

#=========================================================================
# Single config queue
#=========================================================================

proc queue_run_config {config_name args} {
    # Start the orchestration timer
    queue_timer_start 500    
    ::ess_queues::run_single $config_name {*}$args
}

#=========================================================================
# Timer-based Queue Orchestration
#=========================================================================

variable queue_timer_running 0

proc queue_tick_callback {dpoint data} {
    ::ess_queues::tick
}

proc queue_timer_setup {} {
    timerPrefix queueTimer
    dservAddExactMatch queueTimer/0
    dpointSetScript queueTimer/0 queue_tick_callback
}

proc queue_timer_start {{interval_ms 500}} {
    variable queue_timer_running
    if {$queue_timer_running} { return }
    timerTickInterval $interval_ms $interval_ms
    set queue_timer_running 1
}

proc queue_timer_stop {} {
    variable queue_timer_running
    if {!$queue_timer_running} { return }
    timerStop
    set queue_timer_running 0
}

queue_timer_setup

#=========================================================================
# ESS State Monitoring
#=========================================================================

proc on_ess_run_state_change {dpoint data} {
    ::ess_queues::on_ess_run_state $data
}

dservAddExactMatch ess/run_state
dpointSetScript ess/run_state on_ess_run_state_change

proc on_ess_snapshot_change {dpoint_name value} {
    if {$value ne "" && [json_get $value source] eq "config"} {
        return
    }
    dservSet configs/current {}
}

dservAddExactMatch ess/snapshot
dpointSetScript ess/snapshot on_ess_snapshot_change

#=========================================================================
# Project Commands
#=========================================================================

proc project_create {name args} { ess::configs::project_create $name {*}$args }
proc project_get {name} { ess::configs::project_get $name }
proc project_list {} { ess::configs::project_list }
proc project_update {name args} { ess::configs::project_update $name {*}$args }
proc project_delete {name} { ess::configs::project_delete $name }
proc project_exists {name} { ess::configs::project_exists $name }

proc project_activate {name} { ess::configs::project_activate $name }
proc project_deactivate {} { ess::configs::project_deactivate }
proc project_active {} { ess::configs::project_active }

#=========================================================================
# Config Commands
#=========================================================================

proc config_publish_all {} { ess::configs::publish_all }

proc config_create {name system protocol variant args} {
    set project $::ess::configs::active_project
    ess::configs::create $project $name $system $protocol $variant {*}$args
}

# Save current ESS state
# Usage: config_save "name" ?-description text? ?-tags {t1 t2}? ?-file_template "{subject}_{date}"?
proc config_save {name args} {
    ess::configs::save_current $name {*}$args
}

# Load config into ESS (setup only)
# Usage: config_load "name" ?-project "proj"?
proc config_load {name_or_id args} {
    ess::configs::load $name_or_id {*}$args
}

# Run config (load + open file + start)
# Usage: config_run "name" ?-project "proj"? ?-auto_start 1?
proc config_run {name_or_id args} {
    ess::configs::run $name_or_id {*}$args
}

# List configs (in active project by default)
# Usage: config_list ?-project "proj"? ?-tags {t1}? ?-system "sys"? ?-all 1?
proc config_list {args} {
    ess::configs::list {*}$args
}

# Get full config details
# Usage: config_get "name" ?-project "proj"?
proc config_get {name_or_id args} {
    ess::configs::get $name_or_id {*}$args
}

proc config_get_json {name args} {
    set config [ess::configs::get $name {*}$args]
    if {$config eq ""} {
        return "{}"
    }
    return [dict_to_json $config]
}

# Check if config exists
proc config_exists {name_or_id args} {
    ess::configs::exists $name_or_id {*}$args
}

# Clone config
# Usage: config_clone "source" "newname" ?-to_project "proj"? ?-subject "new_subj"?
proc config_clone {source new_name args} {
    ess::configs::clone $source $new_name {*}$args
}

# Update config
# Usage: config_update "name" -description "text" -file_template "..."
proc config_update {name_or_id args} {
    ess::configs::update $name_or_id {*}$args
}

# Archive (soft delete) - fails if config is in any queue
proc config_archive {name_or_id args} {
    ess::configs::archive $name_or_id {*}$args
}

# Hard delete - fails if config is in any queue
proc config_delete {name_or_id args} {
    ess::configs::delete $name_or_id {*}$args
}


proc config_get_variant_options {system protocol variant} {
    set system_path [dservGet ess/system_path]
    if {$system_path eq ""} {
        return "{\"error\":\"System path not available\"}"
    }
    
    set project [dservGet ess/project]
    if {$project eq ""} {
        set project "ess"
    }
    
    set variants_file [file join $system_path $project $system $protocol ${protocol}_variants.tcl]
    
    if {![file exists $variants_file]} {
        return "{\"error\":\"Variants file not found: $variants_file\"}"
    }
    
    set fd [::open $variants_file r]
    set contents [::read $fd]
    ::close $fd
    
    set safe [interp create]
    
    $safe eval {
	lappend auto_path [zipfs root]/dlsh/lib
	package require dlsh
        proc namespace {cmd args} {
            if {$cmd eq "eval"} {
                uplevel #0 [lindex $args end]
            }
        }
        
        proc package {args} {}
        
        proc variable {name args} {
            if {[llength $args] == 1} {
                uplevel 1 [list set $name [lindex $args 0]]
            }
        }
    }
    
    if {[catch {$safe eval $contents} err]} {
        interp delete $safe
        return "{\"error\":\"Could not parse file: $err\"}"
    }
    
    if {[catch {set variants_body [$safe eval {set variants}]} err]} {
        interp delete $safe
        return "{\"error\":\"Could not find variants definition\"}"
    }
    
    interp delete $safe
    
    if {$variants_body eq ""} {
        return "{\"error\":\"Could not find variants definition\"}"
    }
    
    if {[catch {set variants_dict [dict create {*}$variants_body]} err]} {
        return "{\"error\":\"Could not parse variants: $err\"}"
    }
    
    if {![dict exists $variants_dict $variant]} {
        return "{\"error\":\"Variant '$variant' not found\"}"
    }
    
    set variant_def [dict get $variants_dict $variant]
    
    set loader_options {}
    if {[dict exists $variant_def loader_options]} {
        set loader_options [dict get $variant_def loader_options]
    }
    
    package require yajltcl
    set obj [yajl create #auto]
    
    $obj map_open
    $obj string "loader_options" map_open
    
    dict for {arg_name options_list} $loader_options {
        $obj string $arg_name array_open
        
        foreach opt $options_list {
            $obj map_open
            
            if {[llength $opt] == 2} {
                set lbl [lindex $opt 0]
                set val [lindex $opt 1]
                if {[llength $val] > 1} {
                    set val [list {*}$val]
                }
            } else {
                set lbl $opt
                set val $opt
            }
            
            $obj string "label" string $lbl
            $obj string "value" string $val
            
            $obj map_close
        }
        
        $obj array_close
    }
    
    $obj map_close
    $obj map_close
    
    set result [$obj get]
    $obj delete
    return $result
}

#=========================================================================
# Queue Commands
#=========================================================================

# Create queue in active project
# Usage: queue_create "name" ?-project "proj"? ?-description text? ?-auto_start 1?
proc queue_create {name args} {
    ::ess_queues::queue_create $name {*}$args
}

# Get queue details
proc queue_get {name args} {
    ::ess_queues::queue_get $name {*}$args
}


# JSON wrapper for queue_get (for JS clients)  
proc queue_get_json {name args} {
    ::ess_queues::queue_get_json $name {*}$args
}

proc queue_publish_list {} { ::ess_queues::publish_list }

# List queues (in active project by default)
proc queue_list {args} {
    ::ess_queues::queue_list {*}$args
}

# Update queue settings
proc queue_update {name args} {
    ::ess_queues::queue_update $name {*}$args
}

# Delete queue
proc queue_delete {name args} {
    ::ess_queues::queue_delete $name {*}$args
}

#=========================================================================
# Queue Item Commands
#=========================================================================

# Add config to queue (by name, looked up in same project)
# Usage: queue_add "queue" "config" ?-repeat N? ?-pause_after N? ?-notes text?
proc queue_add_item {queue_name config_name args} {
    ::ess_queues::queue_add_item $queue_name $config_name {*}$args
}

# Remove item from queue by position
proc queue_remove_item {queue_name position args} {
    ::ess_queues::queue_remove_item $queue_name $position {*}$args
}

# Update item from queue by position
proc queue_update_item {queue_name position args} {
    ::ess_queues::queue_update_item $queue_name $position {*}$args
}

proc queue_clear_items {queue_name args} {
    ::ess_queues::queue_clear_items $queue_name {*}$args
}

#=========================================================================
# Queue Run Control
#=========================================================================

proc run_close {} {
    ::ess_queues::run_close
}

# Start running a queue
proc queue_start {name args} {
    queue_timer_start 500
    ::ess_queues::queue_start $name {*}$args
}

# Stop queue entirely
proc queue_stop {} {
    ::ess_queues::queue_stop
    queue_timer_stop
}

# Pause queue
proc queue_pause {} {
    ::ess_queues::queue_pause
}

# Resume paused queue
proc queue_resume {} {
    ::ess_queues::queue_resume
}

# Skip current item (ignore remaining repeats)
proc queue_skip {} {
    ::ess_queues::queue_skip
}

# Advance to next run (respects repeats)
proc queue_next {} {
    ::ess_queues::queue_next
}

# Reset queue to beginning (position 0, idle state)
proc queue_reset {} {
    ::ess_queues::queue_reset
}

# Get current queue state
proc queue_status {} {
    return [list \
        status [dservGet queues/status] \
        active [dservGet queues/active] \
        project [dservGet queues/project] \
        position [dservGet queues/position] \
        total [dservGet queues/total] \
        current_config [dservGet queues/current_config] \
        run_count [dservGet queues/run_count] \
        repeat_count [dservGet queues/repeat_count]]
}

#=========================================================================
# Registry Sync Commands
#=========================================================================

# Configure registry connection
# Usage: registry_configure -url "http://server:8080" -workgroup "mylab" ?-user "dave"?
proc registry_configure {args} {
    ess::registry::configure {*}$args
    
    # Publish config for UI
    dservSet registry/config [dict create \
        url [ess::registry::cget -url] \
        workgroup [ess::registry::cget -workgroup] \
        user [ess::registry::cget -user]]
}

# Push local project to registry
# Usage: registry_push "project_name"
proc registry_push {project_name} {
    set result [ess::registry::push_project $project_name]
    
    # Publish result for UI
    dservSet registry/last_operation [dict create \
        action "push" \
        project $project_name \
        success [dict get $result success] \
        timestamp [clock seconds]]
    
    return $result
}

# Pull project from registry to local
# Usage: registry_pull "project_name" ?-overwrite 1?
proc registry_pull {project_name args} {
    set overwrite 0
    foreach {opt val} $args {
        if {$opt eq "-overwrite"} {
            set overwrite $val
        }
    }
    
    set result [ess::registry::pull_project $project_name -overwrite $overwrite]
    
    # Publish result for UI
    dservSet registry/last_operation [dict create \
        action "pull" \
        project $project_name \
        success [dict get $result success] \
        timestamp [clock seconds]]
    
    # Refresh UI lists
    ess::configs::publish_all
    
    return $result
}

# List projects available on registry
# Usage: registry_list_projects
proc registry_list_projects {} {
    ess::registry::list_projects
}

# List projects available on registry as JSON
# Usage: registry_list_projects_json
proc registry_list_projects_json {} {
    ess::registry::list_projects_json
}

# Get project from registry (without importing)
# Usage: registry_get_project "project_name"
proc registry_get_project {name} {
    ess::registry::get_project $name
}

# Check sync status of local project
# Usage: registry_sync_status "project_name"
proc registry_sync_status {project_name} {
    ess::registry::project_sync_status $project_name
}

# Get current registry configuration
proc registry_config {} {
    dict create \
        url [ess::registry::cget -url] \
        workgroup [ess::registry::cget -workgroup] \
        user [ess::registry::cget -user]
}

#=========================================================================
# Startup Complete
#=========================================================================

puts "Configs Manager subprocess ready"
puts "  Database: $configs_db"
puts "  Project: project_create, project_activate, project_list"
puts "  Config:  config_save, config_load, config_run, config_list"
puts "  Queue:   queue_create, queue_add, queue_start, queue_stop"
puts "  Registry: registry_configure, registry_push, registry_pull"

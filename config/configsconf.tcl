#
# configsconf.tcl - Configs Manager subprocess configuration
#
# This subprocess provides configuration persistence and management for ESS,
# plus queue orchestration for running sequences of configs.
#
# It owns the configs database and handles all CRUD operations for both
# configs and queues.
#
# Config operations (via send configs "..."):
#   config_save name ?-tags {t1 t2}? ?-description text?
#   config_load name
#   config_list ?-tags {t1 t2}?
#   config_get name
#   etc.
#
# Queue operations (via send configs "..."):
#   queue_create name ?-description text?
#   queue_add queue_name config_name ?-position N? ?-repeat N?
#   queue_start name
#   queue_stop / queue_pause / queue_resume
#   queue_next / queue_skip / queue_retry
#   etc.
#

package require dlsh
package require qpcs
package require tcljson

# Add local lib to module path
tcl::tm::add $dspath/lib

# Standard subprocess setup
errormon enable
proc exit {args} { error "exit not available for this subprocess" }

# Load the configs and queues modules
package require ess_configs
package require ess_queues

# Load timer module for queue orchestration
load ${dspath}/modules/dserv_timer[info sharedlibextension]

#=========================================================================
# Initialize
#=========================================================================

# Database location
set configs_db [file join $dspath db configs.db]

# Ensure db directory exists
file mkdir [file dirname $configs_db]

# Initialize the configs system
ess::configs::init $configs_db

# Initialize the queues system (shares same database)
::ess_queues::init [ess::configs::get_db]

# Purge archived configs older than 60 days
ess::configs::purge_old_archived 60

# Publish initial state for any connected clients
ess::configs::publish_all
::ess_queues::publish_list

#=========================================================================
# Timer-based Queue Orchestration
#=========================================================================

# Track whether timer is running
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
    
    if {$queue_timer_running} {
        return
    }
    
    timerTickInterval $interval_ms $interval_ms
    set queue_timer_running 1
    puts "Queue orchestration timer started: ${interval_ms}ms interval"
}

proc queue_timer_stop {} {
    variable queue_timer_running
    
    if {!$queue_timer_running} {
        return
    }
    
    timerStop
    set queue_timer_running 0
    puts "Queue orchestration timer stopped"
}

# Initialize timer (but don't start until a queue is started)
queue_timer_setup

#=========================================================================
# ESS Run State Monitoring for Queue Orchestration
#=========================================================================

proc on_ess_run_state_change {dpoint data} {
    ::ess_queues::on_ess_run_state $data
}

# Subscribe to ESS run_state changes
# run_state is "active" when running, "complete" when finished normally
# This is more reliable than ess/status which changes during config loading
dservAddExactMatch ess/run_state
dpointSetScript ess/run_state on_ess_run_state_change

#=========================================================================
# Track ESS Setup Changes
#=========================================================================

# When ESS snapshot changes, check if it came from a config load.
# If not, clear configs/current since saved config no longer matches.
proc on_ess_snapshot_change {dpoint_name value} {
    if {$value ne "" && [json_get $value source] eq "config"} {
        return
    }
    # User change - clear current config
    dservSet configs/current {}
}

# Subscribe to ESS snapshot - fires after any setup change completes
dservAddExactMatch ess/snapshot
dpointSetScript ess/snapshot on_ess_snapshot_change

#=========================================================================
# Config Commands - these are what other threads call via send
#=========================================================================

# Save current ESS state as named config
# Usage: config_save "name" ?-tags {t1 t2}? ?-description "text"?
proc config_save {name args} {
    ess::configs::save_current $name {*}$args
}

# Load a config into ESS
# Usage: config_load "name" or config_load 42
proc config_load {name_or_id} {
    ess::configs::load $name_or_id
}

# List configs with optional filtering
# Usage: config_list ?-tags {t1 t2}? ?-system "fixcal"? ?-search "text"?
proc config_list {args} {
    ess::configs::list {*}$args
}

# Get full config details
# Usage: config_get "name" or config_get 42
proc config_get {name_or_id} {
    ess::configs::get $name_or_id
}

# Get full config details as JSON (for web frontend)
# Usage: config_get_json "name"
proc config_get_json {name_or_id} {
    set config [ess::configs::get $name_or_id]
    if {$config eq ""} {
        return "{}"
    }
    return [dict_to_json $config]
}

# Check if config exists
# Usage: config_exists "name"
proc config_exists {name_or_id} {
    ess::configs::exists $name_or_id
}

# Clone a config
# Usage: config_clone "source" "newname" ?-variant_args {k v}? ?-params {k v}?
proc config_clone {source new_name args} {
    ess::configs::clone $source $new_name {*}$args
}

# Update config metadata/values
# Usage: config_update "name" -description "new desc" -tags {t1 t2}
proc config_update {name_or_id args} {
    ess::configs::update $name_or_id {*}$args
}

# Archive (soft delete)
# Usage: config_archive "name"
proc config_archive {name_or_id} {
    ess::configs::archive $name_or_id
}

# Hard delete
# Usage: config_delete "name"
proc config_delete {name_or_id} {
    ess::configs::delete $name_or_id
}

# Restore archived config
# Usage: config_restore "name"
proc config_restore {name_or_id} {
    ess::configs::restore $name_or_id
}

# Get quick picks (most used)
# Usage: config_quick_picks ?5?
proc config_quick_picks {{limit 5}} {
    ess::configs::quick_picks $limit
}

# Get all tags in use
# Usage: config_tags
proc config_tags {} {
    ess::configs::get_all_tags
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
    
    set safe [interp create -safe]
    
    $safe eval {
	package reuqire dlsh
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
# Export/Import for Cross-Rig Sync
#=========================================================================

# Export config as JSON
# Usage: config_export "name"
proc config_export {name_or_id} {
    ess::configs::export $name_or_id
}

# Import config from JSON
# Usage: config_import {json_string} ?-remap_script_source "new_source"?
proc config_import {json args} {
    ess::configs::import $json {*}$args
}

# Push config to remote rig
# Usage: config_push "name" remote_host
proc config_push {name_or_id remote_host} {
    set json [ess::configs::export $name_or_id]
    send $remote_host "config_import {$json}"
}

# Bulk export all configs matching filter
# Usage: config_export_all ?-tags {t1 t2}?
proc config_export_all {args} {
    set configs [ess::configs::list {*}$args]
    set exports {}
    foreach cfg $configs {
        set name [dict get $cfg name]
        lappend exports [ess::configs::export $name]
    }
    return $exports
}

# Bulk push to remote rig
# Usage: config_push_all remote_host ?-tags {t1 t2}?
proc config_push_all {remote_host args} {
    set exports [config_export_all {*}$args]
    set count 0
    foreach json $exports {
        if {[catch {send $remote_host "config_import {$json}"} err]} {
            puts "Warning: failed to push config: $err"
        } else {
            incr count
        }
    }
    return $count
}

#=========================================================================
# Refresh Publishing
#=========================================================================

# Manually refresh published datapoints
proc config_publish_all {} {
    ess::configs::publish_all
}

proc config_publish_list {} {
    ess::configs::publish_list
}

proc config_publish_tags {} {
    ess::configs::publish_tags
}

#=========================================================================
# Direct Create (for programmatic use)
#=========================================================================

# Create config from explicit values (not current ESS state)
# Usage: config_create "name" system protocol variant ?-subject s? ?-variant_args {}? ?-params {}? ?-tags {}?
proc config_create {name system protocol variant args} {
    ess::configs::create $name $system $protocol $variant {*}$args
}

#=========================================================================
# Queue CRUD Commands
#=========================================================================

# Create a new queue
# Usage: queue_create "name" ?-description "text"? ?-auto_start 1? ?-auto_advance 1? 
#                           ?-auto_datafile 1? ?-datafile_template "{suggest}"?
#
# Datafile template substitutions (when not using {suggest}):
#   {subject}    - Current ESS subject
#   {system}     - ESS system name
#   {protocol}   - ESS protocol name  
#   {variant}    - ESS variant name
#   {config}     - Config name (or short_name if set)
#   {queue}      - Queue name
#   {position}   - Position in queue (0-based)
#   {run}        - Run number (1-based, for repeats)
#   {date}       - YYYYMMDD
#   {date_short} - YYMMDD
#   {time}       - HHMMSS
#   {time_short} - HHMM
#   {timestamp}  - Unix timestamp
#
# Example templates:
#   "{suggest}"                           - Use ESS's default naming (default)
#   "{subject}_{config}_{date}{time}_r{run}" - Custom with all fields
#   "{subject}_{date_short}"              - Minimal naming
#
proc queue_create {name args} {
    ::ess_queues::queue_create $name {*}$args
}

# Delete a queue
# Usage: queue_delete "name"
proc queue_delete {name} {
    ::ess_queues::queue_delete $name
}

# List all queues
# Usage: queue_list
proc queue_list {} {
    ::ess_queues::queue_list
}

# Get queue details with items
# Usage: queue_get "name"
proc queue_get {name} {
    ::ess_queues::queue_get $name
}

# Get queue details as JSON (for web frontend)
# Usage: queue_get_json "name"
# Uses yajltcl to ensure items is properly encoded as a JSON array
proc queue_get_json {name} {
    set queue [::ess_queues::queue_get $name]
    if {$queue eq ""} {
        return "{}"
    }
    
    # Build JSON with yajltcl to ensure items is an array
    package require yajltcl
    set obj [yajl create #auto]
    $obj map_open
    
    dict for {k v} $queue {
        if {$k eq "items"} {
            # Items is a list of dicts - encode as JSON array
            $obj string "items" array_open
            foreach item $v {
                $obj map_open
                dict for {ik iv} $item {
                    if {[string is integer -strict $iv]} {
                        $obj string $ik number $iv
                    } else {
                        $obj string $ik string $iv
                    }
                }
                $obj map_close
            }
            $obj array_close
        } elseif {[string is integer -strict $v]} {
            $obj string $k number $v
        } else {
            $obj string $k string $v
        }
    }
    
    $obj map_close
    set result [$obj get]
    $obj delete
    return $result
}

# Update queue settings
# Usage: queue_update "name" ?-description "text"? ?-auto_start 0? ?-auto_advance 0? 
#                           ?-auto_datafile 0? ?-datafile_template "..."? ?-name "newname"?
proc queue_update {name args} {
    ::ess_queues::queue_update $name {*}$args
}

#=========================================================================
# Queue Item Commands
#=========================================================================

# Add config to queue
# Usage: queue_add "queue_name" "config_name" ?-position N? ?-repeat N? ?-pause_after N? ?-notes "text"?
proc queue_add {queue_name config_name args} {
    ::ess_queues::queue_add $queue_name $config_name {*}$args
}

# Remove item from queue
# Usage: queue_remove "queue_name" position
proc queue_remove {queue_name position} {
    ::ess_queues::queue_remove $queue_name $position
}

# Reorder item in queue
# Usage: queue_reorder "queue_name" from_pos to_pos
proc queue_reorder {queue_name from_pos to_pos} {
    ::ess_queues::queue_reorder $queue_name $from_pos $to_pos
}

# Clear all items from queue
# Usage: queue_clear "queue_name"
proc queue_clear {queue_name} {
    ::ess_queues::queue_clear $queue_name
}

# Update individual queue item
# Usage: queue_item_update "queue_name" position ?-config_name "name"? ?-repeat N? ?-pause_after N? ?-notes "text"?
proc queue_item_update {queue_name position args} {
    ::ess_queues::queue_item_update $queue_name $position {*}$args
}

#=========================================================================
# Queue Run Control Commands
#=========================================================================

# Start running a queue
# Usage: queue_start "name" ?-position N?
proc queue_start {name args} {
    # Start the orchestration timer
    queue_timer_start 500
    ::ess_queues::queue_start $name {*}$args
}

# Stop queue (abort entirely)
# Usage: queue_stop
proc queue_stop {} {
    ::ess_queues::queue_stop
    queue_timer_stop
}

# Pause queue (stop current run, don't advance)
# Usage: queue_pause
proc queue_pause {} {
    ::ess_queues::queue_pause
}

# Resume paused queue
# Usage: queue_resume
proc queue_resume {} {
    ::ess_queues::queue_resume
}

# Manually start run when in ready state (for manual mode)
# Usage: queue_run
proc queue_run {} {
    ::ess_queues::queue_run
}

# Advance to next item (manual advance)
# Usage: queue_next
proc queue_next {} {
    ::ess_queues::queue_next
}

# Skip current item entirely (ignore remaining repeats)
# Usage: queue_skip
proc queue_skip {} {
    ::ess_queues::queue_skip
}

# Retry current item (reload config and restart)
# Usage: queue_retry
proc queue_retry {} {
    ::ess_queues::queue_retry
}

# Force complete current run (end early, close file, advance queue)
# Usage: queue_force_complete
proc queue_force_complete {} {
    ::ess_queues::force_complete
}

# Get current queue state
# Usage: queue_status
proc queue_status {} {
    return [list \
        status [dservGet queues/status] \
        active [dservGet queues/active] \
        position [dservGet queues/position] \
        total [dservGet queues/total] \
        current_config [dservGet queues/current_config] \
        run_count [dservGet queues/run_count]]
}

#=========================================================================
# Schema Management Commands
#=========================================================================

# Get schema information
# Usage: schema_info
proc schema_info {} {
    return [ess::configs::schema_info]
}

# Backup database
# Usage: schema_backup ?suffix?
proc schema_backup {{suffix ""}} {
    ess::configs::backup $suffix
}

# Reset database (dangerous!)
# Usage: schema_reset "YES_DELETE_ALL_DATA"
proc schema_reset {confirm_token} {
    ess::configs::reset $confirm_token
}

# Optimize database
# Usage: schema_optimize
proc schema_optimize {} {
    ess::configs::optimize
}

# Rebuild indexes
# Usage: schema_rebuild_indexes
proc schema_rebuild_indexes {} {
    ess::configs::rebuild_indexes
}

#=========================================================================
# Queue Publishing
#=========================================================================

proc queue_publish_list {} {
    ::ess_queues::publish_list
}

#=========================================================================
# Queue Export/Import for Cross-Rig Sync
#=========================================================================

# Export queue as JSON
# Usage: queue_export "name" ?-include_configs?
# Returns JSON with queue definition and optionally bundled config exports
proc queue_export {name args} {
    ::ess_queues::queue_export $name {*}$args
}

# Import queue from JSON
# Usage: queue_import {json} ?-skip_existing_configs? ?-overwrite_queue?
proc queue_import {json args} {
    ::ess_queues::queue_import $json {*}$args
}

# Push queue to remote rig
# Usage: queue_push "name" remote_host ?-include_configs?
proc queue_push {name remote_host args} {
    set json [queue_export $name {*}$args]
    send $remote_host "queue_import {$json} -skip_existing_configs"
}

# Push queue with configs, overwriting existing queue on remote
# Usage: queue_push_full "name" remote_host
proc queue_push_full {name remote_host} {
    set json [queue_export $name -include_configs]
    send $remote_host "queue_import {$json} -skip_existing_configs -overwrite_queue"
}

#=========================================================================
# Startup Complete
#=========================================================================

puts "Configs Manager subprocess ready"
puts "  Database: $configs_db"
puts "  Config commands: config_save, config_load, config_list, config_get, etc."
puts "  Queue commands: queue_create, queue_add, queue_start, queue_stop, etc."
puts "  Sync commands: config_push, config_import, queue_push, queue_import"

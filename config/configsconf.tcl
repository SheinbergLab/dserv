#
# configsconf.tcl - Configs Manager subprocess configuration
#
# This subprocess provides configuration persistence and management for ESS.
# It owns the configs database and handles all CRUD operations.
#
# Other threads interact with it via:
#   send configs "config_save name ?-tags {t1 t2}? ?-description text?"
#   send configs "config_load name"
#   send configs "config_list ?-tags {t1 t2}?"
#   send configs "config_get name"
#   etc.
#

package require dlsh
package require qpcs

# Add local lib to module path
tcl::tm::add $dspath/lib

# Standard subprocess setup
errormon enable
proc exit {args} { error "exit not available for this subprocess" }

# Load the configs module
package require ess_configs

#=========================================================================
# Initialize
#=========================================================================

# Database location
set configs_db [file join $dspath db configs.db]

# Ensure db directory exists
file mkdir [file dirname $configs_db]

# Initialize the configs system
ess::configs::init $configs_db

# Publish initial state for any connected clients
ess::configs::publish_all

#=========================================================================
# Convenience Commands - these are what other threads call via send
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

puts "Configs Manager subprocess ready"
puts "  Database: $configs_db"
puts "  Commands: config_save, config_load, config_list, config_get, etc."

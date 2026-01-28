# meshconf.tcl - Mesh broadcasting configuration
# 
# This subprocess handles broadcasting this node's status to the mesh.
# Discovery and peer tracking are handled by dserv-agent.
#
# Uses timer module for periodic heartbeat.
# Supports seed peers for cross-subnet discovery.

puts "Initializing mesh broadcasting"

# Disable exit for subprocess
proc exit {args} { error "exit not available for this subprocess" }

# Load required modules
load ${dspath}/modules/dserv_mesh[info sharedlibextension]
load ${dspath}/modules/dserv_timer[info sharedlibextension]

# Initialize mesh broadcaster
set ssl_enabled [dservGet system/ssl]
set web_port [dservGet system/webport]
if {$web_port eq ""} { set web_port 2565 }

meshInit \
    -id [dservGet system/hostname] \
    -name [dservGet system/hostname] \
    -port 12346 \
    -webport $web_port \
    -ssl $ssl_enabled

# Set the host's real IP address (determined by outbound routing)
meshSetField hostaddr [dservGet system/hostaddr]

#################################################################
# Seed Peer Configuration
#################################################################

# Configure seed peers for cross-subnet discovery
# These are well-known dserv-agent instances that aggregate mesh info
# Heartbeats are sent via unicast to seeds in addition to local broadcast

# Add seeds from a list
proc mesh_configure_seeds { seeds } {
    meshClearSeeds
    
    foreach seed $seeds {
        if {$seed ne ""} {
            meshAddSeed $seed
        }
    }
    
    set configured [meshGetSeeds]
    if {[llength $configured] > 0} {
        puts "Mesh seeds configured: $configured"
    } else {
        puts "Mesh: no seed peers configured (broadcast-only mode)"
    }
}

# Default seed configuration (used if no local config exists)
proc mesh_default_seed_config {} {
    # Default lab seeds - edit as needed for your environment
    mesh_configure_seeds {
    }
}

#################################################################
# Datapoint subscriptions
#################################################################

# Default heartbeat interval (milliseconds)
set mesh_interval 1000

# Subscribe to mesh-relevant datapoints from ess
set ess_dps { 
    status system protocol variant subject 
    obs_total obs_id
    block_n_complete block_pct_complete block_pct_correct
    rmt_host
}

foreach dp $ess_dps { 
    dservAddExactMatch ess/$dp 
}

# Datapoint handler for status updates
proc mesh_datapoint_handler { dpoint data } {
    # Extract field name from "ess/fieldname"
    set field [lindex [split $dpoint /] 1]
    
    # Status is special - uses meshUpdateStatus
    if {$field eq "status"} {
        meshUpdateStatus $data
        return
    }
    
    # Everything else: set or remove based on value
    if {$data ne ""} {
        meshSetField $field $data
    } else {
        meshRemoveField $field
    }
}

# Connect the handler to all mesh datapoints
foreach dp $ess_dps {
    dpointSetScript ess/$dp mesh_datapoint_handler
}

# Initialize with current values (in case ess is already running)
proc mesh_init_current_values {} {
    global ess_dps
    foreach dp $ess_dps {
        set val [dservGet ess/$dp]
        if {$val ne ""} {
            if {$dp eq "status"} {
                meshUpdateStatus $val
            } else {
                meshSetField $dp $val
            }
        }
    }
}

mesh_init_current_values

#################################################################
# Timer-based heartbeat
#################################################################

proc mesh_heartbeat_callback { dpoint data } {
    meshSendHeartbeat
}

proc mesh_start { {interval_ms 2000} } {
    global mesh_interval
    set mesh_interval $interval_ms
    timerTickInterval $interval_ms $interval_ms
    puts "Mesh heartbeat started: ${interval_ms}ms interval"
}

proc mesh_stop {} {
    timerStop
    puts "Mesh heartbeat stopped"
}

proc mesh_set_interval { interval_ms } {
    global mesh_interval
    set mesh_interval $interval_ms
    timerTickInterval $interval_ms $interval_ms
    puts "Mesh heartbeat interval changed to ${interval_ms}ms"
}

proc mesh_setup {} {
    timerPrefix meshTimer
    dservAddExactMatch meshTimer/0
    dpointSetScript meshTimer/0 mesh_heartbeat_callback
}

#################################################################
# Helper functions
#################################################################

# Set multiple fields at once using a dictionary
proc mesh_set_fields { field_dict } {
    dict for {key value} $field_dict {
        meshSetField $key $value
    }
}

# Get current broadcast info
proc mesh_get_info {} {
    return [meshInfo]
}

# Get this appliance's ID
proc mesh_get_id {} {
    return [meshGetApplianceId]
}

# Get current custom fields
proc mesh_get_fields {} {
    return [meshGetFields]
}

# Seed management helpers
proc mesh_add_seed { address } {
    meshAddSeed $address
    puts "Mesh: added seed $address"
}

proc mesh_remove_seed { address } {
    meshRemoveSeed $address
    puts "Mesh: removed seed $address"
}

proc mesh_list_seeds {} {
    return [meshGetSeeds]
}

# Get statistics
proc mesh_get_stats {} {
    return [meshStats]
}

#################################################################
# Initialize and start
#################################################################

# Local configuration in /usr/local/dserv/local/mesh.tcl
# Use this to override seed peers for specific machines/networks
if { [file exists $dspath/local/mesh.tcl] } {
    source $dspath/local/mesh.tcl
} else {
    mesh_default_seed_config
}

# Setup timer and start heartbeat
mesh_setup
mesh_start $mesh_interval

if { 0 } {
puts "Mesh broadcasting ready"
puts "  Appliance ID: [meshGetApplianceId]"
puts "  Heartbeat interval: ${mesh_interval}ms"
puts "  Seeds: [meshGetSeeds]"
puts ""
puts "Commands available:"
puts "  meshSendHeartbeat             - Send heartbeat now"
puts "  meshUpdateStatus <status>     - Set current status"
puts "  meshSetField <key> <value>    - Set custom field"
puts "  meshRemoveField <key>         - Remove custom field"
puts "  meshGetFields                 - Get all custom fields"
puts "  meshClearFields               - Clear all custom fields"
puts "  meshInfo                      - Get broadcaster info"
puts "  mesh_start ?interval_ms?      - Start/restart heartbeat timer"
puts "  mesh_stop                     - Stop heartbeat timer"
puts "  mesh_set_interval <ms>        - Change heartbeat interval"
puts ""
puts "Seed peer commands:"
puts "  meshAddSeed <address>         - Add a seed peer"
puts "  meshRemoveSeed <address>      - Remove a seed peer"
puts "  meshGetSeeds                  - List configured seeds"
puts "  meshClearSeeds                - Remove all seeds"
puts "  meshStats                     - Get send statistics"
}

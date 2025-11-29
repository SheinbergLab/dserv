puts "initializing mesh networking"

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

# Subscribe to mesh-relevant datapoints
dservAddExactMatch ess/status
dservAddExactMatch ess/system
dservAddExactMatch ess/protocol
dservAddExactMatch ess/variant
dservAddExactMatch ess/subject

# Datapoint handler for automatic status updates
proc mesh_datapoint_handler { dpoint data } {
    switch $dpoint {
        "ess/status" {
            meshUpdateStatus $data
        }

        "ess/subject" {
            if {$data ne ""} {
                meshSetField "subject" $data
            } else {
                meshRemoveField "subject"
            }
        }
        "ess/system" {
            if {$data ne ""} {
                meshSetField "system" $data
            } else {
                meshRemoveField "system"
            }
        }
        "ess/protocol" {
            if {$data ne ""} {
                meshSetField "protocol" $data
            } else {
                meshRemoveField "protocol"
            }
        }
        "ess/variant" {
            if {$data ne ""} {
                meshSetField "variant" $data
            } else {
                meshRemoveField "variant"
            }
        }
    }
}

# Connect the handler to all mesh datapoints
foreach dp { status subject system variant protocol } {
    dpointSetScript ess/$dp mesh_datapoint_handler
}

# Set multiple fields at once using a dictionary
proc mesh_set_fields { field_dict } {
    dict for {key value} $field_dict {
        meshSetField $key $value
    }
}

#################################################################
# Helper functions
#################################################################

# Get cluster status as a nice Tcl dict
proc mesh_get_cluster_status {} {
    return [meshGetPeers]
}

# Get just the appliance IDs
proc mesh_get_appliance_ids {} {
    set peers [meshGetPeers]
    set ids {}
    foreach peer $peers {
        lappend ids [dict get $peer id]
    }
    return $ids
}

# Check if a specific appliance is online
proc mesh_appliance_online { appliance_id } {
    set ids [mesh_get_appliance_ids]
    return [expr {$appliance_id in $ids}]
}

# Get status of specific appliance
proc mesh_get_appliance_status { appliance_id } {
    set peers [meshGetPeers]
    foreach peer $peers {
        if {[dict get $peer id] eq $appliance_id} {
            return [dict get $peer status]
        }
    }
    return ""
}

# As simple as it gets
proc mesh_generate_html {} {
    set hostname [expr {[dservExists system/hostname] ? [dservGet system/hostname] : "Unknown"}]
    set status [expr {[dservExists ess/status] ? [dservGet ess/status] : "idle"}]
    
    return "<html><body><h1>$hostname</h1><p>Status: $status</p><p>Time: [clock format [clock seconds]]</p></body></html>"
}

puts "Mesh configuration ready - flexible field system active"
puts "Standard fields mapped:"
puts "  ess/status   -> status"
puts "  ess/subject  -> subject"
puts "  ess/system   -> system"
puts "  ess/protocol -> protocol"
puts "  ess/variant  -> variant"
puts ""
puts "Add your own fields with:"
puts "  meshSetField <fieldname> <value>"
puts "  meshRemoveField <fieldname>"
puts "  meshGetFields"

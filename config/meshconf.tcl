# meshconf.tcl - Mesh heartbeat configuration (HTTP-based)
# 
# This subprocess handles sending heartbeats to the mesh registry.
# The registry returns current mesh state which is published to mesh/peers.
#
# Configuration:
#   mesh_registry  - URL of the mesh registry (e.g., https://dserv.io)
#   mesh_workgroup - Workgroup name for this machine
#
# Uses timer module for periodic heartbeat.
# Uses yajltcl for JSON encoding/decoding.
# Uses https_post for HTTPS communication.

puts "Initializing mesh heartbeat (HTTP)"

# Disable exit for subprocess
proc exit {args} { error "exit not available for this subprocess" }

# Load required modules
load ${dspath}/modules/dserv_timer[info sharedlibextension]

# JSON support via yajltcl
package require yajltcl

# https_post command should be available (registered by dserv)

#################################################################
# Configuration
#################################################################

# Registry URL - override in local/mesh.tcl
set mesh_registry ""
set mesh_workgroup ""

# Heartbeat interval (milliseconds)  
set mesh_interval 5000

# Local node info
set mesh_hostname [dservGet system/hostname]
set mesh_hostaddr [dservGet system/hostaddr]
set mesh_webport [dservGet system/webport]
set mesh_ssl [dservGet system/ssl]

if {$mesh_webport eq ""} { set mesh_webport 2565 }
if {$mesh_ssl eq ""} { set mesh_ssl 0 }

# Current status
set mesh_status "idle"

# Custom fields to include in heartbeat
set mesh_fields [dict create]

#################################################################
# Datapoint subscriptions
#################################################################

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
    global mesh_fields mesh_status
    
    # Extract field name from "ess/fieldname"
    set field [lindex [split $dpoint /] 1]
    
    # Status is tracked separately
    if {$field eq "status"} {
        set mesh_status $data
        return
    }
    
    # Everything else: set or remove based on value
    if {$data ne ""} {
        dict set mesh_fields $field $data
    } else {
        if {[dict exists $mesh_fields $field]} {
            dict unset mesh_fields $field
        }
    }
}

# Connect the handler to all mesh datapoints
foreach dp $ess_dps {
    dpointSetScript ess/$dp mesh_datapoint_handler
}

# Initialize with current values (in case ess is already running)
proc mesh_init_current_values {} {
    global ess_dps mesh_fields mesh_status
    foreach dp $ess_dps {
        # These are owned by the `ess` subprocess and are absent until it has
        # loaded a system -- which is exactly what "in case ess is already
        # running" means. A bare dservGet of an absent dpoint RAISES (see
        # Dataserver.cpp dserv_get_command), so on a box where essconf.tcl
        # failed, this loop aborted meshconf.tcl and left mesh with no ess
        # subscriptions at all. Skip what is not there yet; the
        # dpointSetScript handlers below pick each field up when ess
        # publishes it.
        if { ![dservExists ess/$dp] } { continue }
        set val [dservGet ess/$dp]
        if {$val ne ""} {
            if {$dp eq "status"} {
                set mesh_status $val
            } else {
                dict set mesh_fields $dp $val
            }
        }
    }
}

mesh_init_current_values

#################################################################
# Network path (published by the netmon subprocess on Linux boxes)
#################################################################

# netmon owns system/net/* — the live route toward the registry. Follow
# it the same way we follow ess/*: the advertised heartbeat IP tracks
# the path actually in use (wifi vs ethernet can flip when both have
# addresses), and link telemetry rides along as customFields so the
# registry can show a remote box's signal quality. On platforms without
# netmon these dpoints never appear and mesh_hostaddr keeps its boot
# value from system/hostaddr. All the platform-specific sensing lives
# in netmonconf.tcl; nothing here execs anything.

set net_dps {
    system/net/ip system/net/type system/net/wifi/ssid system/net/wifi/bars
}

foreach dp $net_dps {
    dservAddExactMatch $dp
}

proc mesh_net_handler { dpoint data } {
    global mesh_hostaddr mesh_fields

    if {$dpoint eq "system/net/ip"} {
        if {$data ne "" && $data ne $mesh_hostaddr} {
            set mesh_hostaddr $data
            # Converge the registry promptly after a path flip
            mesh_send_heartbeat
        }
        return
    }

    set field [dict get {
        system/net/type      net_type
        system/net/wifi/ssid net_ssid
        system/net/wifi/bars net_bars
    } $dpoint]
    if {$data ne ""} {
        dict set mesh_fields $field $data
    } else {
        if {[dict exists $mesh_fields $field]} {
            dict unset mesh_fields $field
        }
    }
}

foreach dp $net_dps {
    dpointSetScript $dp mesh_net_handler
}

#################################################################
# HTTP Heartbeat
#################################################################

proc mesh_build_heartbeat {} {
    global mesh_hostname mesh_hostaddr mesh_webport mesh_ssl
    global mesh_workgroup mesh_status mesh_fields
    
    set json [yajl create #auto]
    
    $json map_open
    $json string hostname   string $mesh_hostname
    $json string ip         string $mesh_hostaddr
    $json string port       number $mesh_webport
    $json string ssl        bool   [expr {$mesh_ssl ? 1 : 0}]
    $json string workgroup  string $mesh_workgroup
    $json string status     string $mesh_status
    
    $json string customFields map_open
    dict for {k v} $mesh_fields {
        # Determine type: try number first, fall back to string
        if {[string is integer -strict $v]} {
            $json string $k number $v
        } elseif {[string is double -strict $v]} {
            $json string $k number $v
        } else {
            $json string $k string $v
        }
    }
    $json map_close
    
    $json map_close
    
    set result [$json get]
    $json delete
    return $result
}

# The green light, published under REGISTRY (registry/health -- see
# settings-1.0.tm): mesh declares no knobs of its own, so it has no gear
# section -- and the heartbeat is precisely the runtime answer for the
# declared `registry url/workgroup` pair, which is where someone looking
# will look. Change-guarded, because the 5s timer would otherwise
# republish an identical line forever.
set mesh_health_last ""
proc mesh_health {state {detail ""}} {
    global mesh_health_last
    set h [string trim "$state $detail"]
    if {$h eq $mesh_health_last} { return }
    set mesh_health_last $h
    catch { dservSet registry/health $h }
}

proc mesh_send_heartbeat {} {
    global mesh_registry mesh_workgroup

    # Skip if not configured
    if {$mesh_registry eq "" || $mesh_workgroup eq ""} {
        mesh_health off "not configured -- no registry url/workgroup declared"
        return
    }

    # Offline mode (local/offline or DSERV_OFFLINE=1): skip silently
    # rather than logging a refused POST every interval. Loopback
    # registries are exempt, matching TclHttps's socket-layer rule.
    if {[info exists ::env(DSERV_OFFLINE)]
        && $::env(DSERV_OFFLINE) ne "" && $::env(DSERV_OFFLINE) ne "0"
        && ![regexp {^https?://(localhost|127\.[0-9.]+|\[::1\])(:|/|$)} $mesh_registry]} {
        mesh_health off "offline mode -- no heartbeats"
        return
    }

    set url "${mesh_registry}/api/v1/heartbeat"
    set body [mesh_build_heartbeat]

    # Send HTTP POST using tclhttps
    if {[catch {
        set response [https_post $url $body -timeout 5000]
        mesh_process_response $response
    } err]} {
        puts "Mesh heartbeat error: $err"
        mesh_health error "registry unreachable: [string range $err 0 80]"
    }
}

proc mesh_process_response { response_json } {
    global mesh_registry mesh_workgroup

    # Parse response using yajltcl
    if {[catch {
        set response [::yajl::json2dict $response_json]
    } err]} {
        puts "Mesh: invalid JSON response: $err"
        mesh_health error "registry answered, but not with JSON: [string range $err 0 60]"
        return
    }

    # Check ok field
    if {![dict exists $response ok] || ![dict get $response ok]} {
        set _why "heartbeat rejected"
        if {[dict exists $response error]} {
            puts "Mesh registry error: [dict get $response error]"
            set _why "heartbeat rejected: [dict get $response error]"
        }
        mesh_health error $_why
        return
    }

    mesh_health ok "heartbeat acked at $mesh_registry ($mesh_workgroup)"

    # Extract mesh array and publish to dserv
    if {[dict exists $response mesh]} {
        set mesh_list [dict get $response mesh]
        
        # Convert back to JSON for dserv datapoint using yajltcl
        set json [yajl create #auto]
        $json array_open
        
        foreach node $mesh_list {
            $json map_open
            dict for {k v} $node {
                switch $k {
                    port - lastSeenAgo {
                        $json string $k number $v
                    }
                    ssl - isLocal {
                        $json string $k bool [expr {$v ? 1 : 0}]
                    }
                    customFields {
                        $json string $k map_open
                        if {[llength $v] > 0} {
                            dict for {ck cv} $v {
                                if {[string is integer -strict $cv]} {
                                    $json string $ck number $cv
                                } elseif {[string is double -strict $cv]} {
                                    $json string $ck number $cv
                                } else {
                                    $json string $ck string $cv
                                }
                            }
                        }
                        $json map_close
                    }
                    default {
                        $json string $k string $v
                    }
                }
            }
            $json map_close
        }
        
        $json array_close
        set mesh_json [$json get]
        $json delete
        
        # Publish to local dserv for any scripts that want mesh info
        dservSetData mesh/peers 0 11 $mesh_json
    }
}

#################################################################
# Timer-based heartbeat
#################################################################

proc mesh_heartbeat_callback { dpoint data } {
    mesh_send_heartbeat
}

proc mesh_start { {interval_ms 5000} } {
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
# Configuration helpers
#################################################################

proc mesh_configure { registry workgroup } {
    global mesh_registry mesh_workgroup
    
    # Normalize registry URL - add scheme if missing
    if {![regexp {^https?://} $registry]} {
        set registry "http://$registry"
    }
    
    # Add default port if missing
    if {![regexp {:\d+(/|$)} $registry]} {
        if {[string match "https://*" $registry]} {
            # Remove trailing slash, add port, restore path
            regexp {^(https://[^/]+)(.*)} $registry -> base path
            set registry "${base}:443${path}"
        } else {
            regexp {^(http://[^/]+)(.*)} $registry -> base path
            set registry "${base}:80${path}"
        }
    }
    
    set mesh_registry $registry
    set mesh_workgroup $workgroup
    # netmon watches the route toward the registry; publish it for them
    catch { dservSet mesh/registry $registry }
    puts "Mesh configured: registry=$registry workgroup=$workgroup"
}

proc mesh_set_field { key value } {
    global mesh_fields
    dict set mesh_fields $key $value
}

proc mesh_remove_field { key } {
    global mesh_fields
    if {[dict exists $mesh_fields $key]} {
        dict unset mesh_fields $key
    }
}

proc mesh_get_fields {} {
    global mesh_fields
    return $mesh_fields
}

proc mesh_clear_fields {} {
    global mesh_fields
    set mesh_fields [dict create]
}

proc mesh_get_info {} {
    global mesh_registry mesh_workgroup mesh_hostname mesh_hostaddr
    global mesh_webport mesh_ssl mesh_status mesh_interval
    
    return [dict create \
        registry $mesh_registry \
        workgroup $mesh_workgroup \
        hostname $mesh_hostname \
        ip $mesh_hostaddr \
        port $mesh_webport \
        ssl $mesh_ssl \
        status $mesh_status \
        interval $mesh_interval \
    ]
}

# Force a heartbeat now
proc mesh_heartbeat_now {} {
    mesh_send_heartbeat
}

#################################################################
# Initialize and start
#################################################################

# Local configuration in /usr/local/dserv/local/mesh.tcl
# Use this to set registry and workgroup:
#   mesh_configure "https://dserv.io" "brown-sheinberg"
#
if { [file exists $dspath/local/mesh.tcl] } {
    source $dspath/local/mesh.tcl
}

# Otherwise take the rig's declared registry (settings `registry url` /
# `registry workgroup`, exported to the environment by config/rig_settings.tcl
# before this subprocess existed). Mesh and the script registry are the same
# server with the same workgroup on every rig here, and having the pair
# written down twice is how they drift apart. local/mesh.tcl still wins where
# it exists, so a box that deliberately meshes elsewhere is unaffected.
if { $mesh_registry eq "" && $mesh_workgroup eq "" &&
     [info exists ::env(ESS_REGISTRY_URL)] && $::env(ESS_REGISTRY_URL) ne "" &&
     [info exists ::env(ESS_WORKGROUP)] && $::env(ESS_WORKGROUP) ne "" } {
    mesh_configure $::env(ESS_REGISTRY_URL) $::env(ESS_WORKGROUP)
}

# Pick up the network path netmon seeded before we started (netmon runs
# earlier in dsconf.tcl; a safe no-op on boxes without it). Done here —
# after mesh_configure has run — so an ip pickup can heartbeat usefully.
foreach dp $net_dps {
    if { [dservExists $dp] } {
        mesh_net_handler $dp [dservGet $dp]
    }
}

# Setup timer and start heartbeat
mesh_setup
mesh_start $mesh_interval

puts "Mesh heartbeat ready"
puts "  Hostname: $mesh_hostname"
puts "  Registry: [expr {$mesh_registry ne {} ? $mesh_registry : {(not configured)}}]"
puts "  Workgroup: [expr {$mesh_workgroup ne {} ? $mesh_workgroup : {(not configured)}}]"
puts "  Heartbeat interval: ${mesh_interval}ms"
puts ""
puts "Commands available:"
puts "  mesh_configure <registry> <workgroup>  - Configure registry"
puts "  mesh_heartbeat_now                     - Send heartbeat immediately"
puts "  mesh_set_field <key> <value>           - Set custom field"
puts "  mesh_remove_field <key>                - Remove custom field"
puts "  mesh_get_fields                        - Get all custom fields"
puts "  mesh_clear_fields                      - Clear all custom fields"
puts "  mesh_get_info                          - Get current config"
puts "  mesh_start ?interval_ms?               - Start/restart heartbeat"
puts "  mesh_stop                              - Stop heartbeat"
puts "  mesh_set_interval <ms>                 - Change interval"

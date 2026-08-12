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
# Registry path / network status
#################################################################

# Classify a Linux netdev as wifi vs ethernet (wireless sysfs wins).
proc mesh_iface_type { iface } {
    if {$iface eq ""} { return "" }
    if {[file isdirectory "/sys/class/net/$iface/wireless"]} {
        return wifi
    }
    return ethernet
}

# Resolve hostname -> IPv4 via getent (no packets). Returns "" on failure.
proc mesh_resolve_ipv4 { host } {
    if {$host eq ""} { return "" }
    # Already an IPv4 literal?
    if {[regexp {^\d+\.\d+\.\d+\.\d+$} $host]} { return $host }
    set out ""
    if {[catch { set out [exec getent ahostsv4 $host] }]} {
        return ""
    }
    # First field of first line is the address
    set line [lindex [split [string trim $out] \n] 0]
    set addr [lindex $line 0]
    if {[regexp {^\d+\.\d+\.\d+\.\d+$} $addr]} { return $addr }
    return ""
}

# Kernel route lookup toward $dest (hostname or IPv4). Returns
# {ip iface} or empty list. Sends no packets.
proc mesh_route_lookup { dest } {
    if {$dest eq "" || $::tcl_platform(os) ne "Linux"} {
        return [list]
    }
    set target [mesh_resolve_ipv4 $dest]
    if {$target eq ""} {
        # Literal IPv4 already, or unresolvable hostname
        if {[regexp {^\d+\.\d+\.\d+\.\d+$} $dest]} {
            set target $dest
        } else {
            return [list]
        }
    }
    set out ""
    if {[catch { set out [exec ip -4 route get $target] }]} {
        return [list]
    }
    set ip ""
    set iface ""
    regexp {src (\S+)} $out -> ip
    regexp {dev (\S+)} $out -> iface
    if {$ip eq "" || $iface eq ""} {
        return [list]
    }
    return [list $ip $iface]
}

# Extract host from registry URL (https://dserv.net:443/path -> dserv.net).
proc mesh_registry_host {} {
    global mesh_registry
    if {$mesh_registry eq ""} { return "" }
    if {[regexp {^https?://\[([^\]]+)\]} $mesh_registry -> host]} {
        return $host
    }
    if {[regexp {^https?://([^/:]+)} $mesh_registry -> host]} {
        return $host
    }
    return ""
}

# Map RSSI (dBm) to 0..4 bars for the status-bar icon.
proc mesh_wifi_bars_from_dbm { dbm } {
    if {![string is integer -strict $dbm]} { return 0 }
    if {$dbm >= -50} { return 4 }
    if {$dbm >= -60} { return 3 }
    if {$dbm >= -70} { return 2 }
    if {$dbm >= -80} { return 1 }
    return 0
}

# Clear wifi association datapoints (ethernet path, or iw failed).
proc mesh_clear_wifi_info {} {
    catch { dservSet system/net/wifi/ssid "" }
    catch { dservSet system/net/wifi/bssid "" }
    catch { dservSet system/net/wifi/signal_dbm "" }
    catch { dservSet system/net/wifi/bars "" }
}

# Current association only (iw link) — no scan. Publishes ssid/bssid/signal/bars.
proc mesh_refresh_wifi_info { iface } {
    if {$iface eq "" || $::tcl_platform(os) ne "Linux"} {
        mesh_clear_wifi_info
        return
    }

    set out ""
    if {[catch { set out [exec iw dev $iface link] }]} {
        mesh_clear_wifi_info
        return
    }

    # Not associated
    if {[string match "*Not connected*" $out] || $out eq ""} {
        mesh_clear_wifi_info
        return
    }

    set bssid ""
    set ssid ""
    set dbm ""
    regexp -nocase {Connected to ([0-9a-f:]+)} $out -> bssid
    # [^\n]+ — plain .+ can span lines in this Tcl build
    regexp {SSID:\s*([^\n]+)} $out -> ssid
    regexp {signal:\s*(-?\d+)\s*dBm} $out -> dbm
    set ssid [string trim $ssid]

    if {$ssid eq "" && $bssid eq "" && $dbm eq ""} {
        mesh_clear_wifi_info
        return
    }

    set bars [mesh_wifi_bars_from_dbm $dbm]
    catch { dservSet system/net/wifi/ssid $ssid }
    catch { dservSet system/net/wifi/bssid [string toupper $bssid] }
    catch { dservSet system/net/wifi/signal_dbm $dbm }
    catch { dservSet system/net/wifi/bars $bars }
}

# Refresh mesh_hostaddr + system/net/* from the path used toward the
# registry (fallback: default-route lookup via 1.1.1.1). Safe to call
# often; all execs are caught.
proc mesh_refresh_registry_path {} {
    global mesh_hostaddr mesh_registry

    set ip ""
    set iface ""

    set host [mesh_registry_host]
    if {$host ne "" && $host ne "localhost" && $host ne "127.0.0.1"
        && $host ne "::1"} {
        set route [mesh_route_lookup $host]
        if {[llength $route] == 2} {
            lassign $route ip iface
        }
    }

    if {$ip eq ""} {
        set route [mesh_route_lookup 1.1.1.1]
        if {[llength $route] == 2} {
            lassign $route ip iface
        }
    }

    if {$ip eq "" || $iface eq ""} {
        return
    }

    set type [mesh_iface_type $iface]
    set mesh_hostaddr $ip
    catch { dservSet system/hostaddr $ip }
    catch { dservSet system/net/ip $ip }
    catch { dservSet system/net/iface $iface }
    catch { dservSet system/net/type $type }

    if {$type eq "wifi"} {
        mesh_refresh_wifi_info $iface
    } else {
        mesh_clear_wifi_info
    }
}

#################################################################
# Net status modal (on-demand choices — not on the mesh tick)
#################################################################

# Split nmcli -t fields (\: is a literal colon).
proc mesh_nmcli_split { line } {
    set tmp [string map {\\: <<COLON>>} $line]
    set parts [split $tmp :]
    set out {}
    foreach p $parts {
        lappend out [string map {<<COLON>> :} $p]
    }
    return $out
}

# Saved Wi-Fi connection names (SSID / profile name).
proc mesh_net_known_ssids {} {
    set known {}
    set out ""
    if {[catch { set out [exec nmcli -t -f NAME,TYPE connection show] }]} {
        return $known
    }
    foreach line [split [string trimright $out \n] \n] {
        if {$line eq ""} { continue }
        set parts [mesh_nmcli_split $line]
        if {[llength $parts] < 2} { continue }
        set name [lindex $parts 0]
        set typ [lindex $parts 1]
        if {$typ eq "802-11-wireless" && $name ne ""} {
            lappend known $name
        }
    }
    return $known
}

# Current association for one wifi iface: {ssid bssid} or empty.
proc mesh_net_iw_assoc { iface } {
    set ssid ""
    set bssid ""
    set out ""
    if {[catch { set out [exec iw dev $iface link] }]} {
        return [list]
    }
    if {[string match "*Not connected*" $out] || $out eq ""} {
        return [list]
    }
    regexp -nocase {Connected to ([0-9a-f:]+)} $out -> bssid
    regexp {SSID:\s*([^\n]+)} $out -> ssid
    set ssid [string trim $ssid]
    if {$ssid eq "" && $bssid eq ""} { return [list] }
    return [list $ssid [string toupper $bssid]]
}

# Interfaces with a global IPv4 (excludes lo).
# Returns list of dicts: iface type ip ssid bssid
proc mesh_net_interfaces {} {
    set rows {}
    set out ""
    if {[catch { set out [exec ip -4 -o addr show scope global] }]} {
        return $rows
    }
    foreach line [split [string trimright $out \n] \n] {
        if {$line eq ""} { continue }
        # "3: wlan0    inet 10.14.17.221/24 ..."
        if {![regexp {^\d+:\s+(\S+)\s+inet\s+(\d+\.\d+\.\d+\.\d+)} $line -> iface ip]} {
            continue
        }
        if {$iface eq "lo"} { continue }
        set type [mesh_iface_type $iface]
        set ssid ""
        set bssid ""
        if {$type eq "wifi"} {
            set assoc [mesh_net_iw_assoc $iface]
            if {[llength $assoc] == 2} {
                lassign $assoc ssid bssid
            }
        }
        lappend rows [dict create \
            iface $iface type $type ip $ip ssid $ssid bssid $bssid]
    }
    return $rows
}

# Scan APs; keep only SSIDs in $known (list). Returns list of dicts.
proc mesh_net_scan_aps { known } {
    set rows {}
    if {[llength $known] == 0} { return $rows }

    set out ""
    if {[catch {
        set out [exec nmcli -t -f IN-USE,SSID,BSSID,SIGNAL,DEVICE device wifi list --rescan yes]
    } err]} {
        error "wifi scan failed: $err"
    }

    foreach line [split [string trimright $out \n] \n] {
        if {$line eq ""} { continue }
        set parts [mesh_nmcli_split $line]
        if {[llength $parts] < 5} { continue }
        lassign $parts in_use ssid bssid signal device
        if {$ssid eq "" || [lsearch -exact $known $ssid] < 0} { continue }
        set in_use_bool [expr {$in_use eq "*"}]
        if {![string is integer -strict $signal]} { set signal 0 }
        lappend rows [dict create \
            ssid $ssid \
            bssid [string toupper $bssid] \
            signal $signal \
            device $device \
            in_use $in_use_bool]
    }

    # Sort by SSID asc, then signal desc
    set rows [lsort -command mesh_net_ap_sort $rows]
    return $rows
}

proc mesh_net_ap_sort { a b } {
    set sa [dict get $a ssid]
    set sb [dict get $b ssid]
    set c [string compare $sa $sb]
    if {$c != 0} { return $c }
    set qa [dict get $a signal]
    set qb [dict get $b signal]
    if {$qa > $qb} { return -1 }
    if {$qa < $qb} { return 1 }
    return 0
}

# Current registry-path fields from datapoints.
proc mesh_net_current_info {} {
    set cur [dict create iface "" type "" ip "" ssid "" bssid ""]
    catch {
        if {[dservExists system/net/iface]} {
            dict set cur iface [dservGet system/net/iface]
        }
        if {[dservExists system/net/type]} {
            dict set cur type [dservGet system/net/type]
        }
        if {[dservExists system/net/ip]} {
            dict set cur ip [dservGet system/net/ip]
        }
        if {[dservExists system/net/wifi/ssid]} {
            dict set cur ssid [dservGet system/net/wifi/ssid]
        }
        if {[dservExists system/net/wifi/bssid]} {
            dict set cur bssid [dservGet system/net/wifi/bssid]
        }
    }
    return $cur
}

# Encode modal payload. $aps is a list of AP dicts.
proc mesh_net_choices_json { cur ifaces aps error } {
    set cur_iface [dict get $cur iface]

    set json [yajl create #auto]
    $json map_open

    $json string current map_open
    $json string iface string [dict get $cur iface]
    $json string type string [dict get $cur type]
    $json string ip string [dict get $cur ip]
    $json string ssid string [dict get $cur ssid]
    $json string bssid string [dict get $cur bssid]
    $json map_close

    $json string interfaces array_open
    foreach row $ifaces {
        set iface [dict get $row iface]
        set is_cur [expr {$cur_iface ne "" && $iface eq $cur_iface}]
        $json map_open
        $json string iface string $iface
        $json string type string [dict get $row type]
        $json string ip string [dict get $row ip]
        $json string ssid string [dict get $row ssid]
        $json string bssid string [dict get $row bssid]
        $json string current bool $is_cur
        $json map_close
    }
    $json array_close

    $json string access_points array_open
    foreach row $aps {
        $json map_open
        $json string ssid string [dict get $row ssid]
        $json string bssid string [dict get $row bssid]
        $json string signal number [dict get $row signal]
        $json string device string [dict get $row device]
        $json string in_use bool [dict get $row in_use]
        $json map_close
    }
    $json array_close

    $json string error string $error
    $json map_close

    set result [$json get]
    $json delete
    return $result
}

# Fast: interfaces + current registry path (no wifi scan).
proc mesh_net_iface_choices {} {
    set error ""
    set ifaces {}
    set cur [mesh_net_current_info]
    if {[catch { set ifaces [mesh_net_interfaces] } err]} {
        set error $err
        set ifaces {}
    }
    return [mesh_net_choices_json $cur $ifaces {} $error]
}

# Slow: wifi AP scan only (known SSIDs).
proc mesh_net_ap_scan {} {
    set error ""
    set aps {}
    if {[catch {
        set known [mesh_net_known_ssids]
        set aps [mesh_net_scan_aps $known]
    } err]} {
        set error $err
        set aps {}
    }

    set json [yajl create #auto]
    $json map_open
    $json string access_points array_open
    foreach row $aps {
        $json map_open
        $json string ssid string [dict get $row ssid]
        $json string bssid string [dict get $row bssid]
        $json string signal number [dict get $row signal]
        $json string device string [dict get $row device]
        $json string in_use bool [dict get $row in_use]
        $json map_close
    }
    $json array_close
    $json string error string $error
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

# Full snapshot (ifaces + scan). Kept for convenience / debugging.
proc mesh_net_choices {} {
    set error ""
    set ifaces {}
    set aps {}
    set cur [mesh_net_current_info]

    if {[catch {
        set ifaces [mesh_net_interfaces]
        set known [mesh_net_known_ssids]
        if {[catch { set aps [mesh_net_scan_aps $known] } scan_err]} {
            set error $scan_err
            set aps {}
        }
    } err]} {
        set error $err
    }

    return [mesh_net_choices_json $cur $ifaces $aps $error]
}

#################################################################
# Net switch (prefer iface / BSSID for registry path)
#################################################################

proc mesh_net_result_json { ok error } {
    set json [yajl create #auto]
    $json map_open
    $json string ok bool $ok
    $json string error string $error
    $json map_close
    set result [$json get]
    $json delete
    return $result
}

# Active NM connection name for a device, or "".
proc mesh_net_conn_for_iface { iface } {
    if {$iface eq ""} { return "" }
    set out ""
    if {[catch { set out [exec nmcli -t -f GENERAL.CONNECTION device show $iface] }]} {
        return ""
    }
    set name ""
    foreach line [split $out \n] {
        if {[regexp {^GENERAL\.CONNECTION:(.*)$} $line -> v]} {
            set name [string trim $v]
            break
        }
    }
    if {$name eq "" || $name eq "--"} { return "" }
    return $name
}

# List {device type connection} for active eth/wifi devices.
proc mesh_net_active_links {} {
    set rows {}
    set out ""
    if {[catch { set out [exec nmcli -t -f DEVICE,TYPE,CONNECTION device status] }]} {
        return $rows
    }
    foreach line [split [string trimright $out \n] \n] {
        if {$line eq ""} { continue }
        set parts [mesh_nmcli_split $line]
        if {[llength $parts] < 3} { continue }
        lassign $parts device typ conn
        if {$typ ne "ethernet" && $typ ne "wifi"} { continue }
        if {$conn eq "" || $conn eq "--"} { continue }
        lappend rows [list $device $typ $conn]
    }
    return $rows
}

# Prefer prefer_iface (metric 50); other active eth/wifi connections get 600.
# Returns the NM connection name for prefer_iface (may be "").
proc mesh_net_set_metrics { prefer_iface } {
    set links [mesh_net_active_links]
    set prefer_conn ""

    foreach row $links {
        lassign $row device typ conn
        set metric 600
        if {$device eq $prefer_iface} {
            set metric 50
            set prefer_conn $conn
        }
        catch { exec nmcli connection modify id $conn ipv4.route-metric $metric }
    }

    if {$prefer_conn eq ""} {
        set prefer_conn [mesh_net_conn_for_iface $prefer_iface]
        if {$prefer_conn eq ""} {
            set out ""
            if {![catch { set out [exec nmcli -t -f NAME,DEVICE,TYPE connection show] }]} {
                foreach line [split [string trimright $out \n] \n] {
                    set parts [mesh_nmcli_split $line]
                    if {[llength $parts] < 3} { continue }
                    lassign $parts name device typ
                    if {$device eq $prefer_iface ||
                        ($device eq "" && $typ eq [mesh_iface_type $prefer_iface])} {
                        # fall through: NAME match by later callers for wifi SSID
                    }
                    if {$device eq $prefer_iface} {
                        set prefer_conn $name
                        break
                    }
                }
            }
        }
        if {$prefer_conn ne ""} {
            catch { exec nmcli connection modify id $prefer_conn ipv4.route-metric 50 }
        }
    }
    return $prefer_conn
}

# Re-activate non-preferred links so updated metrics hit the routing table.
proc mesh_net_reactivate_others { prefer_iface } {
    foreach row [mesh_net_active_links] {
        lassign $row device typ conn
        if {$device eq $prefer_iface} { continue }
        catch { exec nmcli -w 25 connection up id $conn ifname $device }
    }
}

# Switch registry preference to a non-wifi interface (ethernet).
proc mesh_net_switch_iface { iface } {
    if {$iface eq ""} {
        return [mesh_net_result_json 0 "missing interface"]
    }
    if {![regexp {^[A-Za-z0-9._-]+$} $iface]} {
        return [mesh_net_result_json 0 "invalid interface name"]
    }
    set type [mesh_iface_type $iface]
    if {$type eq "wifi"} {
        return [mesh_net_result_json 0 "choose a Wi-Fi access point to switch wireless"]
    }

    if {[catch {
        set conn [mesh_net_set_metrics $iface]
        if {$conn eq ""} {
            catch { exec nmcli -w 30 device connect $iface }
            set conn [mesh_net_conn_for_iface $iface]
            if {$conn eq ""} {
                error "no NetworkManager connection for $iface"
            }
            catch { exec nmcli connection modify id $conn ipv4.route-metric 50 }
        }
        exec nmcli -w 30 connection up id $conn ifname $iface
        mesh_net_reactivate_others $iface
        catch { exec sleep 1 }
        mesh_refresh_registry_path
    } err]} {
        return [mesh_net_result_json 0 $err]
    }
    return [mesh_net_result_json 1 ""]
}

# Switch to a specific AP on a wifi device (known SSID credentials).
# Temporarily pins 802-11-wireless.bssid so NM associates to that AP, then
# clears the pin so reboot / roam can pick any BSSID for the SSID. Route
# metric preference (50) stays persistent.
proc mesh_net_switch_ap { device ssid bssid } {
    if {$device eq "" || $ssid eq "" || $bssid eq ""} {
        return [mesh_net_result_json 0 "missing device, ssid, or bssid"]
    }
    if {![regexp {^[A-Za-z0-9._-]+$} $device]} {
        return [mesh_net_result_json 0 "invalid device name"]
    }
    if {![regexp -nocase {^[0-9a-f]{2}(:[0-9a-f]{2}){5}$} $bssid]} {
        return [mesh_net_result_json 0 "invalid bssid"]
    }
    if {[regexp {[{}\[\]$;\\]} $ssid]} {
        return [mesh_net_result_json 0 "invalid ssid"]
    }

    set type [mesh_iface_type $device]
    if {$type ne "wifi"} {
        return [mesh_net_result_json 0 "$device is not a Wi-Fi interface"]
    }

    if {[catch {
        mesh_net_set_metrics $device
        exec nmcli connection modify id $ssid \
            802-11-wireless.bssid $bssid \
            ipv4.route-metric 50
        exec nmcli -w 45 connection up id $ssid ifname $device
        # Drop the pin after associate — do not insist on this AP across reboot.
        catch { exec nmcli connection modify id $ssid 802-11-wireless.bssid "" }
        mesh_net_reactivate_others $device
        catch { exec sleep 1 }
        mesh_refresh_registry_path
    } err]} {
        # Best-effort: never leave a sticky BSSID pin behind on failure either.
        catch { exec nmcli connection modify id $ssid 802-11-wireless.bssid "" }
        return [mesh_net_result_json 0 $err]
    }
    return [mesh_net_result_json 1 ""]
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

proc mesh_send_heartbeat {} {
    global mesh_registry mesh_workgroup

    # Keep advertised IP + UI net icon aligned with the live path to
    # the registry (wifi vs ethernet can flip when both have addresses).
    # Run even when heartbeat is skipped so the icon stays current.
    mesh_refresh_registry_path

    # Skip if not configured
    if {$mesh_registry eq "" || $mesh_workgroup eq ""} {
        return
    }

    # Offline mode (local/offline or DSERV_OFFLINE=1): skip silently
    # rather than logging a refused POST every interval. Loopback
    # registries are exempt, matching TclHttps's socket-layer rule.
    if {[info exists ::env(DSERV_OFFLINE)]
        && $::env(DSERV_OFFLINE) ne "" && $::env(DSERV_OFFLINE) ne "0"
        && ![regexp {^https?://(localhost|127\.[0-9.]+|\[::1\])(:|/|$)} $mesh_registry]} {
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
    }
}

proc mesh_process_response { response_json } {
    # Parse response using yajltcl
    if {[catch {
        set response [::yajl::json2dict $response_json]
    } err]} {
        puts "Mesh: invalid JSON response: $err"
        return
    }
    
    # Check ok field
    if {![dict exists $response ok] || ![dict get $response ok]} {
        if {[dict exists $response error]} {
            puts "Mesh registry error: [dict get $response error]"
        }
        return
    }
    
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

# Seed system/net/* immediately (don't wait for first timer tick)
catch { mesh_refresh_registry_path }

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

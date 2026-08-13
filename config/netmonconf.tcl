# netmonconf.tcl - Network path monitor (Linux / NetworkManager)
#
# Owns the system/net/* datapoints that drive the ESS Control network
# icon, and the on-demand interface/AP inspection + switching behind its
# modal. Lives in its OWN subprocess (dsconf.tcl starts it Linux-only,
# ptp-style) for two reasons:
#
#   1. Everything here shells out to Linux tooling (ip, iw, getent,
#      nmcli). Keeping that out of meshconf.tcl keeps mesh platform-free;
#      mesh just subscribes to system/net/* like any other datapoint.
#   2. Scans and switches BLOCK for seconds to a minute (nmcli -w).
#      A `send` blocks the calling interp until the target's queue
#      answers, and web-client evals run through the MAIN interp — so
#      slow work must never run inside a blocking `send`. UIs invoke the
#      slow commands with `sendNoReply netmon {...}` and collect the
#      result from a datapoint (the SyncModal pattern):
#
#        netmon/scan_result    {req, access_points:[...], error}
#        netmon/switch_result  {req, ok, error}
#
#      Blocking THIS interp during a switch is harmless — only the
#      status ticks queue behind it, and they catch up in one burst.
#
# Datapoints published (on change only, to spare browser redraws):
#   system/net/state            up | down  (down = no usable IPv4 route
#                               for 2+ consecutive ticks; wifi fields
#                               cleared, last type/iface/ip retained)
#   system/net/type             wifi | ethernet
#   system/net/iface            e.g. wlan0
#   system/net/ip               source IP on the path to the registry
#   system/net/wifi/ssid        current association ("" on ethernet)
#   system/net/wifi/bssid
#   system/net/wifi/signal_dbm
#   system/net/wifi/bars        0..4 for the status-bar icon
#
# The monitored path is the route toward the mesh registry (subscribed
# from mesh/registry, published by mesh_configure), falling back to the
# default route via 1.1.1.1. The registry hostname is resolved ONCE and
# cached — never per tick — so a dead resolver can't stall the timer;
# unresolved hosts retry on a 60 s backoff. Route lookups themselves are
# kernel-table queries and send no packets, so the icon works offline.
#
# The status path needs only `ip` (and `iw` on wifi). nmcli is required
# only for scan/switch; without NetworkManager those return an error
# payload and the passive icon keeps working.

# Guarded again here in case someone sources this by hand on the wrong box
if { $::tcl_platform(os) ne "Linux" } {
    puts "netmon: Linux-only (got $::tcl_platform(os)) — not starting"
    return
}

puts "Initializing network path monitor"

# Disable exit for subprocess
proc exit {args} { error "exit not available for this subprocess" }

load ${dspath}/modules/dserv_timer[info sharedlibextension]

tcl::tm::add $dspath/lib

package require yajltcl
# route/addr/iface/resolve/url-host queries — see lib/net-1.0.tm for the
# contract (no packets; resolve_ipv4 is real DNS and must stay gated)
package require net

errormon enable

#################################################################
# Configuration / state
#################################################################

set netmon_interval 5000

# Route target: cached registry IPv4 (never re-resolved per tick)
set netmon_target ""
set netmon_target_host ""
set netmon_next_resolve 0

# Consecutive ticks with no usable route (2+ -> state down)
set netmon_fail_count 0

# Last published value per datapoint; netmon_publish dedupes against it
set netmon_last [dict create]

proc netmon_offline {} {
    return [expr {[info exists ::env(DSERV_OFFLINE)]
                  && $::env(DSERV_OFFLINE) ne "" && $::env(DSERV_OFFLINE) ne "0"}]
}

# Publish only on change: every dservSet notifies every subscriber
# (dservTouch relies on exactly that), so republishing 7 unchanged
# values per tick would re-render every connected browser.
proc netmon_publish { key value } {
    global netmon_last
    if {[dict exists $netmon_last $key] &&
        [dict get $netmon_last $key] eq $value} {
        return
    }
    dict set netmon_last $key $value
    catch { dservSet $key $value }
}

#################################################################
# Status core (passive; no scans, no DNS)
#################################################################

# Map RSSI (dBm) to 0..4 bars for the status-bar icon.
proc netmon_wifi_bars_from_dbm { dbm } {
    if {![string is integer -strict $dbm]} { return 0 }
    if {$dbm >= -50} { return 4 }
    if {$dbm >= -60} { return 3 }
    if {$dbm >= -70} { return 2 }
    if {$dbm >= -80} { return 1 }
    return 0
}

proc netmon_clear_wifi {} {
    netmon_publish system/net/wifi/ssid ""
    netmon_publish system/net/wifi/bssid ""
    netmon_publish system/net/wifi/signal_dbm ""
    netmon_publish system/net/wifi/bars ""
}

# Current association only (iw link) — never a scan.
proc netmon_refresh_wifi { iface } {
    set out ""
    if {[catch { set out [exec iw dev $iface link] }] ||
        $out eq "" || [string match "*Not connected*" $out]} {
        netmon_clear_wifi
        return
    }
    set bssid ""
    set ssid ""
    set dbm ""
    regexp -nocase {Connected to ([0-9a-f:]+)} $out -> bssid
    # [^\n]+ — a plain .+ can span lines
    regexp {SSID:\s*([^\n]+)} $out -> ssid
    regexp {signal:\s*(-?\d+)\s*dBm} $out -> dbm
    set ssid [string trim $ssid]
    if {$ssid eq "" && $bssid eq "" && $dbm eq ""} {
        netmon_clear_wifi
        return
    }
    netmon_publish system/net/wifi/ssid $ssid
    netmon_publish system/net/wifi/bssid [string toupper $bssid]
    netmon_publish system/net/wifi/signal_dbm $dbm
    netmon_publish system/net/wifi/bars [netmon_wifi_bars_from_dbm $dbm]
}

# One status tick: route toward the registry (fallback: default route),
# publish the path, and track link-down. A route via lo (registry on
# this box) is treated like no registry route and falls back.
proc netmon_refresh {} {
    global netmon_target netmon_fail_count

    netmon_maybe_resolve

    set ip ""
    set iface ""
    if {$netmon_target ne ""} {
        lassign [::net::route_toward $netmon_target] ip iface
    }
    if {$ip eq "" || $iface eq "" || $iface eq "lo"} {
        lassign [::net::route_toward 1.1.1.1] ip iface
    }

    if {$ip eq "" || $iface eq "" || $iface eq "lo"} {
        # No usable route. Keep last type/iface/ip as context, but say
        # so explicitly — a frozen "4 bars" on a dead link is the one
        # thing a connectivity icon must never show.
        incr netmon_fail_count
        if {$netmon_fail_count >= 2} {
            netmon_publish system/net/state down
            netmon_clear_wifi
        }
        return
    }

    set netmon_fail_count 0
    set type [::net::iface_type $iface]
    netmon_publish system/net/state up
    netmon_publish system/net/ip $ip
    netmon_publish system/net/iface $iface
    netmon_publish system/net/type $type

    if {$type eq "wifi"} {
        netmon_refresh_wifi $iface
    } else {
        netmon_clear_wifi
    }
}

#################################################################
# Registry target (cached resolution; 60 s backoff while unresolved)
#################################################################

# ::net::resolve_ipv4 is real DNS and can block on a dead resolver —
# which is why it is only ever called from netmon_maybe_resolve's
# backoff gate or a registry change, never unconditionally per tick.
proc netmon_maybe_resolve {} {
    global netmon_target netmon_target_host netmon_next_resolve
    if {$netmon_target ne "" || $netmon_target_host eq ""} { return }
    if {[netmon_offline]} { return }
    set now [clock seconds]
    if {$now < $netmon_next_resolve} { return }
    set addr [::net::resolve_ipv4 $netmon_target_host]
    if {$addr ne ""} {
        set netmon_target $addr
        puts "netmon: registry $netmon_target_host -> $addr"
    } else {
        set netmon_next_resolve [expr {$now + 60}]
    }
}

proc netmon_set_target { url } {
    global netmon_target netmon_target_host netmon_next_resolve
    set netmon_target ""
    set netmon_target_host ""
    set netmon_next_resolve 0
    set host [::net::url_host $url]
    if {$host eq "" || $host eq "localhost" ||
        $host eq "127.0.0.1" || $host eq "::1"} {
        return
    }
    if {[regexp {^\d+\.\d+\.\d+\.\d+$} $host]} {
        set netmon_target $host
    } else {
        set netmon_target_host $host
        netmon_maybe_resolve
    }
}

proc netmon_registry_handler { dpoint data } {
    netmon_set_target $data
}

dservAddExactMatch mesh/registry
dpointSetScript mesh/registry netmon_registry_handler
if {[dservExists mesh/registry]} {
    netmon_set_target [dservGet mesh/registry]
}

#################################################################
# Modal: fast queries (fine inside a blocking `send netmon {...}`)
#################################################################

# Split nmcli -t fields (\: is a literal colon).
proc netmon_nmcli_split { line } {
    set tmp [string map {\\: <<COLON>>} $line]
    set parts [split $tmp :]
    set out {}
    foreach p $parts {
        lappend out [string map {<<COLON>> :} $p]
    }
    return $out
}

# Current association for one wifi iface: {ssid bssid} or empty.
proc netmon_iw_assoc { iface } {
    set out ""
    if {[catch { set out [exec iw dev $iface link] }] ||
        $out eq "" || [string match "*Not connected*" $out]} {
        return [list]
    }
    set ssid ""
    set bssid ""
    regexp -nocase {Connected to ([0-9a-f:]+)} $out -> bssid
    regexp {SSID:\s*([^\n]+)} $out -> ssid
    set ssid [string trim $ssid]
    if {$ssid eq "" && $bssid eq ""} { return [list] }
    return [list $ssid [string toupper $bssid]]
}

# Interfaces with a global IPv4 (excludes lo).
# Returns list of dicts: iface type ip ssid bssid
proc netmon_interfaces {} {
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
        set type [::net::iface_type $iface]
        set ssid ""
        set bssid ""
        if {$type eq "wifi"} {
            set assoc [netmon_iw_assoc $iface]
            if {[llength $assoc] == 2} {
                lassign $assoc ssid bssid
            }
        }
        lappend rows [dict create \
            iface $iface type $type ip $ip ssid $ssid bssid $bssid]
    }
    return $rows
}

# Current registry-path fields, straight from what we last published.
proc netmon_current_info {} {
    global netmon_last
    set cur [dict create iface "" type "" ip "" ssid "" bssid "" state ""]
    foreach {k key} {
        iface system/net/iface
        type  system/net/type
        ip    system/net/ip
        ssid  system/net/wifi/ssid
        bssid system/net/wifi/bssid
        state system/net/state
    } {
        if {[dict exists $netmon_last $key]} {
            dict set cur $k [dict get $netmon_last $key]
        }
    }
    return $cur
}

# Interfaces + current path as JSON. No scan — this stays sub-100 ms.
proc netmon_choices {} {
    set error ""
    set ifaces {}
    set cur [netmon_current_info]
    if {[catch { set ifaces [netmon_interfaces] } err]} {
        set error $err
        set ifaces {}
    }

    set cur_iface [dict get $cur iface]

    set json [yajl create #auto]
    $json map_open

    $json string current map_open
    foreach k {iface type ip ssid bssid state} {
        $json string $k string [dict get $cur $k]
    }
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

    $json string error string $error
    $json map_close

    set result [$json get]
    $json delete
    return $result
}

#################################################################
# Modal: slow operations (invoke with `sendNoReply netmon {...}`,
# collect from netmon/scan_result / netmon/switch_result)
#################################################################

# Saved wifi profiles as a dict ssid -> NM profile name. Resolved via
# the profile's 802-11-wireless.ssid property, NOT by assuming the
# profile is named after its SSID (renamed/duplicate profiles broke
# that); name is still the fallback when the property read fails.
proc netmon_wifi_profiles {} {
    set map [dict create]
    set out ""
    if {[catch { set out [exec nmcli -t -f NAME,TYPE connection show] }]} {
        return $map
    }
    foreach line [split [string trimright $out \n] \n] {
        if {$line eq ""} { continue }
        set parts [netmon_nmcli_split $line]
        if {[llength $parts] < 2} { continue }
        lassign $parts name typ
        if {$typ ne "802-11-wireless" || $name eq ""} { continue }
        set ssid ""
        catch {
            set ssid [string trim \
                [exec nmcli -g 802-11-wireless.ssid connection show id $name]]
        }
        if {$ssid eq ""} { set ssid $name }
        if {![dict exists $map $ssid]} {
            dict set map $ssid $name
        }
    }
    return $map
}

proc netmon_ap_sort { a b } {
    set c [string compare [dict get $a ssid] [dict get $b ssid]]
    if {$c != 0} { return $c }
    set qa [dict get $a signal]
    set qb [dict get $b signal]
    if {$qa > $qb} { return -1 }
    if {$qa < $qb} { return 1 }
    return 0
}

# Active scan; keep only SSIDs with saved credentials. SLOW (seconds),
# and the off-channel hops disturb live wifi traffic — which is why the
# UI only runs this on an explicit button, never on modal open.
proc netmon_scan_aps { profmap } {
    set rows {}
    if {[dict size $profmap] == 0} { return $rows }

    set out [exec nmcli -t -f IN-USE,SSID,BSSID,SIGNAL,DEVICE \
                 device wifi list --rescan yes]

    foreach line [split [string trimright $out \n] \n] {
        if {$line eq ""} { continue }
        set parts [netmon_nmcli_split $line]
        if {[llength $parts] < 5} { continue }
        lassign $parts in_use ssid bssid signal device
        if {$ssid eq "" || ![dict exists $profmap $ssid]} { continue }
        if {![string is integer -strict $signal]} { set signal 0 }
        lappend rows [dict create \
            ssid $ssid \
            bssid [string toupper $bssid] \
            signal $signal \
            device $device \
            profile [dict get $profmap $ssid] \
            in_use [expr {$in_use eq "*"}]]
    }
    return [lsort -command netmon_ap_sort $rows]
}

# Publish the scan payload to netmon/scan_result; also returned for
# CLI use (send netmon {netmon_scan 0}).
proc netmon_scan { {req 0} } {
    if {![regexp {^\d+$} $req]} { set req 0 }
    set error ""
    set aps {}
    if {[catch {
        set aps [netmon_scan_aps [netmon_wifi_profiles]]
    } err]} {
        set error $err
        set aps {}
    }

    set json [yajl create #auto]
    $json map_open
    $json string req string $req
    $json string access_points array_open
    foreach row $aps {
        $json map_open
        $json string ssid string [dict get $row ssid]
        $json string bssid string [dict get $row bssid]
        $json string signal number [dict get $row signal]
        $json string device string [dict get $row device]
        $json string profile string [dict get $row profile]
        $json string in_use bool [dict get $row in_use]
        $json map_close
    }
    $json array_close
    $json string error string $error
    $json map_close
    set payload [$json get]
    $json delete

    catch { dservSetData netmon/scan_result 0 11 $payload }
    return $payload
}

proc netmon_switch_result { req ok error } {
    set json [yajl create #auto]
    $json map_open
    $json string req string $req
    $json string ok bool $ok
    $json string error string $error
    $json map_close
    set payload [$json get]
    $json delete
    catch { dservSetData netmon/switch_result 0 11 $payload }
    return $payload
}

# Active NM connection name for a device, or "".
proc netmon_conn_for_iface { iface } {
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
proc netmon_active_links {} {
    set rows {}
    set out ""
    if {[catch { set out [exec nmcli -t -f DEVICE,TYPE,CONNECTION device status] }]} {
        return $rows
    }
    foreach line [split [string trimright $out \n] \n] {
        if {$line eq ""} { continue }
        set parts [netmon_nmcli_split $line]
        if {[llength $parts] < 3} { continue }
        lassign $parts device typ conn
        if {$typ ne "ethernet" && $typ ne "wifi"} { continue }
        if {$conn eq "" || $conn eq "--"} { continue }
        lappend rows [list $device $typ $conn]
    }
    return $rows
}

# Prefer prefer_iface (metric 50); other active eth/wifi links get 600.
# Metrics are set for v4 AND v6 so the two stacks agree on the path.
# Returns the NM connection name for prefer_iface (may be "").
proc netmon_set_metrics { prefer_iface } {
    set prefer_conn ""
    foreach row [netmon_active_links] {
        lassign $row device typ conn
        set metric 600
        if {$device eq $prefer_iface} {
            set metric 50
            set prefer_conn $conn
        }
        catch { exec nmcli connection modify id $conn \
                    ipv4.route-metric $metric ipv6.route-metric $metric }
    }
    if {$prefer_conn eq ""} {
        set prefer_conn [netmon_conn_for_iface $prefer_iface]
        if {$prefer_conn ne ""} {
            catch { exec nmcli connection modify id $prefer_conn \
                        ipv4.route-metric 50 ipv6.route-metric 50 }
        }
    }
    return $prefer_conn
}

# Re-activate non-preferred links so updated metrics hit the routing table.
proc netmon_reactivate_others { prefer_iface } {
    foreach row [netmon_active_links] {
        lassign $row device typ conn
        if {$device eq $prefer_iface} { continue }
        catch { exec nmcli -w 25 connection up id $conn ifname $device }
    }
}

# Switch registry preference to a non-wifi interface (ethernet).
proc netmon_switch_iface { iface {req 0} } {
    if {![regexp {^\d+$} $req]} { set req 0 }
    if {$iface eq ""} {
        return [netmon_switch_result $req 0 "missing interface"]
    }
    if {![regexp {^[A-Za-z0-9._-]+$} $iface]} {
        return [netmon_switch_result $req 0 "invalid interface name"]
    }
    if {[::net::iface_type $iface] eq "wifi"} {
        return [netmon_switch_result $req 0 \
                    "choose a Wi-Fi access point to switch wireless"]
    }

    if {[catch {
        set conn [netmon_set_metrics $iface]
        if {$conn eq ""} {
            catch { exec nmcli -w 30 device connect $iface }
            set conn [netmon_conn_for_iface $iface]
            if {$conn eq ""} {
                error "no NetworkManager connection for $iface"
            }
            catch { exec nmcli connection modify id $conn \
                        ipv4.route-metric 50 ipv6.route-metric 50 }
        }
        exec nmcli -w 30 connection up id $conn ifname $iface
        netmon_reactivate_others $iface
        netmon_refresh
        dservAfter 2000 netmon_refresh
    } err]} {
        return [netmon_switch_result $req 0 $err]
    }
    return [netmon_switch_result $req 1 ""]
}

# Switch to a specific AP (profile = NM connection name from the scan).
# Temporarily pins 802-11-wireless.bssid so NM associates to that AP,
# then clears the pin so reboot/roam can pick any BSSID for the SSID.
# Route-metric preference (50) stays persistent.
proc netmon_switch_ap { profile device bssid {req 0} } {
    if {![regexp {^\d+$} $req]} { set req 0 }
    if {$profile eq "" || $device eq "" || $bssid eq ""} {
        return [netmon_switch_result $req 0 "missing profile, device, or bssid"]
    }
    if {![regexp {^[A-Za-z0-9._-]+$} $device]} {
        return [netmon_switch_result $req 0 "invalid device name"]
    }
    if {![regexp -nocase {^[0-9a-f]{2}(:[0-9a-f]{2}){5}$} $bssid]} {
        return [netmon_switch_result $req 0 "invalid bssid"]
    }
    if {[regexp {[{}\[\]$;\\]} $profile]} {
        return [netmon_switch_result $req 0 "invalid profile name"]
    }
    if {[::net::iface_type $device] ne "wifi"} {
        return [netmon_switch_result $req 0 "$device is not a Wi-Fi interface"]
    }

    if {[catch {
        netmon_set_metrics $device
        exec nmcli connection modify id $profile \
            802-11-wireless.bssid $bssid \
            ipv4.route-metric 50 ipv6.route-metric 50
        exec nmcli -w 45 connection up id $profile ifname $device
        # Drop the pin after associate — never insist on this AP across
        # reboot/roam.
        catch { exec nmcli connection modify id $profile 802-11-wireless.bssid "" }
        netmon_reactivate_others $device
        netmon_refresh
        dservAfter 2000 netmon_refresh
    } err]} {
        # Best-effort: never leave a sticky BSSID pin behind on failure.
        catch { exec nmcli connection modify id $profile 802-11-wireless.bssid "" }
        return [netmon_switch_result $req 0 $err]
    }
    return [netmon_switch_result $req 1 ""]
}

#################################################################
# Timer + startup
#################################################################

proc netmon_timer_callback { dpoint data } {
    netmon_refresh
}

proc netmon_start { {interval_ms 5000} } {
    global netmon_interval
    set netmon_interval $interval_ms
    timerTickInterval $interval_ms $interval_ms
    puts "netmon started: ${interval_ms}ms interval"
}

proc netmon_stop {} {
    timerStop
    puts "netmon stopped"
}

proc netmon_set_interval { interval_ms } {
    global netmon_interval
    set netmon_interval $interval_ms
    timerTickInterval $interval_ms $interval_ms
}

proc netmon_info {} {
    global netmon_interval netmon_target netmon_target_host netmon_fail_count
    return [dict create \
        interval $netmon_interval \
        target $netmon_target \
        target_host $netmon_target_host \
        fail_count $netmon_fail_count \
        current [netmon_current_info] \
    ]
}

proc netmon_setup {} {
    timerPrefix netmonTimer
    dservAddExactMatch netmonTimer/0
    dpointSetScript netmonTimer/0 netmon_timer_callback
}

netmon_setup

# Seed system/net/* immediately (don't wait for the first tick)
catch { netmon_refresh }

netmon_start $netmon_interval

puts "Network path monitor ready"
puts "Commands available:"
puts "  netmon_choices                    - interfaces + current path (JSON)"
puts "  netmon_scan ?req?                 - AP scan -> netmon/scan_result (slow)"
puts "  netmon_switch_iface iface ?req?   - prefer iface -> netmon/switch_result"
puts "  netmon_switch_ap profile device bssid ?req?"
puts "  netmon_info                       - monitor state"
puts "  netmon_set_interval ms            - change status interval"

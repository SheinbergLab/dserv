# Canned-exec harness for netmonconf.tcl — runs the REAL config inside a
# throwaway subprocess of a LIVE dserv (any platform: it fakes Linux and
# intercepts exec/file so every ip/iw/nmcli/getent call gets realistic
# fixture output). Exercises the full logic: status refresh, registry
# resolution + backoff, choices/scan JSON, AP + iface switching, input
# validation, link-down / recovery. Nothing real is executed or changed.
#
# Run against a live dserv from the repo checkout:
#
#   dservctl -c "subprocess nmtest {source $PWD/config/test/netmon_canned_test.tcl}"
#   dservctl get netmontest/report
#
# (Each run needs a fresh subprocess name; there is no subprocess kill.)
# The report is also this script's return value.

package require yajltcl

# Repo root, derived from this file's location (config/test/..)
set _nmtest_root [file normalize [file join [file dirname [info script]] .. ..]]

# Pre-load net-1.0.tm from the repo before the exec/file intercepts go in
# (the tm loader itself uses file/glob); its procs still exec through the
# intercepts at call time, which is the point.
tcl::tm::add [file join $_nmtest_root lib]
package require net

set ::results {}
proc t {name ok {detail ""}} {
    lappend ::results [list [expr {$ok ? "PASS" : "FAIL"}] $name $detail]
}

# --- pretend to be Linux for the platform gate ---
set ::tcl_platform(os) Linux

# --- intercept file: /sys wireless probes (wl* = wifi) ---
rename file _file
proc file {cmd args} {
    if {$cmd eq "isdirectory"} {
        set p [lindex $args 0]
        if {[string match "/sys/class/net/*/wireless" $p]} {
            return [string match "/sys/class/net/wl*" $p]
        }
    }
    tailcall _file $cmd {*}$args
}

# --- intercept exec: canned Linux tool output ---
rename exec _exec
set ::exec_log {}
set ::route_fail 0
proc exec {args} {
    lappend ::exec_log $args
    set a [join $args " "]
    if {$::route_fail && [string match "ip -4 route get*" $a]} {
        error "canned: Network is unreachable"
    }
    switch -glob -- $a {
        "ip -4 route get 5.6.7.8" {
            return "5.6.7.8 via 192.168.1.1 dev eth0 src 192.168.1.40 uid 0\n    cache"
        }
        "ip -4 route get *" {
            return "1.1.1.1 via 192.168.1.1 dev wlan0 src 192.168.1.50 uid 0\n    cache"
        }
        "getent ahostsv4 dserv.net" {
            return "5.6.7.8       STREAM dserv.net\n5.6.7.8       DGRAM  dserv.net\n5.6.7.8       RAW    dserv.net"
        }
        "iw dev wlan0 link" {
            return "Connected to aa:bb:cc:11:22:33 (on wlan0)\n\tSSID: LabNet\n\tfreq: 5180\n\tsignal: -58 dBm\n\ttx bitrate: 866.7 MBit/s"
        }
        "ip -4 -o addr show scope global" {
            return "2: eth0    inet 192.168.1.40/24 brd 192.168.1.255 scope global eth0\\       valid_lft forever preferred_lft forever\n3: wlan0    inet 192.168.1.50/24 brd 192.168.1.255 scope global dynamic noprefixroute wlan0\\       valid_lft 85000sec preferred_lft 85000sec"
        }
        "nmcli -t -f NAME,TYPE connection show" {
            return "LabNet:802-11-wireless\nSecond Profile:802-11-wireless\nWired connection 1:802-3-ethernet"
        }
        "nmcli -g 802-11-wireless.ssid connection show id LabNet" {
            return "LabNet"
        }
        "nmcli -g 802-11-wireless.ssid connection show id Second Profile" {
            return "Second Net"
        }
        "nmcli -t -f IN-USE,SSID,BSSID,SIGNAL,DEVICE device wifi list --rescan yes" {
            return "*:LabNet:AA\\:BB\\:CC\\:11\\:22\\:33:82:wlan0\n:LabNet:AA\\:BB\\:CC\\:44\\:55\\:66:55:wlan0\n:Second Net:DD\\:EE\\:FF\\:00\\:11\\:22:40:wlan0\n:StrangerNet:11\\:22\\:33\\:44\\:55\\:66:90:wlan0"
        }
        "nmcli -t -f DEVICE,TYPE,CONNECTION device status" {
            return "eth0:ethernet:Wired connection 1\nwlan0:wifi:LabNet\nlo:loopback:lo"
        }
        "nmcli -t -f GENERAL.CONNECTION device show eth0" {
            return "GENERAL.CONNECTION:Wired connection 1"
        }
        "nmcli connection modify *" { return "" }
        "nmcli -w * connection up *" { return "" }
        "nmcli -w * device connect *" { return "" }
        default {
            error "no canned output for: $a"
        }
    }
}

# --- run the real config ---
source [file join $_nmtest_root config netmonconf.tcl]
netmon_stop   ;# deterministic: no timer ticks during asserts

# Hermetic baseline: the LIVE dserv may carry a real mesh/registry
# datapoint (new meshconf publishes it), which netmonconf correctly picks
# up at source time. Reset and refresh via the default route so section 1
# asserts a known state regardless of the host's mesh config.
netmon_set_target ""
netmon_refresh

# ---------- 1. seed refresh (default route -> wlan0 wifi) ----------
t "seed state=up"      [expr {[dservGet system/net/state] eq "up"}] [dservGet system/net/state]
t "seed type=wifi"     [expr {[dservGet system/net/type] eq "wifi"}] [dservGet system/net/type]
t "seed iface=wlan0"   [expr {[dservGet system/net/iface] eq "wlan0"}] [dservGet system/net/iface]
t "seed ip"            [expr {[dservGet system/net/ip] eq "192.168.1.50"}] [dservGet system/net/ip]
t "seed ssid"          [expr {[dservGet system/net/wifi/ssid] eq "LabNet"}] [dservGet system/net/wifi/ssid]
t "seed bssid upper"   [expr {[dservGet system/net/wifi/bssid] eq "AA:BB:CC:11:22:33"}] [dservGet system/net/wifi/bssid]
t "seed dbm"           [expr {[dservGet system/net/wifi/signal_dbm] eq "-58"}] [dservGet system/net/wifi/signal_dbm]
t "seed bars=3"        [expr {[dservGet system/net/wifi/bars] eq "3"}] [dservGet system/net/wifi/bars]

# ---------- 2. registry target: resolve once, route toward it ----------
netmon_set_target "https://dserv.net:443"
t "target resolved"    [expr {$::netmon_target eq "5.6.7.8"}] $::netmon_target
set n_getent_before [llength [lsearch -all -glob $::exec_log "getent*"]]
netmon_refresh
netmon_refresh
set n_getent_after [llength [lsearch -all -glob $::exec_log "getent*"]]
t "no DNS in hot loop" [expr {$n_getent_after == $n_getent_before}] "getent calls: $n_getent_before -> $n_getent_after"
t "path follows registry (eth0)" [expr {[dservGet system/net/iface] eq "eth0"}] [dservGet system/net/iface]
t "type=ethernet"      [expr {[dservGet system/net/type] eq "ethernet"}] [dservGet system/net/type]
t "wifi cleared"       [expr {[dservGet system/net/wifi/ssid] eq "" && [dservGet system/net/wifi/bars] eq ""}]

# ---------- 2b. resolver backoff (unresolvable host) ----------
set n_getent_b [llength [lsearch -all -glob $::exec_log "getent*"]]
netmon_set_target "https://no.such.host"
t "unresolved: no target" [expr {$::netmon_target eq ""}] $::netmon_target
t "backoff doubled"       [expr {$::netmon_resolve_wait == 120}] $::netmon_resolve_wait
netmon_maybe_resolve
set n_getent_a [llength [lsearch -all -glob $::exec_log "getent*"]]
t "backoff gates retry"   [expr {$n_getent_a == $n_getent_b + 1}] "getent calls: $n_getent_b -> $n_getent_a"
# restore the resolvable registry so later sections keep the eth0 path
netmon_set_target "https://dserv.net:443"
t "backoff reset on retarget" [expr {$::netmon_resolve_wait == 60 && $::netmon_target eq "5.6.7.8"}] "wait=$::netmon_resolve_wait target=$::netmon_target"

# ---------- 3. choices JSON ----------
set choices [::yajl::json2dict [netmon_choices]]
t "choices error empty" [expr {[dict get $choices error] eq ""}] [dict get $choices error]
set ifs [dict get $choices interfaces]
t "choices 2 interfaces" [expr {[llength $ifs] == 2}] [llength $ifs]
set by_iface [dict create]
foreach row $ifs { dict set by_iface [dict get $row iface] $row }
t "eth0 current"       [expr {[dict get $by_iface eth0 current]}]
t "wlan0 has assoc"    [expr {[dict get $by_iface wlan0 ssid] eq "LabNet"}] [dict get $by_iface wlan0 ssid]
t "current state up"   [expr {[dict get $choices current state] eq "up"}]

# ---------- 4. scan -> netmon/scan_result ----------
netmon_scan 42
set scan [::yajl::json2dict [dservGet netmon/scan_result]]
t "scan req"           [expr {[dict get $scan req] eq "42"}] [dict get $scan req]
t "scan error empty"   [expr {[dict get $scan error] eq ""}] [dict get $scan error]
set aps [dict get $scan access_points]
t "scan 3 rows (stranger filtered)" [expr {[llength $aps] == 3}] [llength $aps]
t "scan sorted"        [expr {[dict get [lindex $aps 0] signal] == 82 && [dict get [lindex $aps 1] signal] == 55}]
t "scan in_use"        [expr {[dict get [lindex $aps 0] in_use]}]
t "scan bssid unescaped" [expr {[dict get [lindex $aps 0] bssid] eq "AA:BB:CC:11:22:33"}] [dict get [lindex $aps 0] bssid]
set second [lindex $aps 2]
t "profile by ssid property" [expr {[dict get $second ssid] eq "Second Net" && [dict get $second profile] eq "Second Profile"}] "[dict get $second ssid] -> [dict get $second profile]"

# ---------- 5. switch_iface ----------
set r [::yajl::json2dict [netmon_switch_iface eth0 43]]
t "switch_iface ok"    [expr {[dict get $r ok]}] [dict get $r error]
set v6 0
foreach e $::exec_log {
    if {[lsearch -exact $e ipv6.route-metric] >= 0} { set v6 1; break }
}
t "metrics set v4+v6"  $v6
set r2 [::yajl::json2dict [dservGet netmon/switch_result]]
t "switch result dpoint req" [expr {[dict get $r2 req] eq "43"}]

# ---------- 6. switch_ap (spacey profile, pin then clear) ----------
set r [::yajl::json2dict [netmon_switch_ap "Second Profile" wlan0 DD:EE:FF:00:11:22 44]]
t "switch_ap ok"       [expr {[dict get $r ok]}] [dict get $r error]
set pinned 0; set cleared 0
foreach e $::exec_log {
    if {[lrange $e 0 4] eq [list nmcli connection modify id "Second Profile"]} {
        set i [lsearch -exact $e 802-11-wireless.bssid]
        if {$i >= 0} {
            set v [lindex $e [expr {$i+1}]]
            if {$v eq "DD:EE:FF:00:11:22"} { set pinned 1 }
            if {$v eq ""} { set cleared 1 }
        }
    }
}
t "bssid pinned"       $pinned
t "bssid pin cleared"  $cleared

# ---------- 7. validation rejects ----------
set r [::yajl::json2dict [netmon_switch_ap "Second Profile" wlan0 "not-a-mac" 45]]
t "bad bssid rejected" [expr {![dict get $r ok] && [dict get $r error] eq "invalid bssid"}] [dict get $r error]
set r [::yajl::json2dict [netmon_switch_iface "eth0; rm -rf /" 46]]
t "bad iface rejected" [expr {![dict get $r ok]}] [dict get $r error]
set r [::yajl::json2dict [netmon_switch_iface wlan0 47]]
t "wifi via switch_iface rejected" [expr {![dict get $r ok]}] [dict get $r error]

# ---------- 8. link down -> explicit state, then recovery ----------
set ::route_fail 1
netmon_refresh
t "one failure: still up" [expr {[dservGet system/net/state] eq "up"}]
netmon_refresh
t "two failures: down" [expr {[dservGet system/net/state] eq "down"}] [dservGet system/net/state]
t "down keeps last iface" [expr {[dservGet system/net/iface] eq "eth0"}]
set ::route_fail 0
netmon_refresh
t "recovery: up"       [expr {[dservGet system/net/state] eq "up"}]

# ---------- report ----------
set npass 0; set nfail 0
set lines {}
foreach r $::results {
    lassign $r st name detail
    if {$st eq "PASS"} { incr npass } else { incr nfail }
    lappend lines [format "%-4s %s%s" $st $name [expr {$detail ne "" ? "  ($detail)" : ""}]]
}
set report "netmon canned tests: $npass passed, $nfail failed\n[join $lines \n]"
dservSet netmontest/report $report
set report

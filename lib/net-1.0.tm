# net-1.0.tm — read-only host address / route / identity queries
#
# The one real primitive is route_toward: ask the KERNEL which interface
# and source address it would use toward a destination. Per-destination
# routing is the point — on a multi-homed box the answer for a peer on a
# directly connected subnet (a stim computer on the wired rig LAN) does
# not change when the DEFAULT route moves (netmon flipping mesh-primary
# to wifi). Every address handed to a peer should be computed toward
# THAT peer; local_addr (the default-route address) is for identifying
# this box in general, not for telling a specific peer where to reach us.
#
# Contract, which every caller relies on:
#
#   - No packets are sent and no DNS is done — kernel-table lookups and
#     sysfs reads only — EXCEPT resolve_ipv4, which is real DNS and can
#     block for seconds on a dead resolver. Gate it: resolve once and
#     cache, retry with backoff; never call it on a hot path (netmonconf's
#     netmon_maybe_resolve is the reference caller).
#   - Everything is catch-wrapped and failure is an ANSWER ("" or empty
#     fields), never an error. These run on boot paths (dsconf.tcl runs
#     as one Tcl_Eval, where an uncaught error silently truncates the
#     rest of the config), and exec itself forks a large, threaded,
#     mlockall()ed process under RT scheduling — a missing tool or a
#     failed fork must degrade, not abort.
#   - Platform coverage is per-proc: route/addr queries work on Linux
#     and Darwin; iface_type and resolve_ipv4 are Linux-only today and
#     return "" elsewhere.

package provide net 1.0

namespace eval ::net {
    namespace export route_toward addr_toward local_addr \
        iface_type resolve_ipv4 url_host

    # Kernel route toward an IPv4 literal: returns {ip iface}, with both
    # fields "" on any failure (non-IPv4 dest included).
    proc route_toward { dest } {
        if { ![regexp {^\d+\.\d+\.\d+\.\d+$} $dest] } { return [list "" ""] }
        set ip ""
        set iface ""
        if { $::tcl_platform(os) eq "Linux" } {
            catch {
                set out [exec ip -4 route get $dest]
                regexp {src (\S+)} $out -> ip
                regexp {dev (\S+)} $out -> iface
            }
        } elseif { $::tcl_platform(os) eq "Darwin" } {
            catch {
                set iface [exec sh -c "route -n get $dest 2>/dev/null | awk '/interface:/{print \$2; exit}'"]
                if { $iface ne "" } {
                    set ip [string trim [exec ipconfig getifaddr $iface]]
                }
            }
        }
        if { $ip eq "" || $iface eq "" } { return [list "" ""] }
        return [list $ip $iface]
    }

    # Source address of the route toward $dest, or "".
    proc addr_toward { dest } {
        return [lindex [route_toward $dest] 0]
    }

    # This box's address on the default route ("" if none). 1.1.1.1 is a
    # route-table probe only — nothing is sent to it.
    proc local_addr {} {
        return [addr_toward 1.1.1.1]
    }

    # Classify a netdev: "wifi" | "ethernet" ("" for empty input or on
    # platforms without sysfs).
    proc iface_type { iface } {
        if { $iface eq "" || $::tcl_platform(os) ne "Linux" } { return "" }
        if { [file isdirectory "/sys/class/net/$iface/wireless"] } {
            return wifi
        }
        return ethernet
    }

    # Hostname -> IPv4 via getent. REAL DNS — see the gating note in the
    # header. IPv4 literals pass through; "" on failure or off-Linux.
    proc resolve_ipv4 { host } {
        if { $host eq "" } { return "" }
        if { [regexp {^\d+\.\d+\.\d+\.\d+$} $host] } { return $host }
        if { $::tcl_platform(os) ne "Linux" } { return "" }
        set out ""
        if { [catch { set out [exec getent ahostsv4 $host] }] } {
            return ""
        }
        set line [lindex [split [string trim $out] \n] 0]
        set addr [lindex $line 0]
        if { [regexp {^\d+\.\d+\.\d+\.\d+$} $addr] } { return $addr }
        return ""
    }

    # Host part of an http(s) URL: https://dserv.net:443/x -> dserv.net,
    # bracketed IPv6 unwrapped. "" if it doesn't parse.
    proc url_host { url } {
        if { $url eq "" } { return "" }
        if { [regexp {^https?://\[([^\]]+)\]} $url -> host] } {
            return $host
        }
        if { [regexp {^https?://([^/:]+)} $url -> host] } {
            return $host
        }
        return ""
    }
}

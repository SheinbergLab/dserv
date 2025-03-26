set dlshlib [file join /usr/local dlsh dlsh.zip]
set base [file join [zipfs root] dlsh]
zipfs mount $dlshlib $base
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

package require dlsh
package require qpcs

proc send_grasp { ds ms } {
    dl_local coords [dl_short [dl_mult [dl_ones 6] [incr ::count]]]
    qpcs::dsSocketSetData $ds grasp/sensor0/vals $coords
    after $ms "send_grasp $ds $ms"
}

set count 0
set ms 5 
set server localhost
set ds [qpcs::dsSocketOpen $::server]
send_grasp $ds $ms

vwait forever


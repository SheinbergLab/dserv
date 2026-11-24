# test_ess_photo.tcl -- the gear's `photo` section reaches the processor.
#
# config/essconf.tcl declares the section; lib/ess_photo-1.0.tm (BEGIN photo)
# pushes each key to dserv's `photo` processor (processors/photo.c) and
# logs proc/photo/edge in each data file while the rig has it on. The
# processor keeps nothing across a restart, so these pushes are the only
# way a rig's settings reach it.
#
#   tclsh tests/test_ess_photo.tcl
#
# The block and the declarations are read out of the two files (the whole
# module needs a dserv); processSetParam, the logger and the settings
# store are stand-ins.

set here [file dirname [file normalize [info script]]]
set root [file dirname $here]

set fails 0
proc check { got want what } {
    if { $got eq $want } { puts "  ok: $what" } else {
        puts "  FAIL: $what: got {$got} want {$want}"
        incr ::fails
    }
}
proc slurp { f } { set fd [open $f] ; set s [read $fd] ; close $fd ; return $s }

# the processor: records each param, answers like dserv (0, or -1 when
# there is no such processor)
set ::sent {}
set ::psp 0
proc processSetParam { name p v } {
    lappend ::sent [list $name $p $v]
    return $::psp
}
set ::matches {}
proc dservLoggerAddMatch { args } { lappend ::matches $args }
# a dserv clock 37 s off the wall clock, as on a PTP site
set ::true_off 37000123
proc now {} { expr {[clock microseconds] + $::true_off} }
set ::dp [dict create]
proc dservSet { name v } { dict set ::dp $name $v }
# the datapoint table photo_health reads: name -> {timestamp value}
set ::table [dict create]
proc dservKeys { pat } {
    lmap k [dict keys $::table] { if { ![string match $pat $k] } continue ; set k }
}
proc dservTimestamp { k } { lindex [dict get $::table $k] 0 }
proc dservGet { k } { lindex [dict get $::table $k] 1 }
# the processor: no answer (no processor) unless ::pgp says
set ::pgp [dict create]
proc processGetParam { name p } {
    expr {[dict exists $::pgp $p] ? [dict get $::pgp $p] : ""}
}
namespace eval ::ess {
    variable warned {}
    proc ess_warning { msg cat } { variable warned ; lappend warned $msg }
}

# ---- the ess block ----
# the photo blocks (ess_photo-1.0.tm), and ess-2.0.tm for din_resolve
set src "[slurp [file join $root lib ess_photo-1.0.tm]][slurp [file join $root lib ess-2.0.tm]]"
set b [string first "    # BEGIN photo\n" $src]
set e [string first "    # END photo\n" $src]
if { $b < 0 || $e < 0 } { error "no photo block in ess_photo-1.0.tm" }
namespace eval ::ess [string range $src $b $e]
# ... and the pairing it starts (test_ess_photo_verify.tcl tests it)
set b [string first "    # BEGIN photo_verify\n" $src]
set e [string first "    # END photo_verify\n" $src]
namespace eval ::ess [string range $src $b $e]
proc dservSetData { args } {}
# ... and the line resolver it shares with tlock (beside tlock's code)
set b [string first "    proc din_resolve \{ v \} \{\n" $src]
set e [string first "\n    \}\n" $src $b]
if { $b < 0 } { error "no din_resolve in ess-2.0.tm" }
namespace eval ::ess [string range $src $b $e+6]
proc dservAddExactMatch { name } { lappend ::subs $name }
proc dpointSetScript { name script } { lappend ::subs $script }
set ::ess::stim_cmd_hook ""

puts "pushes:"
set ::sent {}
::ess::set_photo_group " box01/photodiode "
::ess::set_photo_strobe yes
::ess::set_photo_invert on
::ess::set_photo_period_ms 8.3333
::ess::set_photo_smooth_ms 2
::ess::set_photo_min_step 150
::ess::set_photo_settle_ms 10
check $::sent {{photo group box01/photodiode} {photo strobe 1} {photo invert 1} {photo period 0.0083333} {photo smooth 0.002} {photo min_step 150} {photo settle_dur 0.01}} \
    "each key, ms as seconds, group trimmed"

puts "validation:"
check [catch { ::ess::photo_group_norm "a b" }] 1 "a group with a space is refused"
check [catch { ::ess::photo_group_norm "a/b/c" }] 1 "two slashes refused"
check [catch { ::ess::photo_group_norm "extio/*" }] 1 "a pattern refused"
check [::ess::photo_group_norm ""] "" "empty allowed (every block)"
check [catch { ::ess::photo_nonneg_norm -1 }] 1 "a negative period refused"
check [catch { ::ess::photo_nonneg_norm abc }] 1 "a word refused"

puts "enabled:"
set ::sent {}
::ess::set_photo_enabled 1
check $::sent {{photo active 1}} "active 1"
check $::ess::warned {} "no warning where the processor exists"
set ::psp -1
::ess::set_photo_enabled 1
::ess::set_photo_enabled 1
check [llength $::ess::warned] 1 "no processor: warned once"
::ess::set_photo_enabled 0
check [llength $::ess::warned] 1 "turning it off does not warn"
set ::psp 0

puts "data file:"
set ::matches {}
::ess::set_photo_enabled 0
check [::ess::photo_log_match f.ess] 0 "off: not logged"
::ess::set_photo_enabled 1
check [::ess::photo_log_match f.ess] 1 "on: logged"
check $::matches {{f.ess proc/photo/edge 0 0 1} {f.ess ess/dserv_wall_us 0 0 1} {f.ess photo/pair 0 0 1} {f.ess photo/verify 0 0 1}} \
    "every edge, not obs-limited, unbuffered; the clock offset; the pairs and verdict"
check [dict exists $::dp ess/dserv_wall_us] 1 "  the offset set after its match"
check [expr {abs([dict get $::dp ess/dserv_wall_us] - $::true_off) < 2000}] 1 \
    "  dserv minus wall, to the bracket ([dict get $::dp ess/dserv_wall_us] vs $::true_off)"

puts "stim2 patch:"
# rmtSend: the script is run in a stand-in stim2, with or without the module
set ::rmt {}
set ::stim [interp create]
proc rmtSend { script } { lappend ::rmt $script ; $::stim eval $script }
$::stim eval {
    set calls {}
    proc photopatch { args } { lappend ::calls $args ; expr {[lindex $args 0] eq "on" ? 1 : {}} }
}
set ::ess::warned {}
::ess::set_photo_enabled 0
::ess::set_photo_patch 1
check [::ess::stim_photopatch_enable] 0 "photo off: the patch is not drawn"
check $::rmt {} "  nothing sent"
::ess::set_photo_enabled 1
::ess::set_photo_patch 0
check [::ess::stim_photopatch_enable] 0 "patch off: not drawn"
::ess::set_photo_patch 1
::ess::set_photo_patch_mode toggle
::ess::set_photo_patch_corner bl
::ess::set_photo_patch_size " 40 "
::ess::set_photo_patch_inset 5
::ess::set_photo_patch_dark {0.1 0.1 0.2}
check [::ess::stim_photopatch_enable] 1 "both on: drawn"
check [$::stim eval { set calls }] \
    {{set -mode toggle -corner bl -size 40 -inset 5 -dark {0.1 0.1 0.2}} on} \
    "  its settings, then on"
check [llength $::rmt] 1 "  one rmtSend, so the data file records it"
check $::ess::warned {} "  no warning"
check [list $::ess::pv_on $::ess::pv_mode $::ess::stim_cmd_hook] {1 toggle ::ess::pv_cmd} \
    "  its changes paired with edges, in its mode, '!' commands heard"
check $::subs {stim/photopatch ::ess::pv_change_dp proc/photo/edge ::ess::pv_edge_dp} \
    "  stim/photopatch and proc/photo/edge subscribed"
$::stim eval { rename photopatch {} }
check [::ess::stim_photopatch_enable] -1 "a stim2 without the module: -1"
check [list $::ess::pv_on $::ess::stim_cmd_hook] {0 {}} "  pairing stopped"
check [llength $::ess::warned] 1 "  and a warning"
interp delete $::stim
check [catch { ::ess::photo_patch_size_norm 0 }] 1 "size 0 refused"
check [catch { ::ess::photo_patch_inset_norm -1 }] 1 "inset -1 refused"
check [::ess::photo_patch_inset_norm " 20 60 "] "20 60" "inset x y"
check [::ess::photo_patch_inset_norm "8 8"] 8 "  one number when they agree (an older stim2 takes it)"
check [catch { ::ess::photo_patch_inset_norm "1 2 3" }] 1 "  three refused"
check [catch { ::ess::photo_patch_inset_norm "5 -1" }] 1 "  a negative y refused"
check [catch { ::ess::photo_patch_dark_norm "0 1" }] 1 "two values for dark refused"
check [catch { ::ess::photo_patch_dark_norm 1.5 }] 1 "dark past 1 refused"
check [::ess::photo_patch_dark_norm " 0.1 "] 0.1 "one grey"
set ::ess::photo_enabled 0
set ::ess::photo_patch 0

puts "the green light (photo/health):"
proc light {} { lindex [dict get $::dp photo/health] 0 }
proc why {} { lrange [dict get $::dp photo/health] 1 end }
set ::stim [interp create]
$::stim eval { proc photopatch { args } { expr {[lindex $args 0] eq "on" ? 1 : {}} } }
set ::ess::photo_patch_stim 0
::ess::set_photo_patch 0
::ess::set_photo_enabled 0
check [light] off "not enabled: off"
::ess::set_photo_enabled 1
check [light] error "enabled, no processor: error"
check [string match "*no photo processor*" [why]] 1 "  says so"
dict set ::pgp n_edges 0
dict set ::pgp group photodiode
::ess::photo_health
check [string match "*no block from group photodiode: is the diode's group streaming*" [why]] 1 \
    "no analog stream at all: error, streaming? ([why])"
set T [now]
dict set ::table extio/box01/state/ain/eye [list $T {}]
::ess::photo_health
check [string match "*other analog groups stream*group name*" [why]] 1 \
    "only other groups: the name is wrong ([why])"
dict set ::table extio/box02/state/ain/photodiode [list [expr {$T - 6000000}] {}]
::ess::photo_health
check [list [light] [string match "*6 s old*stopped*" [why]]] {error 1} "a stale stream: stopped ([why])"
dict set ::table extio/box02/state/ain/photodiode [list $T {}]
dict set ::pgp n_edges 12
::ess::photo_health
check [dict get $::dp photo/health] "ok reading group photodiode, 12 edges" "streaming: ok"
dict set ::pgp group box01/photodiode
::ess::photo_health
check [string match "*no block from group box01/photodiode*" [why]] 1 "box/label: only that box ([why])"
dict set ::pgp group photodiode

# the patch
::ess::set_photo_patch 1
check [string match "ok*not changed yet" [dict get $::dp photo/health]] 1 "patch on, no change yet: ok"
$::stim eval { rename photopatch {} }
::ess::stim_photopatch_enable
check [list [light] [string match "*cannot draw the patch*" [why]]] {error 1} "stim2 cannot draw it"
$::stim eval { proc photopatch { args } { expr {[lindex $args 0] eq "on" ? 1 : {}} } }
::ess::stim_photopatch_enable
check [light] ok "stim2 draws it: ok again"
# swapcount wall level serial ntags why
dict set ::table stim/photopatch [list $T {10 0 0 1 0 1}]
::ess::photo_health
check [light] ok "the patch appearing, no edge: not yet a change"
dict set ::table stim/photopatch [list $T {11 0 1 2 1 2}]
::ess::photo_health
check [list [light] [string match "*no edge ever came back*" [why]]] {error 1} "changed, no edge ever: error"
dict set ::table proc/photo/edge [list [expr {$T + 10000}] {}]
::ess::photo_health
check [string match "ok*answered" [dict get $::dp photo/health]] 1 "an edge after the change: ok"
dict set ::table stim/photopatch [list [expr {$T + 3000000}] {12 0 0 3 1 2}]
::ess::photo_health
check [list [light] [string match "*3.0 s older than the last change*" [why]]] {error 1} \
    "changes go on, edges stopped: error ([why])"
dict set ::table stim/photopatch [list [expr {$T + 1000000}] {12 0 0 3 1 2}]
::ess::photo_health
check [light] ok "  within 2 s: ok"
set n [llength [lsearch -all [dict keys $::dp] photo/health]]
::ess::photo_health
check [::ess::photo_health] [dict get $::dp photo/health] "returns what it publishes"
interp delete $::stim
set ::ess::photo_enabled 0
set ::ess::photo_patch 0
set ::ess::photo_patch_stim 0

puts "the light line (photo din):"
set ::removed {}
proc dservRemoveMatch { name } { lappend ::removed $name }
set ::subs {}
set PDI extio/photo-sync/state/di/0
::ess::set_photo_din " photopatch "
check $::subs {} "photo off: nothing subscribed"
::ess::set_photo_enabled 1
check [dict get [::ess::photo_din_resolve] status] unresolved "no box announces it: unresolved"
check [list [light] [string match "*no box announces*photopatch*" [why]]] {error 1} \
    "  the light says so ([why])"
dict set ::table extio/photo-sync/state/label/0 [list $T photopatch]
check [::ess::photo_din_attach] $PDI "a box labels a line so: resolved"
check $::subs [list $PDI ::ess::photo_din_edge] "  the line subscribed"
::ess::photo_din_attach
check [llength $::subs] 2 "  once"
dict set ::table extio/box01/state/group/lever/pins [list $T 1,2]
::ess::set_photo_din lever
check [list [dict get [::ess::photo_din_resolve] status] $::removed] [list unresolved $PDI] \
    "a chord group refused, the old line let go"
::ess::set_photo_din photopatch
set ::ess::warned {}
set ::psp -1
set ::ess::photo_warned 0
::ess::set_photo_enabled 1
check $::ess::warned {} "no processor, but a line: no warning"
set ::psp 0

# its edges pair with the patch's changes; the processor's are ignored
::ess::pv_start marked 0
set T0 [expr {$T + 20000000}]
::ess::pv_change [list 50 $T0 1 7 1 2 0] $T0
dict set ::table $PDI [list [expr {$T0 + 13100}] 1]
::ess::photo_din_edge $PDI 1
::ess::pv_edge_dp proc/photo/edge [list 1 [expr {$T0 + 12000}] 0 0 0 0 1 9 0 0]
check [llength $::ess::pv_edges] 1 "one edge: the line's, not the processor's"
check [lrange [lindex $::ess::pv_edges 0] 0 2] [list [expr {$T0 + 13100}] 1 [expr {$T0 + 13100}]] \
    "  at the box's stamp, brighter"
::ess::pv_resolve [expr {$T0 + 400000}]
check [list [dict get $::ess::pv_n changes] [dict get $::ess::pv_n lit]] {1 1} "  the change lit"
check [::ess::photo_din_row 0 5] {-1 5 5 5 1 0 1 2 5 5} "a 0 is darker, a row of ten"
::ess::pv_stop

# the light, from the line
set ::ess::photo_patch 1
set ::ess::photo_patch_stim 1
dict set ::table stim/photopatch [list [expr {$T0 + 30000}] {60 0 0 8 1 2}]
dict set ::table $PDI [list [expr {$T0 + 42000}] 0]
::ess::photo_health
check [string match "ok reading line photopatch; pass: 1 of 1*" [dict get $::dp photo/health]] 1 \
    "edges answer the patch: ok ([why])"
dict set ::table stim/photopatch [list [expr {$T0 + 3000000}] {61 0 1 9 1 2}]
::ess::photo_health
check [list [light] [string match "*3.0 s older*detector over the patch*" [why]]] {error 1} \
    "the line stopped answering: error ([why])"

# the data file
set ::matches {}
::ess::photo_log_match g.ess
check [lrange $::matches 4 end] [list {g.ess photo/din 0 0 1} [list g.ess $PDI 0 0 1] \
    {g.ess extio/photo-sync/state/sync/* 0 0 1} {g.ess extio/photo-sync/state/in_obs 0 0 1}] \
    "the resolution, the line's edges, the box clock's records"
check [dict get $::dp photo/din dp] $PDI "  photo/din says which datapoint"
::ess::set_photo_din ""
check [lindex $::removed end] $PDI "cleared: the line let go"
set ::ess::photo_enabled 0
set ::ess::photo_patch 0
set ::ess::photo_patch_stim 0

# ---- the declarations ----
puts "gear section (essconf.tcl):"
set conf [slurp [file join $root config essconf.tcl]]
set b [string first "settings::declare photo enabled" $conf]
set e [string first "::ess::set_photo_enabled    \[settings::get photo enabled\]" $conf]
if { $b < 0 || $e < 0 } { error "no photo section in essconf.tcl" }
set e [string first "\n" $conf $e]

namespace eval ::settings {
    variable decl [dict create]
    proc declare { sub key args } {
        variable decl
        dict set decl $sub/$key $args
    }
    proc get { sub key } {
        variable decl
        dict get [dict get $decl $sub/$key] -default
    }
}
set ::sent {}
eval [string range $conf $b $e]

set keys {}
dict for { k opts } $::settings::decl { lappend keys $k }
check $keys [concat photo/enabled photo/group photo/din photo/strobe photo/invert photo/period_ms photo/min_step photo/smooth_ms \
    photo/settle_ms photo/patch photo/patch_mode photo/patch_corner photo/patch_size \
    photo/patch_inset photo/patch_dark] "fifteen keys"
check [list $::ess::photo_patch $::ess::photo_patch_opts] \
    {0 {-mode marked -corner tr -size 60 -inset 0 -dark 0}} \
    "boot: the patch off, its defaults as stim2's"
dict for { k opts } $::settings::decl {
    set ap [dict get $opts -apply]
    check [llength [info commands $ap]] 1 "$k applies through $ap"
    if { [dict exists $opts -validate] } {
        set v [dict get $opts -validate]
        check [catch { $v [dict get $opts -default] }] 0 "$k: its default passes $v"
    }
    check [dict exists $opts -doc] 1 "$k is documented"
}
check [lindex $::sent end] {photo active 0} "boot: enabled pushed last, off by default"
check [lsearch -inline -index 1 $::sent group] {photo group photodiode} \
    "boot: group photodiode"

if { $fails } {
    puts "$fails FAILURE(S)"
    exit 1
}
puts "all checks passed"

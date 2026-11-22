#
# test_extio_models.tcl
#
#  Box MODELS (lib/extio_models-1.0.tm): a named kind of box we build more
#  than one of. Checks the pure half -- parsing model files, diffing a box's
#  export against a model, and the verdict that decides whether to OFFER a
#  model at all. The offer rule is the one that matters: a blank box, or one
#  whose name a model claims, gets an offer; a box someone configured by hand
#  into something else is left alone.
#
#  Also loads the SHIPPED config/models/*.tcl, so a model file that stops
#  parsing fails here instead of on a rig.
#
#  A UNIT test: plain tclsh, no dserv.
#  Run as: tclsh tests/test_extio_models.tcl   (or: ctest -R extio_models)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]
tcl::tm::path add $::REPO/lib
package require extio_models

set FAIL 0
proc ok   { label } { puts "  ok   $label" }
proc fail { label } { puts "  FAIL $label"; set ::FAIL 1 }
proc check { label expected actual } {
    if { $expected eq $actual } { ok $label } \
    else { fail "$label -- expected '$expected', got '$actual'" }
}

puts "parse"
set d [extio_models::parse_box "# comment\npin/10/label select\n\ndesc rig two left\n"]
check "leaf value" select [dict get $d pin/10/label]
check "value keeps its spaces" "rig two left" [dict get $d desc]
check "comments skipped" 2 [dict size $d]

set r [extio_models::parse_rig "slider source extio\ndial sources {ring touch}\n"]
check "rig triples" 2 [llength $r]
check "braced value whole" "ring touch" [lindex $r 1 2]
check "a bare 4-word line is refused" 1 \
    [catch { extio_models::parse_rig "dial sources ring touch" }]

puts "shipped models"
extio_models::config -dirs [list $::REPO/config/models]
set n [extio_models::load]
check "no load errors" "" [extio_models::errors]
check "astick present" 1 [expr {"astick" in [extio_models::names]}]
set astick [extio_models::get astick]
check "astick box has its stick group" stick [dict get $astick box ain/group/0/label]
check "astick box has its select group" stick_select [dict get $astick box group/0/label]
check "astick is calibrated by the slider" slider [dict get $astick calibrate]

puts "verdicts"
set want [dict get $astick box]
# a box configured exactly as the model, plus unit-level leaves a model ignores
set have [dict merge $want {dserv/ip 192.168.1.5 channel dev}]
set v [extio_models::verdict anyname teensy40 $have]
check "exact config matches whatever the name" matches [dict get $v state]
check "...and names the model" astick [dict get $v model]
check "...and offers nothing" "" [dict get $v offer]

set v [extio_models::verdict astick7 teensy40 {}]
check "blank box -> blank" blank [dict get $v state]
check "blank box -> offered" astick [dict get $v offer]

set v [extio_models::verdict box1 teensy41 {}]
check "wrong board -> nothing to offer" "" [dict get $v offer]

# the astick as it was this morning: no button configured
set nobutton [dict remove $want pin/10/mode pin/10/debounce_ms pin/10/active_low \
                  pin/10/label group/0/pins group/0/label]
set v [extio_models::verdict astick teensy40 $nobutton]
check "named box missing leaves -> differs" differs [dict get $v state]
check "...offered" astick [dict get $v offer]
check "...six leaves missing" 6 [dict size [dict get $v diff missing]]

set v [extio_models::verdict lever1 teensy40 {pin/3/mode in_pullup pin/3/label lever}]
check "hand-configured stranger -> unknown" unknown [dict get $v state]
check "...not nagged" "" [dict get $v offer]

set v [extio_models::verdict lever1 teensy40 {pin/3/mode in_pullup} astick]
check "a DECLARED model is judged even unnamed" differs [dict get $v state]

set d [extio_models::box_diff {ain/rate 100} {ain/rate 200 pin/4/mode out net/mode dhcp}]
check "differ carries want/have" {100 200} [dict get $d differ ain/rate]
check "extra counts shape leaves only" {pin/4/mode} [dict keys [dict get $d extra]]

puts "switch plan: cookie-io-eye -> photodiode"
check "photodiode present" 1 [expr {"photodiode" in [extio_models::names]}]
set eye   [dict get [extio_models::get cookie-io-eye] box]
set photo [dict get [extio_models::get photodiode] box]
# the box as exported: the model plus a unit calibration and a crystal trim
set have [dict merge $eye {meta/model cookie-io-eye meta/cal/slider {v1 x1} ain/clk_ppm -12}]
set plan [extio_models::switch_plan $photo $have]
set w [dict get $plan writes]
set leaves {}; foreach x $w { lappend leaves [lindex $x 0] }
proc pos {leaves leaf} { lsearch -exact $leaves $leaf }
proc val {w leaf} { foreach x $w { if { [lindex $x 0] eq $leaf } { return [lindex $x 1] } }; return "<none>" }
check "nothing kept" "" [dict get $plan kept]
check "stick group off" 1 [expr {[pos $leaves ain/group/0/off] >= 0}]
check "eye group off" 1 [expr {[pos $leaves ain/group/1/off] >= 0}]
check "eye group's mode back to on-change" onchange [val $w ain/group/1/mode]
check "every group off before the rate moves" 1 \
    [expr {[pos $leaves ain/group/1/off] < [pos $leaves ain/rate]}]
check "rate before the new group's channels" 1 \
    [expr {[pos $leaves ain/rate] < [pos $leaves ain/group/0/channels]}]
check "batch before continuous (else 5000 blocks/s is refused)" 1 \
    [expr {[lsearch -exact -all $leaves ain/group/0/batch] ne ""
           && [lindex [lsearch -exact -all $leaves ain/group/0/batch] end]
              < [lindex [lsearch -exact -all $leaves ain/group/0/mode] end]}]
check "group 0 ends continuous" continuous \
    [lindex [lsearch -inline -all -index 0 $w ain/group/0/mode] end 1]
check "stick pins cleared" off [val $w pin/15/mode]
check "button debounce cleared" 0 [val $w pin/3/debounce_ms]
check "active_low cleared" 0 [val $w pin/11/active_low]
check "chord groups cleared" off [val $w group/0/pins]
check "groups cleared before their pins" 1 \
    [expr {[pos $leaves group/0/pins] < [pos $leaves pin/3/mode]}]
check "obs mirror off" off [val $w obs/pin]
check "eye labels cleared" off [val $w ain/label/0]
check "A2 put in ain mode" ain [val $w pin/14/mode]
check "ain/enable already 1: not re-sent" <none> [val $w ain/enable]
check "calibration untouched" -1 [pos $leaves meta/cal/slider]
check "crystal trim untouched" -1 [pos $leaves ain/clk_ppm]
check "meta/model left to the caller" -1 [pos $leaves meta/model]

puts "switch plan: photodiode -> cookie-io-eye"
set plan [extio_models::switch_plan $eye $photo]
set w [dict get $plan writes]
check "photodiode group off, then re-sent as stick" 3,4 [val $w ain/group/0/channels]
check "old batch reset" 1 [val $w ain/group/0/batch]
check "A2 out of ain mode" off [val $w pin/14/mode]
check "rate back to 250" 250 [val $w ain/rate]
check "eye group re-sent" 0,1 [val $w ain/group/1/channels]
check "a leaf with no default is kept, not guessed" {ain/foo} \
    [dict get [extio_models::switch_plan {} {ain/foo 3}] kept]

puts "switch: rig knobs to clear"
set rows {}
foreach {sub key ok} {eye source 1  slider source 1  button 0 0  photo group 1} {
    lappend rows [dict create sub $sub key $key ok $ok]
}
set clr [extio_models::rig_to_clear {} {{photo group photodiode}} $rows]
set names {}; foreach r $clr { lappend names "[dict get $r sub] [dict get $r key]" }
check "old knobs at the model's value are cleared; changed-by-hand and shared ones kept" \
    {{eye source} {slider source}} $names

puts "snapshot round trip"
set dir [file join [expr {[info exists ::env(TMPDIR)] ? $::env(TMPDIR) : "/tmp"}] \
             extio_models_test_[pid]]
file mkdir $dir
set f [open $dir/snap.tcl w]
puts $f [extio_models::snapshot_text snap "a test box" teensy40 $have]
close $f
extio_models::config -dirs [list $::REPO/config/models $dir]
extio_models::load
check "snapshot parses" "" [extio_models::errors]
check "snapshot drops unit leaves" 0 \
    [dict exists [extio_models::get snap] box dserv/ip]
set v [extio_models::verdict x teensy40 $want]
check "snapshot of a match matches too (tie broken by name)" astick [dict get $v model]
file delete -force $dir

if { $FAIL } { puts "FAILURES"; exit 1 }
puts "all checks passed"

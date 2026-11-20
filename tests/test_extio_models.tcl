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

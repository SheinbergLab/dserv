#
# test_settings.tcl
#
#  A declared knob records WHERE it was declared.
#
#  settings::put must run in the interp that declared the knob, and until
#  now the schema did not say which -- so a page could read every setting on
#  the rig and write none of them (docs/settings_panel_plan.md §0). dserv
#  stamps ::dserv_interp into each interp it creates; settings::declare
#  copies it into the declaration, and the schema datapoint carries it.
#
#  The failure this guards against is the quiet one: a knob declared with no
#  interp recorded (or a wrong one) sends the put to a live interp where the
#  knob does not exist. `send` succeeds, `put` errors from _effective, and
#  the setting simply is not there. So both halves are checked -- what is
#  recorded, and that -interp cannot be supplied by hand.
#
#  A UNIT test: plain tclsh, dserv STUBBED. ::dserv_interp is an ordinary
#  global, which is exactly what makes both the named and the nameless case
#  reachable from here.
#
#  Run as: tclsh tests/test_settings.tcl   (or: ctest -R settings)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

set FAIL 0
proc ok   { label } { puts "  ok   $label" }
proc fail { label } { puts "  FAIL $label"; set ::FAIL 1 }
proc check { label expected actual } {
    if { $expected eq $actual } {
        ok $label
    } else {
        fail "$label -- expected '$expected', got '$actual'"
    }
}

# the one dserv command settings-1.0.tm publishes through
array set ::DP {}
proc dservSet { name val } { set ::DP($name) $val }

source [file join $::REPO lib settings-1.0.tm]

# NOT local/rig.tcl: this repo has one, and a test that reads the developer's
# own rig file passes or fails by accident.
set ::RIGFILE [file join [file dirname [info script]] .test_rig_[pid].tcl]
settings::config -file $::RIGFILE
file delete $::RIGFILE

puts "settings interp stamping"

#
# 1. No ::dserv_interp -- plain tclsh, or a nameless one-off interp. Empty
#    is the honest answer: there is no route, and the panel must show such a
#    knob read-only rather than guess at one.
#
check "unnamed interp records empty" "" [settings::_interp_name]
settings::declare probe orphan -default 1 -type bool -doc "no interp"
check "declaration records empty interp" "" [settings::interp_of probe orphan]
check "schema datapoint carries the field" 1 \
    [dict exists $::DP(settings/probe/orphan/schema) interp]

# A declaration publishes its VALUE too, not just its schema. _publish_declared
# runs at load, so a knob declared after the first load used to appear in the
# tree with a schema and no value and no source -- present but unreadable to
# anything enumerating settings/*, which is how a panel sees the rig.
check "declare publishes the value" 1 [settings::get probe orphan]
settings::get probe orphan          ;# force the load that _effective triggers
settings::declare probe late -default 42 -type int
check "a LATE declaration publishes too" 42 $::DP(settings/probe/late)
check "and its source" default $::DP(settings/probe/late/source)

#
# 2. Named interp. The value is the REGISTRY name, i.e. what `send` takes.
#
set ::dserv_interp ess
settings::declare joystick transport -default none \
    -values {none box dial} -doc "where joystick input comes from"
check "declaration records the interp" ess [settings::interp_of joystick transport]
check "schema datapoint records the interp" ess \
    [dict get $::DP(settings/joystick/transport/schema) interp]

# main is "dserv" -- a name `send` REFUSES ("cannot send directly to
# dserv"), so a caller has to recognize it and evaluate directly. Recorded
# as-is rather than as some friendlier word that matches no interp at all.
set ::dserv_interp dserv
settings::declare juicer destination -default auto \
    -values {auto usb gpio extio:<box>/<pin>} -doc "where juice pulses go"
check "main records its registry name" dserv [settings::interp_of juicer destination]

set ::dserv_interp ess

#
# 3. -interp is not an option. A call site that could name its own interp is
#    a call site that could name the wrong one.
#
set rc [catch { settings::declare probe typo -default 1 -interp juicer } err]
check "-interp rejected" 1 $rc
check "-interp error names the option" 1 [string match "*unknown option '-interp'*" $err]

#
# 4. The added field is inert: declare/get/put/example behave as before.
#
check "get returns the default" none [settings::get joystick transport]
check "source is default" default [settings::source_of joystick transport]
check "put validates" box [settings::put joystick transport box]
check "put is runtime until persisted" runtime [settings::source_of joystick transport]
set rc [catch { settings::put joystick transport sideways } err]
check "a bad value still throws" 1 $rc
check "the error still teaches" 1 [string match "*not one of {none box dial}*" $err]
check "put -persist reclassifies" file \
    [expr {[settings::put joystick transport dial -persist]
           eq "dial" ? [settings::source_of joystick transport] : "put failed"}]
check "the file line was written" 1 [file exists $::RIGFILE]
check "example still generates" 1 \
    [string match "*setting joystick transport none*" [settings::example joystick]]

#
# 5. A file line is judged when its knob BECOMES known, not only when the
#    file is parsed. Otherwise whichever knob triggers the first load leaves
#    every later declaration's file value unvalidated -- an order dependency
#    nobody could see, since the declarations live in different modules.
#
set fd [open $::RIGFILE w]
puts $fd "setting later knob bogus"
close $fd
settings::config -file $::RIGFILE       ;# resets loaded
settings::load                          ;# ... with the sub `later` still UNKNOWN
check "an unknown sub's line is held" 0 [llength [settings::errors]]
settings::declare later knob -default a -values {a b}
check "declaring it validates the held line" a [settings::get later knob]
check "and leaves a breadcrumb" 1 \
    [string match "*'bogus' not one of {a b}*" [lindex [settings::errors] 0]]

#
# 6. clear -- the only verb that takes a value BACK. Without it the panel
#    can only ever add lines to the file it exists to stop people editing.
#
set fd [open $::RIGFILE w]
puts $fd "# a human's comment, and it must survive"
puts $fd "setting keep mine 7"
close $fd
settings::config -file $::RIGFILE
settings::declare keep mine -default 0 -type int
settings::declare cl knob -default a -values {a b c}

check "file value in force" 7 [settings::get keep mine]
settings::put cl knob b -persist
check "persisted" file [settings::source_of cl knob]
settings::put cl knob c
check "runtime on top" runtime [settings::source_of cl knob]

# one layer at a time: the runtime override goes, the declaration stays --
# so what /source reported is exactly what was undone
check "clear pops the runtime layer" b [settings::clear cl knob]
check "the file value is back" file [settings::source_of cl knob]
check "clear again reaches the default" a [settings::clear cl knob]
check "and the source says so" default [settings::source_of cl knob]

set fd [open $::RIGFILE]; set txt [read $fd]; close $fd
check "our line is gone" 0 [string match "*setting cl knob*" $txt]
check "the generated comment went with it" 0 [string match "*# persisted*" $txt]
check "the human's comment survived" 1 [string match "*human's comment*" $txt]
check "an unrelated line survived" 1 [string match "*setting keep mine 7*" $txt]
check "and its value still works" 7 [settings::get keep mine]

# -all strips both layers in one call
settings::put cl knob b -persist
settings::put cl knob c
check "-all goes straight to default" a [settings::clear cl knob -all]
check "nothing left in the file" 0 \
    [string match "*setting cl knob*" [read [set fd [open $::RIGFILE]]]][close $fd]

# -apply fires on a real change, and NOT on a no-op: clearing an override
# that matched the file value must not re-bind hardware for nothing
set ::APPLIED {}
settings::declare cl hooked -default a -values {a b} \
    -apply {apply {{v} {lappend ::APPLIED $v}}}
settings::put cl hooked b -persist
settings::put cl hooked b                 ;# runtime override, same value
set ::APPLIED {}                          ;# (put itself always applies)
settings::clear cl hooked                 ;# pops it: effective unchanged
check "no apply for a no-op clear" 0 [llength $::APPLIED]
settings::clear cl hooked                 ;# now the file line goes
check "apply fired on the real change" a [lindex $::APPLIED 0]

#
# 7. Parse-error breadcrumbs are PER INTERP. Every interp reads the whole
#    file but judges only its own declarations, so one flat datapoint meant
#    whichever interp published last decided what a page could see.
#
set fd [open $::RIGFILE w]
puts $fd "setting cl knob nonsense"
close $fd
set ::dserv_interp juicer
settings::reload
check "published under this interp" 1 \
    [info exists ::DP(settings/parse_errors/juicer)]
check "and names the bad value" 1 \
    [string match "*'nonsense' not one of*" $::DP(settings/parse_errors/juicer)]
check "no flat datapoint any more" 0 [info exists ::DP(settings/parse_errors)]

file delete $::RIGFILE

if { $FAIL } {
    puts "FAILURES"
    exit 1
}
puts "all checks passed"

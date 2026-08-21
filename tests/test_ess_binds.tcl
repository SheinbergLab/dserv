#
# test_ess_binds.tcl
#
#  button_bind / joystick_bind read guards: reading a binding must never
#  clear it. Extracts the two procs from lib/ess_transports-1.0.tm so the
#  test tracks the file rather than a copy of it.
#
#  A UNIT test: dserv is STUBBED, so this runs under plain tclsh with no
#  server, no rig and no hardware. That is the point -- the geometry and
#  the state machine are the parts worth pinning down, and they are pure
#  Tcl. The integration tests beside it (test_triggers, test_private) run
#  the other way, inside dserv via --tscript.
#
#  Run as: tclsh tests/test_ess_binds.tcl        (or: ctest -R ess_binds)
#

# repo root, from this script's location -- so it runs from anywhere
set ::REPO [file normalize [file join [file dirname [info script]] ..]]

namespace eval ess {
    variable joystick_binding {}
    variable button_bindings
    array set button_bindings {}
}

# Pull the two procs out of the real source so the test tracks the file
# rather than a copy of it.
#
# Located by string search, NOT by a regex over the whole file: ess-2.0.tm
# is ~310 KB and a non-greedy (.*?) across it backtracked for TWENTY
# SECONDS. The scan below is linear, and the regex only ever sees one
# proc's worth of text.
set fh [open [file join $::REPO lib ess_transports-1.0.tm]]
set src [read $fh]
close $fh

# Returns {arglist body}, or "" if the proc could not be found.
#
# No regex at all. Beyond the backtracking, a pattern matching literal
# braces has to spell them \\{ and \\}, and Tcl's brace counter counts
# those when it parses the script -- so the pattern unbalances the very
# proc that contains it. The proc's own header line is already a Tcl list
# once its trailing brace is trimmed, so lindex does the work.
proc extract_proc { src name } {
    set start [string first "\n    proc $name " $src]
    if { $start < 0 } { return "" }
    set end [string first "\n    \}\n" $src $start]
    if { $end < 0 } { return "" }
    set nl   [string first "\n" $src [expr {$start + 1}]]
    set head [string trimright [string range $src [expr {$start + 1}] \
                                    [expr {$nl - 1}]] " \{"]
    set body [string range $src [expr {$nl + 1}] [expr {$end - 1}]]
    return [list [lindex $head 2] $body]
}

foreach p {joystick_bind button_bind} {
    set got [extract_proc $src $p]
    if { $got eq "" } { puts "FAIL: could not extract proc $p"; exit 1 }
    proc ::ess::$p [lindex $got 0] [lindex $got 1]
}

set FAIL 0
proc check { label got want } {
    if { $got eq $want } { puts "  ok   $label" } else {
        puts "  FAIL $label: got '$got' want '$want'"; set ::FAIL 1
    }
}

puts "joystick_bind:"
::ess::joystick_bind box {* joystick}
check "set, then read is non-destructive" \
    [::ess::joystick_bind] "{} box {* joystick}"
check "still set after the read" \
    [set ::ess::joystick_binding] "{} box {* joystick}"
check "reading twice is stable" [::ess::joystick_bind] [::ess::joystick_bind]
check "leading {} placeholder form still works" \
    [::ess::joystick_bind {} box {* pad}] "{} box {* pad}"
check "explicit clear" [::ess::joystick_bind {}] ""
check "cleared for real" [set ::ess::joystick_binding] ""

puts "\nbutton_bind:"
::ess::button_bind 0 {} box {* response left}
::ess::button_bind 1 {} box {* response right}
check "read one channel" [::ess::button_bind 0] "{} box {* response left}"
check "read did not clear it" [::ess::button_bind 0] "{} box {* response left}"
check "other channel intact" [::ess::button_bind 1] "{} box {* response right}"
check "read-all returns a dict" [::ess::button_bind] \
    "0 {{} box {* response left}} 1 {{} box {* response right}}"
check "read-all did not clear" [::ess::button_bind 0] "{} box {* response left}"
check "unbound channel reads empty" [::ess::button_bind 7] ""
check "local pin form still works" [::ess::button_bind 2 24] "24"
check "explicit clear" [::ess::button_bind 2 {}] ""
check "cleared channel gone from read-all" \
    [dict exists [::ess::button_bind] 2] 0
check "clearing one leaves the others" \
    [::ess::button_bind 0] "{} box {* response left}"

puts ""
if { $FAIL } { puts "FAILED"; exit 1 } else { puts "all checks passed" }

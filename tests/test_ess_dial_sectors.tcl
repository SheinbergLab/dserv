#
# test_ess_dial_sectors.tcl
#
#  joystick sector <-> dial angle round trip. Sector k is clockwise from
#  up and maps to 90-45k degrees; targets.tcl inverts it. Getting this
#  backwards is silent -- the report simply lands on the wrong target.
#
#  A UNIT test: dserv is STUBBED, so this runs under plain tclsh with no
#  server, no rig and no hardware. That is the point -- the geometry and
#  the state machine are the parts worth pinning down, and they are pure
#  Tcl. The integration tests beside it (test_triggers, test_private) run
#  the other way, inside dserv via --tscript.
#
#  Run as: tclsh tests/test_ess_dial_sectors.tcl        (or: ctest -R ess_dial_sectors)
#

# repo root, from this script's location -- so it runs from anywhere
set ::REPO [file normalize [file join [file dirname [info script]] ..]]

set pi 3.14159265358979
set names {up up_right right down_right down down_left left up_left}
set fail 0
for { set k 0 } { $k < 8 } { incr k } {
    # forward, exactly as dial_dpad_dir computes it
    set a [expr {(90.0 - 45.0*$k)*$pi/180.0}]
    set a [expr {fmod($a, 2*$pi)}]
    if { $a < 0 } { set a [expr {$a + 2*$pi}] }
    # reverse, exactly as `responded` computes it
    set deg [expr {$a*180.0/$pi}]
    set back [expr {(int(round((90.0-$deg)/45.0)) % 8 + 8) % 8}]
    set ok [expr {$back == $k ? "ok  " : "FAIL"}]
    if { $back != $k } { set fail 1 }
    puts [format "  %s sector %d %-11s angle %6.1f deg -> sector %d" \
              $ok $k [lindex $names $k] $deg $back]
}
puts [expr {$fail ? "FAILED" : "all checks passed"}]
exit $fail

#
# test_ess_sling.tcl
#
#  ess_sling: the slingshot response mode. Covers the pull -> launch-velocity
#  mapping (and pins it to sling_sim's copy when that package is on disk),
#  arming and the latches, the stick source (engage / release thresholds,
#  the peak-hold window that beats a self-centring stick's spring-back, the
#  abort on a short draw), the mouse source (press / drag / release, grab
#  radius vs press-relative pulls, the y flip), what is published for things
#  that draw, and the state-machine discipline (woken on engage, commit and
#  abort -- never on a pull sample).
#
#  A UNIT test: dserv is STUBBED, so this runs under plain tclsh with no
#  server, no rig and no hardware -- the arrangement of test_ess_roam.tcl.
#
#  Run as: tclsh tests/test_ess_sling.tcl        (or: ctest -R ess_sling)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

namespace eval ess {}
array set ::DP {}
set ::DPLOG {}
proc dservSet { name val } { set ::DP($name) $val; lappend ::DPLOG [list $name $val] }
proc dservExists { name } { return [info exists ::DP($name)] }
proc dservGet { name } { return $::DP($name) }
proc dservTimestamp { name } { return $::CLOCK }
proc dservAddExactMatch { args } {}
proc dservAddMatch { args } {}
proc dpointAddScript { args } {}
proc dpointRemoveScript { args } {}
proc do_update {} { incr ::UPDATES }
set ::UPDATES 0

set ::CLOCK 1000000
proc now {} { return $::CLOCK }
proc advance_ms { ms } { incr ::CLOCK [expr {int($ms*1000)}] }

# ::ess::stick_gain lives in ess_transports; copied verbatim (as
# test_ess_roam does) so this stays a unit test.
namespace eval ess {
    proc stick_gain { f deadzone expo } {
        if { $f < $deadzone } { return 0.0 }
        set g [expr {($f - $deadzone)/(1.0 - $deadzone)}]
        if { $g <= 0.0 } { return 0.0 }
        if { $g > 1.0 } { set g 1.0 }
        if { $expo != 1.0 } { set g [expr {pow($g, $expo)}] }
        return $g
    }
}

source [file join $::REPO lib ess_sling-1.0.tm]

set FAIL 0
proc check { label got want } {
    if { $got eq $want } { puts "  ok   $label" } else {
        puts "  FAIL $label: got '$got' want '$want'"; set ::FAIL 1
    }
}
proc approx { label got want { tol 0.05 } } {
    if { abs($got - $want) <= $tol } { puts "  ok   $label ($got)" } else {
        puts "  FAIL $label: got $got want ~$want"; set ::FAIL 1
    }
}
proc fails { label script } {
    if { [catch { uplevel 1 $script } err] } {
        puts "  ok   $label ([string range $err 0 50]...)"
    } else {
        puts "  FAIL $label: did not raise"; set ::FAIL 1
    }
}
proc pull_dp {} { return [split $::DP(ess/sling/pull) ,] }
proc n_pull_pubs {} {
    set n 0
    foreach e $::DPLOG { if { [lindex $e 0] eq "ess/sling/pull" } { incr n } }
    return $n
}

###########################################################################
puts "the mapping: pull -> launch velocity"
###########################################################################

::ess::sling_init -sources mouse -reach 3.0 -v_max 16.0
lassign [::ess::sling_velocity -3.0 0.0] vx vy
approx "full pull -x launches +x at v_max" $vx 16.0 1e-9
approx "no y"                              $vy 0.0  1e-9
lassign [::ess::sling_velocity -30.0 0.0] vx vy
approx "over-draw clamps at v_max"         $vx 16.0 1e-9
lassign [::ess::sling_velocity 0.0 1.5] vx vy
approx "half draw up launches DOWN at v_max/2" $vy -8.0 1e-9

# pin to the dlsh sling_sim copy when it is on disk beside this checkout
set sim [file normalize [file join $::REPO .. dlsh vfs lib sling_sim sling_sim.tcl]]
catch { source /usr/local/dlsh/dlsh_setup.tcl }
if { [file exists $sim] && ![catch { package require dlsh }] && ![catch { source $sim } serr] } {
    set spec [dict merge [sling_sim::default_spec] {reach 3.0 v_max 16.0}]
    set same 1
    foreach { dx dy } {-3 0  -1.5 -1.5  0.7 -2.9  -40 5  0 0} {
        if { [::ess::sling_velocity $dx $dy] ne [sling_sim::velocity $spec $dx $dy] } { set same 0 }
    }
    check "ess_sling and sling_sim agree on the mapping" $same 1
} else {
    puts "  skip sling_sim pin (sling_sim not loadable here: [expr {[info exists serr] ? $serr : {no dlsh}}])"
}

###########################################################################
puts "\nsling_held: is the hand on the input right now (the let-go gate):"
###########################################################################

::ess::sling_init -sources stick -scale 1.0 -engage 0.30 -release 0.15
check "nothing held at init" [::ess::sling_held] 0
::ess::sling_simulate_stick -0.5 0.0                  ;# NOT armed: still tracked
check "a deflected stick is held even unarmed" [::ess::sling_held] 1
::ess::sling_simulate_stick 0.05 0.0
check "centred: not held"                       [::ess::sling_held] 0
::ess::sling_init -sources mouse
::ess::sling_simulate_mouse press 0 0
check "a mouse button down is held (unarmed)"   [::ess::sling_held] 1
::ess::sling_simulate_mouse release 0 0
check "button up: not held"                     [::ess::sling_held] 0

###########################################################################
puts "\ninit + arm:"
###########################################################################

fails "a dial reading word is refused" { ::ess::sling_init -sources rate }
fails "unknown source refused"          { ::ess::sling_init -sources trackball }
::ess::sling_init -sources {mouse touch}
check "mouse and touch may be bound together" $::DP(ess/sling/sources) {mouse touch}
fails "release above engage refused"    { ::ess::sling_init -sources stick -engage 0.2 -release 0.3 }
fails "stick arm without a scale refuses" {
    ::ess::sling_init -sources stick
    ::ess::sling_arm
}

::ess::sling_init -sources mouse -anchor {-9.0 -3.0} -reach 3.0 -v_max 16.0 -min_frac 0.2
check "active"            $::DP(ess/sling_active) 1
check "state idle"        $::DP(ess/sling/state) idle
check "geometry published" $::DP(ess/sling/geometry) "-9.0000,-3.0000,3.0000,16.0000,0.2000"
check "pull hidden"       [lindex [pull_dp] 5] 0
::ess::sling_arm
check "armed"             [::ess::sling_armed] 1
check "state armed"       $::DP(ess/sling/state) armed
check "nothing committed" [::ess::sling_committed] 0

###########################################################################
puts "\nmouse: press anywhere, pull is measured from the press:"
###########################################################################

set ::UPDATES 0
::ess::sling_simulate_mouse press 2.0 2.0
check "engaged on press"          [::ess::sling_engaged] 1
check "one wake on engage"        $::UPDATES 1
check "engage time = press time"  [::ess::sling_engage_time] $::CLOCK
check "source is mouse"           [::ess::sling_engage_source] mouse
check "state engaged"             $::DP(ess/sling/state) engaged
set t_engage $::CLOCK

advance_ms 20
::ess::sling_simulate_mouse drag 1.0 0.5
check "no wake on a drag"  $::UPDATES 1
lassign [::ess::sling_pull] dx dy fr
approx "pull dx from the press point" $dx -1.0 1e-9
approx "pull dy"                      $dy -1.5 1e-9
approx "frac = |pull|/reach"          $fr [expr {hypot(1.0,1.5)/3.0}] 1e-9
lassign [pull_dp] pdx pdy pvx pvy pfr pshow
check  "pull published and shown"  $pshow 1
approx "published vx = -dx/reach*v_max" $pvx [expr {1.0/hypot(1.0,1.5)*16.0*hypot(1.0,1.5)/3.0}] 1e-3

advance_ms 20
::ess::sling_simulate_mouse drag -8.0 -8.0            ;# way past the reach
lassign [::ess::sling_pull] dx dy fr
approx "over-draw clamps frac to 1" $fr 1.0 1e-9
approx "...and the pull to the reach" [expr {hypot($dx,$dy)}] 3.0 1e-6

advance_ms 20
::ess::sling_simulate_mouse release -1.0 -1.0          ;# pull (-3,-3) -> clamped
check  "committed"                 [::ess::sling_committed] 1
check  "two wakes: engage + commit" $::UPDATES 2
check  "window closed by the commit" [::ess::sling_armed] 0
check  "no longer engaged"         [::ess::sling_engaged] 0
check  "state released"            $::DP(ess/sling/state) released
check  "release time = release sample" [::ess::sling_release_time] $::CLOCK
lassign [::ess::sling_release_vec] dx dy vx vy fr src
approx "committed dx (clamped)" $dx [expr {-3.0/sqrt(2)}] 1e-6
approx "committed frac"          $fr 1.0 1e-9
approx "launch vx up-right"      $vx [expr {16.0/sqrt(2)}] 1e-6
approx "launch vy up-right"      $vy [expr {16.0/sqrt(2)}] 1e-6
check  "source"                  $src mouse
lassign [split $::DP(ess/sling/release) ,] rvx rvy rdx rdy rfr rsrc
approx "ess/sling/release carries vx" $rvx $vx 1e-3
check  "pull hidden after release"  [lindex [pull_dp] 5] 0

advance_ms 50
::ess::sling_simulate_mouse press 0 0
check "a press after the commit is ignored (window closed)" [::ess::sling_engaged] 0
check "and does not wake" $::UPDATES 2

###########################################################################
puts "\nmouse: a short draw is an ABORT, and the draw may be retried:"
###########################################################################

::ess::sling_arm
set ::UPDATES 0
::ess::sling_simulate_mouse press 0 0
advance_ms 10
::ess::sling_simulate_mouse release 0.2 0.1           ;# frac 0.07 < min 0.2
check "not committed"        [::ess::sling_committed] 0
check "one abort counted"    [::ess::sling_aborts] 1
check "abort time recorded"  [::ess::sling_abort_time] $::CLOCK
check "state aborted"        $::DP(ess/sling/state) aborted
check "engage + abort = two wakes" $::UPDATES 2
check "still armed"          [::ess::sling_armed] 1
advance_ms 10
::ess::sling_simulate_mouse press 0 0
advance_ms 10
::ess::sling_simulate_mouse release -2.0 0.0
check "retry commits"        [::ess::sling_committed] 1
check "aborts kept"          [::ess::sling_aborts] 1

###########################################################################
puts "\nmouse: grab_radius > 0 -- press ON the seat, pull from the anchor:"
###########################################################################

::ess::sling_init -sources mouse -anchor {-9.0 -3.0} -reach 3.0 -v_max 16.0 -grab_radius 1.0
::ess::sling_arm
::ess::sling_simulate_mouse press 0 0
check "a press off the seat does not engage" [::ess::sling_engaged] 0
::ess::sling_simulate_mouse press -8.6 -3.2
check "a press on the seat engages"          [::ess::sling_engaged] 1
lassign [::ess::sling_pull] dx dy fr
approx "pull measured from the ANCHOR, not the press" $dx 0.4 1e-9
::ess::sling_simulate_mouse drag -11.0 -4.0
lassign [::ess::sling_pull] dx dy fr
approx "drag: dx = cursor - anchor" $dx -2.0 1e-9
approx "drag: dy"                   $dy -1.0 1e-9
::ess::sling_simulate_mouse release -11.0 -4.0
check "committed" [::ess::sling_committed] 1

puts "\n... the raw mouse handler flips y and scales the extent:"
::ess::sling_init -sources mouse -anchor {0 0} -reach 3.0 -v_max 16.0
set ::DP(ess/screen_halfx) 16.0
set ::DP(ess/screen_halfy) 9.0
::ess::sling_mouse_range mouse/event/range {0 1600 0 900}     ;# 32 deg / 1600 px = 50 px/deg
::ess::sling_arm
::ess::sling_mouse_sample mouse/event {800 450 0}             ;# press at centre
::ess::sling_mouse_sample mouse/event {900 550 1}             ;# drag +100px right, +100px DOWN
lassign [::ess::sling_pull] dx dy fr
approx "x: +100 px = +2 deg" $dx 2.0 1e-6
approx "y: +100 px screen-down = -2 deg" $dy -2.0 1e-6
::ess::sling_mouse_sample mouse/event {900 550 3}             ;# MOVE is ignored
::ess::sling_mouse_sample mouse/event {900 550 2}
check "release via the raw handler commits" [::ess::sling_committed] 1

###########################################################################
puts "\ntouch: mtouch/event in screen pixels, mapped like touch_pixels_to_deg:"
###########################################################################

set ::DP(ess/screen_w) 580
set ::DP(ess/screen_h) 340
set ::DP(ess/screen_halfx) 29.0     ;# 10 px/deg
set ::DP(ess/screen_halfy) 17.0     ;# 10 px/deg
::ess::sling_init -sources touch -anchor {-9.0 -3.0} -reach 3.0 -v_max 16.0 -min_frac 0.2
check "touch accepted as a source" $::DP(ess/sling/sources) touch
::ess::sling_arm
set ::UPDATES 0
::ess::sling_touch_sample mtouch/event {200 200 0}           ;# press: (-9, -3) deg = the seat
check "finger down engages"        [::ess::sling_engaged] 1
check "held while the finger is down" [::ess::sling_held] 1
check "source is touch"            [::ess::sling_engage_source] touch
advance_ms 16
::ess::sling_touch_sample mtouch/event {180 210 1}           ;# drag 20 px left, 10 px DOWN
lassign [::ess::sling_pull] dx dy fr
approx "drag dx = -2 deg" $dx -2.0 1e-6
approx "drag dy = -1 deg (screen-down is negative)" $dy -1.0 1e-6
check  "no wake on a drag" $::UPDATES 1
advance_ms 16
::ess::sling_touch_sample mtouch/event {175 215 2}           ;# lift
check  "lift commits" [::ess::sling_committed] 1
check  "not held after the lift" [::ess::sling_held] 0
lassign [::ess::sling_release_vec] dx dy vx vy fr src
check  "commit carries the touch source" $src touch
approx "committed dx" $dx -2.5 1e-6
check  "launches up-right" [expr {$vx > 0 && $vy > 0}] 1
check  "release datapoint names touch" [lindex [split $::DP(ess/sling/release) ,] 5] touch

puts "\n... the range is read at init; a missing screen datapoint is refused, not guessed:"
unset ::DP(ess/screen_w)
::ess::sling_init -sources touch -anchor {0 0}
::ess::sling_arm
::ess::sling_touch_sample mtouch/event {200 200 0}
check "no range -> touch ignored" [::ess::sling_engaged] 0
set ::DP(ess/screen_w) 580

###########################################################################
puts "\nstick: deflection is the pull; engage/release thresholds:"
###########################################################################

::ess::sling_init -sources stick -anchor {-9 -3} -reach 3.0 -v_max 16.0 \
    -scale 1.0 -deadzone 0.0 -expo 1.0 -engage 0.30 -release 0.15 -window_ms 60 -min_frac 0.2
::ess::sling_arm
set ::UPDATES 0
::ess::sling_simulate_stick 0.1 0.0
check "below engage: nothing" [::ess::sling_engaged] 0
check "and no wake"           $::UPDATES 0
advance_ms 8
::ess::sling_simulate_stick -0.5 0.0
check "engaged once past the threshold" [::ess::sling_engaged] 1
check "one wake"                        $::UPDATES 1
lassign [::ess::sling_pull] dx dy fr
approx "half deflection = half reach" $dx -1.5 1e-9
approx "frac 0.5"                      $fr 0.5 1e-9

# hold at full draw for 200 ms, then spring back over three 8 ms samples
for { set i 0 } { $i < 25 } { incr i } {
    advance_ms 8
    ::ess::sling_simulate_stick -0.7 -0.7
}
check "no wake while drawing" $::UPDATES 1
advance_ms 8 ; ::ess::sling_simulate_stick -0.4 -0.4
advance_ms 8 ; ::ess::sling_simulate_stick -0.2 -0.2
advance_ms 8 ; ::ess::sling_simulate_stick -0.05 -0.05     ;# under release
check  "released -> committed"  [::ess::sling_committed] 1
check  "two wakes"              $::UPDATES 2
lassign [::ess::sling_release_vec] dx dy vx vy fr src
approx "the PEAK deflection was committed, not the last sample" $fr [expr {hypot(0.7,0.7)}] 1e-6
check  "source stick"           $src stick
check  "up-right launch"        [expr {$vx > 0 && $vy > 0}] 1

puts "\n... a spring-back through the deadzone after a SHORT draw aborts:"
::ess::sling_arm
set ::UPDATES 0
advance_ms 8 ; ::ess::sling_simulate_stick -0.35 0.0       ;# just engaged, frac 0.35
advance_ms 8 ; ::ess::sling_simulate_stick -0.1 0.0        ;# released; peak 0.35 >= min 0.2
check "a real short flick commits" [::ess::sling_committed] 1
::ess::sling_init -sources stick -anchor {-9 -3} -reach 3.0 -v_max 16.0 \
    -scale 1.0 -deadzone 0.0 -engage 0.30 -release 0.15 -min_frac 0.5
::ess::sling_arm
advance_ms 8 ; ::ess::sling_simulate_stick -0.35 0.0
advance_ms 8 ; ::ess::sling_simulate_stick -0.1 0.0
check "under min_frac -> abort"   [::ess::sling_committed] 0
check "abort counted"             [::ess::sling_aborts] 1
check "window stays open"         [::ess::sling_armed] 1

puts "\n... the window forgets samples older than window_ms:"
::ess::sling_init -sources stick -anchor {-9 -3} -reach 3.0 -v_max 16.0 \
    -scale 1.0 -deadzone 0.0 -engage 0.30 -release 0.15 -window_ms 60 -min_frac 0.2
::ess::sling_arm
advance_ms 8 ; ::ess::sling_simulate_stick -1.0 0.0        ;# full draw
for { set i 0 } { $i < 40 } { incr i } {                  ;# ease off to 0.5 for 320 ms
    advance_ms 8
    ::ess::sling_simulate_stick -0.5 0.0
}
advance_ms 8 ; ::ess::sling_simulate_stick -0.1 0.0
lassign [::ess::sling_release_vec] dx dy vx vy fr src
approx "a deliberate ease-off is honoured (peak outside the window)" $fr 0.5 1e-6

puts "\n... -invert makes the deflection the AIM:"
::ess::sling_init -sources stick -anchor {-9 -3} -reach 3.0 -v_max 16.0 \
    -scale 1.0 -deadzone 0.0 -engage 0.30 -release 0.15 -invert 1
::ess::sling_arm
advance_ms 8 ; ::ess::sling_simulate_stick 0.7 0.7          ;# push up-right
advance_ms 8 ; ::ess::sling_simulate_stick 0.0 0.0
lassign [::ess::sling_release_vec] dx dy vx vy fr src
check "launch goes where the stick pointed" [expr {$vx > 0 && $vy > 0}] 1

puts "\n... the deadzone shapes the gain:"
::ess::sling_init -sources stick -anchor {-9 -3} -reach 3.0 -v_max 16.0 \
    -scale 1.0 -deadzone 0.2 -engage 0.30 -release 0.15
::ess::sling_arm
advance_ms 8 ; ::ess::sling_simulate_stick -0.6 0.0
lassign [::ess::sling_pull] dx dy fr
approx "gain rescaled past the deadzone: (0.6-0.2)/0.8 = 0.5" $fr 0.5 1e-9

###########################################################################
puts "\npublishing: throttled, but a show change never is:"
###########################################################################

::ess::sling_init -sources mouse -anchor {0 0} -reach 3.0 -v_max 16.0 -pub_ms 16
::ess::sling_arm
set ::DPLOG {}
::ess::sling_simulate_mouse press 0 0
set n0 [n_pull_pubs]
check "engage publishes" [expr {$n0 >= 1}] 1
advance_ms 4 ; ::ess::sling_simulate_mouse drag -0.5 0
advance_ms 4 ; ::ess::sling_simulate_mouse drag -0.6 0
advance_ms 4 ; ::ess::sling_simulate_mouse drag -0.7 0
check "drags inside pub_ms are throttled" [n_pull_pubs] $n0
advance_ms 20 ; ::ess::sling_simulate_mouse drag -0.8 0
check "a drag past pub_ms publishes"     [n_pull_pubs] [expr {$n0 + 1}]
advance_ms 1 ; ::ess::sling_simulate_mouse release -0.8 0
check "the release publishes (show change) regardless" [n_pull_pubs] [expr {$n0 + 2}]
check "and hides the pull" [lindex [pull_dp] 5] 0

###########################################################################
puts "\ndisarm + deinit:"
###########################################################################

::ess::sling_arm
::ess::sling_simulate_mouse press 0 0
::ess::sling_disarm
check "disarm closes the window"  [::ess::sling_armed] 0
check "and drops the draw"        [::ess::sling_engaged] 0
check "state idle"                $::DP(ess/sling/state) idle
::ess::sling_simulate_mouse release -2 0
check "a release after disarm is ignored" [::ess::sling_committed] 0

::ess::sling_deinit
check "gate published false" $::DP(ess/sling_active) 0
check "pull hidden"          [lindex [pull_dp] 5] 0
check "not active"           [::ess::sling_active] 0

puts ""
if { $FAIL } { puts "FAILURES"; exit 1 }
puts "all checks passed"
exit 0

#
# virtual_slider process - steady stream of virtual slider samples
#
# Mirrors virtualeyeconf.tcl: a periodic timer publishes the current
# held position to slider/virtual at a fixed rate, so downstream code
# (sliderconf's process_virtual handler, state machines that compute
# derivatives or expect a steady cadence) sees the same structural
# behavior with a virtual slider as with real hardware.
#
# Default state: OFF. Must be explicitly started by a client. Rigs with
# a real slider wired to the ain path never touch this subprocess.
#
# Usage from outside:
#   send virtual_slider "start 4"           ;# 250 Hz stream
#   send virtual_slider "set_slider 2.5 0"  ;# update held position
#   send virtual_slider stop                ;# halt stream
#
package require dlsh
tcl::tm::add $dspath/lib

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

# enable error logging
errormon enable

set virtual_slider(x) 0.0
set virtual_slider(y) 0.0

# load extra modules
set ess_modules "timer"
foreach f $ess_modules {
    load ${dspath}/modules/dserv_${f}[info sharedlibextension]
}

proc start { { interval 4 } } {
    timerTickInterval $interval $interval
    dservSet slider/virtual_enabled 1
}

proc stop {} {
    timerStop
    dservSet slider/virtual_enabled 0
}

proc timer_callback { dpoint data } {
    global virtual_slider
    # DSERV_STRING (type 6) - sliderconf::process_virtual does lassign
    # on the decoded list, so a plain space-separated string is fine.
    dservSetData slider/virtual 0 6 "$virtual_slider(x) $virtual_slider(y)"
}

proc setup {} {
    timerPrefix virtualsliderTimer
    dservRemoveAllMatches
    dservAddExactMatch virtualsliderTimer/0
    dpointSetScript    virtualsliderTimer/0 timer_callback
}

proc set_slider { x {y ""} } {
    global virtual_slider
    set virtual_slider(x) $x
    if { $y ne "" } { set virtual_slider(y) $y }
}

setup
puts "Virtual_slider initialized"

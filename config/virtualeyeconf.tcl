#
# virtual_eye process
#

set dspath [file dir [info nameofexecutable]]

set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

package require dlsh
tcl::tm::add $dspath/lib

set virtual_eye(h) 0.0
set virtual_eye(v) 0.0

# load extra modules
set ess_modules "timer"
foreach f $ess_modules {
    load ${dspath}/modules/dserv_${f}[info sharedlibextension]
}

proc start { { interval 4 } } {
    timerTickInterval $interval $interval
}

proc stop {} {
    timerTickInterval 0 0
}

proc timer_callback { dpoint data } {
    global virtual_eye
    set h [expr {int($virtual_eye(h)*8.0)+2048}]
    set v [expr {2048-int($virtual_eye(v)*8.0)}]

    set ainvals [binary format ss $v $h]
    dservSetData eyetracking/virtual 0 4 $ainvals
}

proc setup {} {
    timerPrefix virtualeyeTimer
    dservRemoveAllMatches
    dservAddExactMatch virtualeyeTimer/0
    dpointSetScript virtualeyeTimer/0 timer_callback
}

proc set_eye { h v } {
    global virtual_eye
    set virtual_eye(h) $h
    set virtual_eye(v) $v
}

setup
puts "virtual_eye subprocess configured"



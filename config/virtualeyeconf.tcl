#
# virtual_eye process - simplified for degrees
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
    dservSet eyetracking/virtual_enabled 1
}

proc stop {} {
    timerStop
    dservSet eyetracking/virtual_enabled 0
}

proc timer_callback { dpoint data } {
    global virtual_eye
    set eyevals [binary format ff $virtual_eye(h) $virtual_eye(v)]
    dservSetData eyetracking/virtual 0 2 $eyevals  ;# type 2 = DSERV_FLOAT
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

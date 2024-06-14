puts "initializing dataserver"

if { [file exists /home/sheinb/src/dserv] } {
    set env(ESS_SYSTEM_PATH) /home/sheinb/src/dserv/systems/incage2
} else {
    set env(ESS_SYSTEM_PATH) /Users/sheinb/src/dserv/systems/incage2
}

tcl::tm::add [file dir $::env(ESS_SYSTEM_PATH)]/pkgs
package require ess

# load extra modules

set path [file dir [info nameofexecutable]]
set dllspec modules/*/*[info sharedlibextension]
foreach f [glob [file join $path $dllspec]] {
    load $f
}

# start analog input if available
catch { ainStart 1 }

dservSet qpcs/ipaddr 127.0.0.1

proc dpointGet { d } { return [dservGet $d] }
proc rpioPinOn { pin } { gpioLineSetValue $pin 1 }
proc rpioPinOff { pin } { gpioLineSetValue $pin 0 }
proc ainSetProcessor { args } {}
proc ainSetParam { p v } { processSetParam "windows" $p $v }
proc ainSetIndexedParam { i p v } {  processSetParam "windows" $p $v $i }
proc ainSetIndexedParams { win args } {
    if { [expr {[llength $args]%2}] } {
	error "wrong number of arguments"
    }
    for { set i 0 } { $i < [llength $args] } { incr i 2 } {
	lassign [lrange $args $i [expr {$i+2}]] p v
	processSetParam "windows" $p $v $win
    }
}
proc ainGetRegionInfo { reg } { processSetParam windows settings 1 $reg }
proc ainGetParam { p } { processGetParam "windows" $p }
proc ainGetIndexedParam { i p } {  processGetParam "windows" $p $i }

gpioLineRequestOutput 26
juicerSetPin 0 27

dservSet rpio/levels/24 0
dservSet rpio/levels/25 0



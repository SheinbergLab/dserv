puts "initializing dataserver"

if { [file exists /home/sheinb/src/dserv] } {
    set env(ESS_SYSTEM_PATH) /home/sheinb/src/dserv/systems/incage2
} else {
    set env(ESS_SYSTEM_PATH) /Users/sheinb/src/dserv/systems/incage2
}

tcl::tm::add [file dir $::env(ESS_SYSTEM_PATH)]/pkgs
package require ess

# load extra modules
set dll_path modules/$tcl_platform(os)/$tcl_platform(machine)
foreach f [glob [file join $dll_path *[info sharedlibextension]]] {
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
gpioLineRequestOutput gpiochip4 26
gpioLineRequestOutput gpiochip4 27
juicerSetPin 0 27

dservSet rpio/levels/24 0
dservSet rpio/levels/25 0


puts "initializing dataserver"

set dspath [file dir [info nameofexecutable]]
if { ![info exists ::env(ESS_SYSTEM_PATH)] } {
    set env(ESS_SYSTEM_PATH) [file join $dspath systems/incage2]
}

tcl::tm::add $dspath/pkgs
package require ess

# load extra modules

set dllspec modules/*/*[info sharedlibextension]
foreach f [glob [file join $dspath $dllspec]] {
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

gpioLineRequestInput 24
gpioLineRequestInput 25
gpioLineRequestOutput 26
juicerSetPin 0 27

dservSet gpio/input/24 0
dservSet gpio/input/25 0

set ports "/dev/ttyUSB0 /dev/cu.usbserial-FTD1906W"
foreach p $ports {
    if [file exists $p] {
	soundOpen $p 
	soundReset
	break
    }
}

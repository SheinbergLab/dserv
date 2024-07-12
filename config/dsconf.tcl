puts "initializing dataserver"

set dspath [file dir [info nameofexecutable]]
if { ![info exists ::env(ESS_SYSTEM_PATH)] } {
    set env(ESS_SYSTEM_PATH) [file join $dspath systems/ess]
}

tcl::tm::add $dspath/pkgs
package require ess

# load extra modules

set dllspec modules/*[info sharedlibextension]
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

proc touchSetProcessor { args } {}
proc touchSetParam { p v } { processSetParam "touch_windows" $p $v }
proc touchSetIndexedParam { i p v } {  processSetParam "touch_windows" $p $v $i }
proc touchSetIndexedParams { win args } {
    if { [expr {[llength $args]%2}] } {
	error "wrong number of arguments"
    }
    for { set i 0 } { $i < [llength $args] } { incr i 2 } {
	lassign [lrange $args $i [expr {$i+2}]] p v
	processSetParam "touch_windows" $p $v $win
    }
}
proc touchGetRegionInfo { reg } { processSetParam "touch_windows" settings 1 $reg }
proc touchGetParam { p } { processGetParam "touch_windows" $p }
proc touchGetIndexedParam { i p } {  processGetParam "touch_windows" $p $i }

gpioOutputInit /dev/gpiochip4
gpioInputInit /dev/gpiochip4
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

proc connect_touchscreen {} {
    set screens [dict create \
		     /dev/input/by-id/usb-wch.cn_USB2IIC_CTP_CONTROL-event-if00 {1024 600} \
                     /dev/input/by-id/usb-ILITEK_ILITEK-TP-event-if00 {1280 800} ]
    dict for { dev res } $screens { 
	if [file exists $dev] {
	    touchOpen $dev {*}$res
	    touchStart
	    break
	}
    }
}

connect_touchscreen
   

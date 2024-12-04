puts "initializing dataserver"

set dspath [file dir [info nameofexecutable]]
if { ![info exists ::env(ESS_SYSTEM_PATH)] } {
    set env(ESS_SYSTEM_PATH) [file join $dspath systems]
}

set dlshlib [file join [file dirname $dspath] dlsh dlsh.zip]
if [file exists $dlshlib] {
    set base [file join [zipfs root] dlsh]
   zipfs mount $dlshlib $base
   set auto_path [linsert $auto_path [set auto_path 0] $base/lib]
}

tcl::tm::add $dspath/lib
package require ess

foreach d "/shared/qpcs/data/essdat /tmp" {
    if { [file exists $d] } {
	set ess::data_dir $d
	break
    }
}

# load extra modules

set dllspec modules/*[info sharedlibextension]
foreach f [glob [file join $dspath $dllspec]] {
    load $f
}

# start analog input if available
catch { ainStart 1 }

proc dpointGet { d } { return [dservGet $d] }
proc rpioPinOn { pin } { gpioLineSetValue $pin 1 }
proc rpioPinOff { pin } { gpioLineSetValue $pin 0 }

proc timerSetScript { id script } {
    set dpoint timer/$id
    dservAddExactMatch $dpoint
    dpointSetScript $dpoint $script
}

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

# make an educated guess about which gpiochip to use
if { $::tcl_platform(os) == "Linux" } {
    if { $::tcl_platform(machine) == "x86_64" } {
	set gpiochip /dev/gpiochip1
    } else {
	if { [file exists /dev/gpiochip4] } {
	    set gpiochip /dev/gpiochip4
	} else {
	    set gpiochip /dev/gpiochip0
	}
    }
} else {
    set gpiochip {}
}

catch { gpioOutputInit $gpiochip }
catch { gpioInputInit $gpiochip }

gpioLineRequestInput 24
gpioLineRequestInput 25
gpioLineRequestOutput 26

catch { juicerInit $gpiochip }
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

proc set_hostinfo {} {
    # target_host allows us to connect using NIC
    set target_host google.com
    
    # set IP addresses for use in stim communication
    dservSet ess/ipaddr 127.0.0.1

    # set host address to identify this machine
    set s [socket $target_host 80]
    dservSet system/hostaddr [lindex [fconfigure $s -sockname] 0]
    close $s

    if { $::tcl_platform(os) == "Darwin" } {
	set name [exec scutil --get ComputerName]
    } elseif { $::tcl_platform(os) == "Linux" } {
	set name [exec hostname]
    } else {
	set name $::env(COMPUTERNAME)
    }
    dservSet system/hostname $name
    dservSet system/os $::tcl_platform(os)
}

connect_touchscreen
set_hostinfo

# connect to battery power circuits
ina226Add 0x45 system 12v
ina226Add 0x44 system 24v

# start up subprocess to store trials in either postgresql or sqlite3 db
set host [dservGet system/hostaddr]

# start sqlite local db
subprocess 2571 [file join $dspath config/sqliteconf.tcl]

# homebase computers use postgresql
set hbs "192.168.4.100 192.168.4.101 192.168.5.30"
if { [lsearch $hbs $host] >= 0 } {
    subprocess 2572 [file join $dspath config/postgresconf.tcl]
}

# and finally load a default system
ess::load_system

# set initial subject
ess::set_subject human


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
catch { juicerInit $gpiochip }

############################### GPIO ##############################
###
### can put lines like these in local/pins.tcl, e.g.
###   gpioLineRequestInput  24
###   gpioLineRequestInput  25
###   gpioLineRequestOutput 26

###   juicerSetPin 0 27

###
### joystick support through 4 GPIO lines
###

### in pins.tcl select lines that are used by, e.g.:
###    dservSet joystick/lines { 1 12 2 13 4 18 8 17 }
### which would set 12->1 (up)
###                 13->2 (down)
###                 18->4 (left)
###                 17->8 (right)
### If:
###    joystick/value == 2 then joystick is down
###    joystick/value == 9 then joystick is up-right

proc joystick_callback { dpoint data } {
    set jlines [dservGet joystick/lines]
    set jval 0
    dict for { k v } $jlines {
	set jval [expr $jval | $k*[dservGet gpio/input/$v]]
    }
    dservSet joystick/value $jval
}

proc joystick_button_callback { dpoint data } {
    dservSet joystick/button $data
}

proc joystick_init { } {
    if { ![dservExists joystick/lines] } { return }
    
    dict for { k p } [dservGet joystick/lines] {
	gpioLineRequestInput $p BOTH 2500 PULL_UP ACTIVE_LOW
    }
    
    dict for { k p } [dservGet joystick/lines] {
	dservAddMatch gpio/input/$p
	dpointSetScript gpio/input/$p joystick_callback
    }

    joystick_callback init 0

    if { [dservExists joystick/button_line] } {
	set pin [dservGet joystick/button_line]
	gpioLineRequestInput $pin BOTH 2500 PULL_UP ACTIVE_LOW
	dservAddMatch gpio/input/$pin
	dpointSetScript gpio/input/$pin joystick_button_callback
	joystick_button_callback init 0
    }
}


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
subprocess 2571 "source [file join $dspath config/sqliteconf.tcl]"

# homebase computers use postgresql
set hbs "192.168.4.100 192.168.4.101 192.168.4.102 192.168.4.201"
if { [lsearch $hbs $host] >= 0 } {
    subprocess 2572 "source [file join $dspath config/postgresconf.tcl]"
}

# and finally load a default system
ess::load_system

# set initial subject
ess::set_subject human

# look for any .tcl configs in local/*.tcl
foreach f [glob [file join $dspath local *.tcl]] {
    source $f
}

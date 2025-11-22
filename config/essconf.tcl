#
# ess process for running experiments
#

set dspath [file dir [info nameofexecutable]]

set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

package require dlsh
package require qpcs
package require sqlite3
package require yajltcl

tcl::tm::add $dspath/lib

# initialize ip addr datapoint
dservSet ess/ipaddr ""

# load extra modules
set ess_modules \
    "ain eventlog gpio_input gpio_output \
    joystick4 rmt sound timer touch usbio"
foreach f $ess_modules {
    load ${dspath}/modules/dserv_${f}[info sharedlibextension]
}

# look for any .tcl configs in local/*.tcl
foreach f [glob [file join $dspath local pre-*.tcl]] {
    source $f
}

# now ready to start ess
if { ![info exists ::env(ESS_SYSTEM_PATH)] } {
    set env(ESS_SYSTEM_PATH) [file join $dspath systems]
}

package require ess

foreach d "/shared/qpcs/data/essdat" {
    if { [file exists $d] } {
	set ess::data_dir $d
	break
    }
}

# start analog input if available
catch { ainStart 1 }
# if we didn't find an A/D still setup 2 channels for virtual inputs
ainSetNchan 2

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

#
# Sampler processor convenience functions
#
proc samplerSetParam { p v } { processSetParam "sampler" $p $v }
proc samplerSetIndexedParam { i p v } { processSetParam "sampler" $p $v $i }
proc samplerGetParam { p } { processGetParam "sampler" $p }
proc samplerGetIndexedParam { i p } { processGetParam "sampler" $p $i }

# Basic control
proc samplerStart { {slot 0} } { processSetParam "sampler" start 1 $slot }
proc samplerStop { {slot 0} } { processSetParam "sampler" stop 1 $slot }
proc samplerSetActive { slot active } { processSetParam "sampler" active $active $slot }

# Query functions
proc samplerQueryRate { {slot 0} } { processSetParam "sampler" rate 1 $slot }
proc samplerGetStatus { {slot 0} } {
    processSetParam sampler status 1 0
    return [dservGet proc/sampler/status]
}
proc samplerGetVals { {slot 0} } {
    return [dservGet proc/sampler/vals]
}
proc samplerGetCount { {slot 0} } {
    processSetParam sampler count 1 0
    return [dservGet proc/sampler/count]
}
proc samplerGetRate { {slot 0} } {
   return [dservGet proc/sampler/rate]
}

# Rate tracking
proc samplerEnableRateTracking { {slot 0} {interval 50} } {
    processSetParam "sampler" track_rate 1 $slot
    processSetParam "sampler" rate_update_interval $interval $slot
}
proc samplerDisableRateTracking { {slot 0} } {
    processSetParam "sampler" track_rate 0 $slot
}

# Configuration - sample count mode (original behavior)
proc samplerConfigure { slot nsamples nchannels {operation 0} } {
    processSetParam "sampler" sample_count $nsamples $slot
    processSetParam "sampler" nchannels $nchannels $slot
    processSetParam "sampler" operation $operation $slot
    processSetParam "sampler" use_time_window 0 $slot
}

# Configuration - time window mode (new)
proc samplerConfigureTime { slot time_window nchannels {operation 0} } {
    processSetParam "sampler" time_window $time_window $slot
    processSetParam "sampler" nchannels $nchannels $slot
    processSetParam "sampler" operation $operation $slot
    processSetParam "sampler" use_time_window 1 $slot
}

# Loop mode control
proc samplerSetLoop { slot enable } {
    processSetParam "sampler" loop $enable $slot
}

proc detect_board_type {} {
    if { $::tcl_platform(os) != "Linux" } {
        return "unknown"
    }
    
    if { $::tcl_platform(machine) == "x86_64" } {
        return "x86_64"
    }
    
    # Try to read device tree model
    set model ""
    if { [file exists /sys/firmware/devicetree/base/model] } {
        catch {
            set fp [open /sys/firmware/devicetree/base/model r]
            set model [read $fp]
            close $fp
            set model [string trim $model "\x00"]
        }
    }
    
    # Match board types
    if { [string match "*Raspberry Pi 5*" $model] } {
        return "rpi5"
    } elseif { [string match "*Raspberry Pi 4*" $model] } {
        return "rpi4"
    } elseif { [string match "*Raspberry Pi*" $model] } {
        return "rpi"
    } elseif { [string match "*BeaglePlay*" $model] } {
        return "beagleplay"
    } elseif { [string match "*BeagleY-AI*" $model] || [string match "*BEAGLEY-AI*" $model] } {
        return "beagley-ai"
    } elseif { [string match "*PocketBeagle*" $model] } {
        return "pocketbeagle"
    } elseif { [string match "*BeagleBone*" $model] } {
        return "beaglebone"
    }
    
    return "unknown"
}

# GPIO chip mapping by board type
set gpio_chip_map {
    x86_64       /dev/gpiochip1
    rpi5         /dev/gpiochip4
    rpi4         /dev/gpiochip0
    rpi          /dev/gpiochip0
    beagleplay   /dev/gpiochip2
    beagley-ai   /dev/gpiochip2
    pocketbeagle /dev/gpiochip2
    beaglebone   /dev/gpiochip0
    unknown      /dev/gpiochip0
}

set board_type [detect_board_type]
set gpiochip [dict get $gpio_chip_map $board_type]

puts "Detected board: $board_type, using GPIO chip: $gpiochip"

catch { gpioOutputInit $gpiochip }
catch { gpioInputInit $gpiochip }

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


#
# callback for Mikroe Joystick4 click
#
proc joystick4_callback { dpoint data } {
    if { $data } {
	# reading clears the interrupt and pin falls
	# up=1   down=2 left=4  right=8
	# ul=5   ur=9   dl=6    dr=10
	lassign [joystick4Read] position button
    	dservSet joystick/value $position
    	dservSet joystick/button $button
    }
}

proc joystick_init { } {
    if { [dservExists joystick4/interrupt] } {
	set p [dservGet joystick4/interrupt]

	# the mikroe joystick 4 signals change on rising edge
	gpioLineRequestInput $p RISING
	dservAddMatch gpio/input/$p
	dpointSetScript gpio/input/$p joystick4_callback
	dservSet joystick/value 0
	dservSet joystick/button 0
    }
    
    if { [dservExists joystick/lines] } { 
	dict for { k p } [dservGet joystick/lines] {
	    gpioLineRequestInput $p BOTH 2500 PULL_UP ACTIVE_LOW
	}
	
	dict for { k p } [dservGet joystick/lines] {
	    dservAddMatch gpio/input/$p
	    dpointSetScript gpio/input/$p joystick_callback
	}
	dservSet joystick/value 0
    }
    if { [dservExists joystick/button_line] } {
	set pin [dservGet joystick/button_line]
	gpioLineRequestInput $pin BOTH 2500 PULL_UP ACTIVE_LOW
	dservAddMatch gpio/input/$pin
	dpointSetScript gpio/input/$pin joystick_button_callback
	dservSet joystick/button 0
    }
}


###
### setup sound (try hardware over serial then fluidsynth
###
set ports "/dev/ttyUSB0 /dev/cu.usbserial-FTD1906W"
foreach p $ports {
    if [file exists $p] {
	soundOpen $p 
	soundReset
	break
    }
}
set soundfiles {{YAMAHA S-YXG50_0.2.1.2.sf2} {FluidR3_GM.sf2}}
foreach sf $soundfiles {
    set sfile  [file join /usr/local/dserv/soundfonts $sf]
    if { [file exists $sfile] } {
	soundInitFluidSynth $sfile
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

# and finally load a default system
ess::load_system emcalib

# set initial subject
ess::set_subject human

# look for any .tcl configs in local/*.tcl
foreach f [glob [file join $dspath local post-*.tcl]] {
    source $f
}

puts "ESS thread configured"


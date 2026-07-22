#
# ess process for running experiments
#
package require dlsh
package require qpcs
package require sqlite3
package require yajltcl

tcl::tm::add $dspath/lib

# enable error logging
errormon enable

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

# load extra modules
# (The `ain` module is owned by the dedicated `ain` subprocess — see
# config/ainconf.tcl. Touchscreen/trackpad reading is owned by the
# dedicated `input` subprocess — see config/inputconf.tcl. State systems
# subscribe to mtouch/event via ::ess::touch_win_set as before.)
set ess_modules \
    "eventlog gpio_input gpio_output \
    joystick4 rmt timer"
foreach f $ess_modules {
    load ${dspath}/modules/dserv_${f}[info sharedlibextension]
}

# now ready to start ess
if { ![info exists ::env(ESS_SYSTEM_PATH)] } {
    set env(ESS_SYSTEM_PATH) [file join $dspath systems]
}

# initialize ip addr datapoint
if {[info exists ::env(ESS_IPADDR)]} {
    dservSet ess/ipaddr $::env(ESS_IPADDR)
} else {
    dservSet ess/ipaddr ""
}

package require ess
package require ess_registry
package require ess_validation
package require ess_sync
package require settingsdb   ;# persist stable per-box settings across restarts

ess::registry::init_from_dserv

# Per-box settings store for the ess subprocess (separate file from em's
# calibration.db to avoid cross-thread sqlite contention). Restore the
# persisted sound settings (feedback mute + master gain) so they survive
# restarts; sound_init re-applies them to the synth on each system load.
settingsdb::init [file join $dspath db settings.db]
ess::sound_restore_settings

# Convenience control aliases so bare verbs work over the wire,
# e.g. "dservctl ess reset" instead of "dservctl ess ess::reset".
# start/stop can be added the same way if desired.
proc reset {} { ess::reset }

# Configure registry from environment or defaults
if {[info exists ::env(ESS_REGISTRY_URL)]} {
    ess::registry::configure -url $::env(ESS_REGISTRY_URL)
}
if {[info exists ::env(ESS_WORKGROUP)]} {
    ess::registry::configure -workgroup $::env(ESS_WORKGROUP)
}

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
    } elseif { [string match "*Orange Pi 5 Plus*" $model] } {
        return "orangepi5plus"
    } elseif { [string match "*Orange Pi 5*" $model] || [string match "*OrangePi 5*" $model] } {
        return "orangepi5"
    } elseif { [string match "*Orange Pi*" $model] || [string match "*OrangePi*" $model] } {
        return "orangepi"
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
    orangepi5    /dev/gpiochip3
    orangepi5plus /dev/gpiochip3
    orangepi     /dev/gpiochip0
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

# Touchscreen discovery has moved to the `input` subprocess
# (config/inputconf.tcl). Rigs that previously relied on the hardcoded
# USB paths above should declare screen dimensions and expectations in
# local/input.tcl, e.g.:
#   inputConfigure touchscreen -screen_w 1280 -screen_h 800
#   inputExpect touchscreen

# Defer the registry script sync OFF the boot path. ess::sync_base does ~1 HTTP
# round-trip per system (~12) to the registry (~6s) and is the last thing
# essconf runs, so it blocks dserv startup (dsconf waits on this subprocess's
# source). Instead arm a one-shot timer (timer module already loaded) to run it
# ~4s after boot. Uses a HIGH timer index (7) + the DEFAULT "timer" prefix so it
# can't collide with the state system's timers (low indices, poll-based via
# timerExpired, and armed only during runs, which start much later). One-shot:
# disarm in the callback. The rig boots on its on-disk (last-sync) scripts;
# updates land seconds later and apply on the next system load.
proc _deferred_sync_base {dpoint data} {
    catch { timerStop 7 }
    if { ![info exists ess::registry_url] } { return }
    if {[catch {ess::sync_base} r]} {
        puts stderr "ess: deferred sync_base failed: $r"
    } else {
        puts stderr "ess: deferred sync_base complete ($r)"
    }
    flush stderr
}
if { [info exists ess::registry_url] } {
    dservAddExactMatch timer/7
    dpointSetScript timer/7 _deferred_sync_base
    timerTickInterval 7 4000 4000
}

# source rig-local configs (local/post-*.tcl) BEFORE the default system
# loads: rig-level bindings (button_bind/joystick_bind) must exist for the
# boot load's *_init calls to pick them up, and any legacy activation a
# post script does (direct *_init) gets cleared by the load's input_reset
# -- so the input panels reflect the loaded system from the very first
# page hit, not whatever the rig script switched on.
foreach f [glob -nocomplain [file join $dspath local post-*.tcl]] {
    source $f
}

# and finally load a default system
ess::load_system emcalib

# Pull the workgroup's subject list from the registry. This OVERRIDES the
# env/hardcoded default that ess-2.0.tm set during load (which stays the
# fallback); if the registry is unset/unreachable/empty the default remains.
# Registry url+workgroup were configured above via ess::registry::configure.
# Bare verb so the Workbench (or "dservctl ess refresh_subjects") can re-pull;
# the logic itself lives in the ess package (ess::refresh_subjects).
proc refresh_subjects {} { ess::refresh_subjects }
refresh_subjects

# set initial subject
ess::set_subject human

puts "ESS thread configured"



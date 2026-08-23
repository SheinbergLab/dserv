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
# (Host-side analog input is RETIRED -- the extio boxes digitize and publish
# state/ain/<group> themselves. Touchscreen/trackpad reading is owned by the
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
package require settings     ;# `ess boot_system` is DECLARED (see the load below)

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
# pin < 0 means "no host pin" -- the declared `ess obs_pin -1` norm on rigs
# whose extio box owns the obs line. These sit on the BEGINOBS/ENDOBS path,
# so they must be no-ops there, never errors.
proc rpioPinOn { pin } { if {$pin < 0} return; gpioLineSetValue $pin 1 }
proc rpioPinOff { pin } { if {$pin < 0} return; gpioLineSetValue $pin 0 }

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
# Read the datapoint; do NOT ask the processor to republish it.
#
# The sampler maintains proc/sampler/status itself -- it publishes 0 from the
# start handler and sets status_pending on completion, flushed on the next
# input sample -- so forcing a republish adds nothing. It also FEEDS BACK:
# ess wires dpointSetScript proc/sampler/status -> do_update, so every poll
# published a datapoint that woke the state machine, which re-ran the
# sample_position transition, which polled again. Measured on the rig
# 2026-08-06: 310,115 status publishes across 18 trials (17,228 per trial,
# ~30k/s inside the sample windows), each one running the whole state machine.
# That spin starved the sampler's own input -- eyetracking/raw was delivered at
# 246 Hz but only 44 samples landed in a 400 ms window -- which is what made
# count-based sampling miss its deadline and skip store_calibration.
proc samplerGetStatus { {slot 0} } {
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
    } elseif { [string match "*FRDM-IMX93*" $model] } {
        return "imx93"
    } elseif { [string match "*FRDM-IMX95*" $model] } {
        return "imx95"
    }

    return "unknown"
}

# GPIO chip mapping by board type
#
# imx93 -> gpiochip0 is a native SoC GPIO bank (not the onboard PCAL6524 I2C
# expander -- that has USER_BTN1/USER_BTN2 on lines 5/6, but this rig gets its
# buttons from extio boxes instead, so they're unused here). Confirmed
# empirically with an LED HAT (no schematic was available) that gpiochip0
# offset N drives 40-pin header net GPIO_IOn directly -- the obs-sync line
# this is actually used for is the declared `ess obs_pin` (see below).
#
# imx95 (FRDM-IMX95) mirrors it: gpiochip0 is gpio@43810000, the bank whose
# pads are named GPIO_IO00..37 (the 40-pin header nets); gpiochips 4/5 are
# the I2C expanders. Same offset-N == GPIO_IOn convention as the imx93.
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
    imx93        /dev/gpiochip0
    imx95        /dev/gpiochip0
    unknown      /dev/gpiochip0
}

set board_type [detect_board_type]
set gpiochip [dict get $gpio_chip_map $board_type]

puts "Detected board: $board_type, using GPIO chip: $gpiochip"

catch { gpioOutputInit $gpiochip }
catch { gpioInputInit $gpiochip }

############################### GPIO ##############################
###
### Pin routing is DECLARED now (local/rig.tcl via the settings gear):
###   setting ess obs_pin 26         obs sync output (claimed below)
###   setting button 2 gpio:17       button lines (transport claims at
###                                  activation -- ess_transports)
###   setting juicer gpio_pin 27     juice valve
### A legacy local/post-pins.tcl still works, e.g.
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

# The deferred boot-time registry sync now runs in the dedicated `scripts`
# subprocess (config/scriptsconf.tcl → scripts::initial_sync), keeping the
# registry HTTP off this interp entirely. The rig still boots on its
# on-disk (last-sync) scripts; updates land seconds later and apply on the
# next system load. ess::sync_base remains available here for manual use.

# source rig-local configs (local/post-*.tcl) BEFORE the default system
# loads: rig-level bindings (button_bind/joystick_bind) must exist for the
# boot load's *_init calls to pick them up, and any legacy activation a
# post script does (direct *_init) gets cleared by the load's input_reset
# -- so the input panels reflect the loaded system from the very first
# page hit, not whatever the rig script switched on.
foreach f [glob -nocomplain [file join $dspath local post-*.tcl]] {
    source $f
}

#
# Rig-level remote compute host, DECLARED. Generalizes what
# local/post-remotecompute.tcl did for planko alone: the host is a rig fact,
# so it lives in local/rig.tcl beside the others, and the system package
# reads it when it loads (planko-3.0.tm, bottom). Declared AFTER the post-*
# sourcing above so a declaration wins over a legacy set_compute_host call,
# and BEFORE the boot-system load below so the package's load-time read
# finds it.
#
proc compute_host_norm {v} {
    set v [string trim $v]
    if { $v eq "" } { return "" }            ;# compute locally -- the default
    if { [regexp {^[a-zA-Z][a-zA-Z0-9+.-]*://} $v] } {
        error "ess compute_host: '$v' is a URL -- give the host alone\
 (remoteEval talks to dserv on port 2560)"
    }
    if { [regexp {\s} $v] } {
        error "ess compute_host: '$v' contains whitespace"
    }
    return $v
}

proc compute_host_apply {v} {
    set v [string trim $v]
    # Only a LOADED planko can be told -- an unloaded one reads the setting
    # itself when its package loads. Prefer the probing setter so an
    # unreachable host degrades to local compute with a journal line rather
    # than erroring the next trial generation; plain set_compute_host is the
    # fallback for a package version without the probe, and the way to say
    # "" = back to local.
    if { $v ne "" &&
         [llength [info commands ::planko::set_compute_host_if_available]] } {
        planko::set_compute_host_if_available $v
    } elseif { [llength [info commands ::planko::set_compute_host]] } {
        planko::set_compute_host $v
    }
}

settings::declare ess compute_host -default "" \
    -validate ::compute_host_norm \
    -apply    ::compute_host_apply \
    -doc "a remote dserv/ess box that compute-heavy systems offload
generation to (planko board physics today; remoteEval against its
port 2560). EMPTY -- the default -- computes locally. The system
package reads it when it loads; a live change reaches a loaded
planko immediately, probing reachability first, so an unreachable
host falls back to local compute with a journal line. Replaces
planko::set_compute_host in local/post-remotecompute.tcl."

# One-time carryover: a rig whose local/post-remotecompute.tcl (sourced
# just above) already pointed planko at a compute host gets that value
# adopted into rig.tcl; after that the declaration is the single home and
# the file is deletable (rig_settings warns while it remains). Already
# declared? Then push it over whatever the legacy file just set.
if { [settings::source_of ess compute_host] eq "default" } {
    set _legacy ""
    catch { set _legacy [string trim $::planko::compute_host] }
    if { $_legacy ne "" } {
        if { [catch { settings::put ess compute_host $_legacy -persist } _e] } {
            puts stderr "ess: compute host not adopted into local/rig.tcl: $_e"
        } else {
            puts "ess: compute host adopted into local/rig.tcl ($_legacy)"
        }
    }
} elseif { [catch { compute_host_apply [settings::get ess compute_host] } _e] } {
    puts stderr "ess: compute_host declaration could not be applied: $_e"
}
unset -nocomplain _legacy _e

#
# The obs sync OUTPUT pin, declared. This was the last unique job of
# local/post-pins.tcl: gpio BUTTON lines are claimed by the button
# transport at activation (`setting button 2 gpio:17`, ess_transports),
# the juicer pin is `juicer gpio_pin`, so the board-cased pin file
# reduces to this one fact.
#
# NO adoption from a legacy file, deliberately: ess-2.0.tm compiles in
# obs_pin 26 (an rpi default), so a file's `set_obs_pin 26` is
# indistinguishable from nobody having said anything -- and the rigs
# that used host obs pins have mostly moved to extio-owned obs lines,
# where adopting 26 would be adopting the WRONG thing. Declare to take
# control; declared -1 is a positive statement -- "no host obs pin, the
# box owns the line" -- which is what closes the dual-driven-obs gap.
# Undeclared keeps the legacy behavior (compiled default + whatever a
# local/post-pins.tcl still does) untouched.
#
settings::declare ess obs_pin -default -1 -type int \
    -doc "host GPIO line driven as the obs sync output (claimed at boot,
then ::ess::set_obs_pin). -1 -- the default and the norm -- means
none: rigs with an extio box let the box own the obs line.
Old board defaults from local/post-pins.tcl: rpi/rpi4/rpi5/
beagley-ai 26, orangepi5 24 (bank gpio3), beagleplay/pocketbeagle
46. Takes effect at the next restart (a claimed line cannot be
re-claimed live)."

if { [settings::source_of ess obs_pin] ne "default" } {
    set _op   [settings::get ess obs_pin]
    set _cur  -1
    catch { set _cur [set ::ess::obs_pin] }
    if { $_op >= 0 && $_op != $_cur } {
        if { [catch { gpioLineRequestOutput $_op } _e] } {
            puts stderr "ess: declared obs_pin $_op could not be claimed\
 ($_e) -- if local/post-pins.tcl still claims a pin, delete the file"
        } else {
            ::ess::set_obs_pin $_op
            puts "ess: obs sync output on GPIO $_op (declared)"
        }
    } elseif { $_op >= 0 } {
        # Same pin a legacy file just claimed and set: nothing to redo.
        puts "ess: obs sync output on GPIO $_op (declared; already in force)"
    } else {
        # Declared NONE: eventing must not drive a host pin even though
        # ess-2.0.tm compiles in 26. A legacy file's claim (if any) is
        # released only by restarting without the file.
        ::ess::set_obs_pin -1
        puts "ess: obs sync output declared none -- extio owns the obs line"
    }
    unset -nocomplain _op _cur _e
}

#
# and finally load the rig's boot system.
#
# `emcalib` was hardcoded here, pointing into the systems tree that ships
# with dserv -- so a rig whose ESS_SYSTEM_PATH holds something else, or
# nothing yet, hit a load failure at boot. Declared instead, and empty is a
# real answer: a fresh box with no systems yet should come up idle, not
# broken.
#
# NON-FATAL, which is the point. load_system reports every failure path and
# then RE-RAISES, and this file is a subprocess config -- so an unloadable
# boot system used to abort the rest of essconf, taking refresh_subjects and
# set_subject with it. The rig then had no system AND no subject, which
# looks like a much deeper failure than "the named system did not load".
#
settings::declare ess boot_system -default "" \
    -candidates system \
    -doc "what to load at boot: a system name, or `system protocol variant`.
EMPTY (the default) means follow the rig: whatever it last loaded
cleanly, else the stock bootstrap system. Declare one to pin it --
a rig that must always come up on the same task should say so
rather than depend on what someone last opened."

#
# WHAT THIS RIG LAST RAN is LEARNED, not declared, so it lives in the
# calibration db beside the other learned things -- and it is why a rig that
# adopts the real system tree does not have to be told twice. Load planko
# once, cleanly, and the next boot comes up on planko.
#
# ess-2.0.tm keeps last_good in memory, publishes ess/last_good_system and --
# guarded on settingsdb being present -- saves it. It is written there, at
# the point the fact is produced, rather than watched from here: a datapoint
# an interp sets itself does not come back to that interp as an event -- and
# more decisively, ::ess::init clears this interp's datapoint scripts on
# every system load, so a binding made once at config time is gone the first
# time anyone loads anything. Verified: bind, load, and dpointScripts no
# longer lists it.
#
# The ladder, most specific first. Each rung is checked for EXISTENCE, so a
# system that has been removed (or a db remembering one from another rig's
# tree) falls through instead of failing the boot.
proc ess_boot_target {} {
    set declared [string trim [::settings::get ess boot_system]]
    if { $declared ne "" } { return [list $declared declared] }

    set stored ""
    catch { set stored [dict get [::settingsdb::load ess default] last_good] }
    if { [dict exists $stored system] } {
        set s [dict get $stored system]
        if { [file isdirectory [file join $::ess::system_path ess $s]] } {
            set t $s
            foreach k {protocol variant} {
                if { [dict exists $stored $k] && [dict get $stored $k] ne "" } {
                    lappend t [dict get $stored $k]
                }
            }
            return [list $t "last loaded cleanly here"]
        }
    }

    if { [file isdirectory [file join $::ess::system_path ess blinky]] } {
        return [list blinky "the stock bootstrap system"]
    }

    # ANY system beats none.
    #
    # blinky only exists in dserv's own shipped systems/ dir, so on a rig
    # whose ESS_SYSTEM_PATH points at its real tree that rung cannot fire --
    # which is how rpi500 reached "nothing to load" on the first boot after
    # an update (nothing declared, last_good not yet recorded, no blinky in
    # /home/sheinb/systems). Coming up empty is worse than coming up on an
    # arbitrary system: a loaded system is inspectable, its panels populate,
    # and the rest of the ESS config -- subject, datafile hooks -- runs
    # against something real.
    #
    # `system_candidates` rather than a fresh glob, so "what counts as a
    # loadable system" has one definition: it already skips `lib` and the
    # project-named leftover directory, and marks a directory with no
    # <name>.tcl unselectable. First alphabetically, which is arbitrary but
    # stable -- a rig that cares declares `ess boot_system`.
    # The catch covers the ENUMERATION only. Wrapping the loop in it hid the
    # `return` -- catch traps TCL_RETURN the same as an error, so the proc
    # ran on to "nothing to load" and this rung silently never fired.
    set cands {}
    catch { set cands [::ess::system_candidates] }
    foreach c $cands {
        if { ![dict exists $c selectable] || ![dict get $c selectable] } continue
        set s [dict get $c route]
        if { $s eq "" } continue
        return [list $s "first system in the tree -- nothing was declared and\
 this rig has no last-good yet"]
    }
    return [list "" "nothing to load"]
}

lassign [ess_boot_target] _boot _why
if { $_boot ne "" } {
    puts "essconf: loading '$_boot' ($_why)"
    if { [catch { ess::load_system {*}$_boot } _err] } {
        puts stderr "essconf: boot system '$_boot' did not load: $_err"
        catch { dservSet ess/boot_error \
                    "boot system '$_boot' ($_why) did not load: $_err" }
    } else {
        catch { dservSet ess/boot_error "" }
    }
} else {
    puts "essconf: no system loaded ($_why)"
    catch { dservSet ess/boot_error "" }
}
unset -nocomplain _boot _why _err

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



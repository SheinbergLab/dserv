# JSON support
package require yajltcl

tcl::tm::add $dspath/lib
package require settings

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

# enable error logging
errormon enable

### example usage:
# set j [Juicer new /dev/ttyACM0]
# $j open
# $j reward 0.75
# $j close
#

# /*
#  * Available API Commands:
#  *
#  * 1. Set Commands:
#  *    - {"set": {"flow_rate": <float>}}
#  *      - Sets the flow rate (must be > 0).
#  *    - {"set": {"purge_vol": <float>}}
#  *      - Sets the purge volume (must be > 0).
#  *    - {"set": {"target_rps": <float>}} {"set": {"target_rps": 3}}
#  *      - Sets the target revolutions per second (must be > 0 and <= MAX_RPS).
#  *    - {"set": {"voltage_mult": <float>}} {"set": {"voltage_mult": 11.909}}
#  *      - Sets the multiplier to account for the voltage divider ratio
#  *
#  * 2. Do Commands (only 1 allowed per request):
#  *    - {"do": "abort"}
#  *      - Aborts the current operation, stops the pump, and updates reward metrics.
#  *    - {"do": {"reward": <float>}} {"do": {"reward": 0.8}}
#  *      - Dispenses a reward of the specified amount (must be > 0).
#  *    - {"do": {"purge": <float>}}
#  *      - Starts a purge operation for the specified amount (must be > 0).
#  *    - {"do": {"calibration": {"n": <int>, "on": <int>, "off": <int>}}} {"do": {"calibration": {"n": 10, "on": 500, "off": 500}}}
#  *      - Runs a calibration cycle with:
#  *        - n: Number of cycles (must be > 0).
#  *        - on: Duration in milliseconds for the pump to be ON (must be > 0).
#  *        - off: Duration in milliseconds for the pump to be OFF (must be > 0).
#  *
#  * 3. Get Commands:
#  *    - {"get": ["flow_rate"]}
#  *      - Retrieves the current flow rate.
#  *    - {"get": ["purge_vol"]}
#  *      - Retrieves the current purge volume.
#  *    - {"get": ["target_rps"]}
#  *      - Retrieves the current target revolutions per second.
#  *    - {"get": ["reward_mls"]}
#  *      - Retrieves the total amount of reward dispensed in milliliters.
#  *    - {"get": ["reward_number"]}
#  *      - Retrieves the total number of rewards dispensed.
#  *    - {"get": ["juice_level"]}
#  *      - Retrieves the current juice reservoir level (e.g. ">50mLs" or "<50mLs").
#  *    - {"get": ["pump_voltage"]}
#  *      - Retrieves the current voltage supplied to the pump
#  *    - {"get": ["voltage_mult"]}
#  *      - Retrieves the multiplier to account for the voltage divider ratio
#  *    - {"get": ["charging"]}
#  *      - Retrieves the status of whether the battery is charging or not
#  *    - {"get": ["<unknown_parameter>"]}
#  *      - Returns "Unknown parameter" for any unrecognized parameter.
#  *
#  * Example Combined Request:
#  *    {"set": {"target_rps": 2, "flow_rate": 0.65}, "do": {"reward": 1},"get": ["reward_mls", "reward_number"]}
#  *
#  * Expected Response:
#  *    {
#  *      "status": "success",
#  *      "reward_mls": <float>,
#  *      "reward_number": <int>
#  *    }
#  */


set ::juicer_status_fields {juice_level reward_mls reward_number}

proc publish_juicer_fields {parsed {keys ""}} {
    if {$keys eq ""} {
	set keys $::juicer_status_fields
    }
    foreach key $keys {
	if {![dict exists $parsed $key]} {
	    continue
	}
	set value [dict get $parsed $key]
	dservSet juicer/${key} $value
    }
}

proc publish_juicer_response {response} {
    if {$response eq ""} {
	return
    }
    if {[catch {
	publish_juicer_fields [::yajl::json2dict $response]
    } err]} {
	puts stderr "Juicer publish error: $err"
    }
}

# load juicer module
set ess_modules "juicer"
foreach f $ess_modules {
    load ${dspath}/modules/dserv_${f}[info sharedlibextension]
}

catch { Juicer destroy }

oo::class create Juicer {
    variable _fd
    variable _path /dev/ttyACM0
    variable _backend
    variable _extio_target
    variable _timeout_ms

    constructor { { path {} } } {
	set _fd -1
	set _path $path
	set _backend ""
	set _extio_target {}
	set _timeout_ms 10000   ;# bound the pump-reply wait (covers a large dispense/purge)
    }

    destructor { my close }

    method set_path { path } { set _path $path }
    method path {} { return $_path }
    method set_timeout { ms } { set _timeout_ms $ms }
    method open {} {
	# NOCTTY: a systemd service is a session leader with no controlling
	# terminal, so opening a tty WITHOUT this makes the pump our ctty (it
	# showed up as dserv's TT=ttyACM0 + made a pump hangup able to SIGHUP
	# dserv). NONBLOCK: don't block on modem lines at open, and enable the
	# timed read in do_cmd. Same flags modules/usbio.c already uses.
	set _fd [open $_path {RDWR NOCTTY NONBLOCK}]
	fconfigure $_fd -buffering line -translation lf
    }
    method is_open {} { return [expr {$_fd != -1}] }

    # The bound reward route: usb | extio | gpio | none. Set only by
    # juicer_bind, which resolves the DECLARED `juicer destination` setting
    # against what is live -- presence alone never decides.
    method set_backend {kind {target {}}} {
	set _backend $kind
	set _extio_target $target
	dservSet juicer/backend $kind
	switch -- $kind {
	    usb     { dservSet juicer/target $_path }
	    extio   { dservSet juicer/target [join $target /] }
	    gpio    { dservSet juicer/target host }
	    default { dservSet juicer/target "" }
	}
    }
    method backend {} { return $_backend }
    method extio_target {} { return $_extio_target }

    # compat shims (pre-backend API; local/juicer.tcl on old rigs)
    method use_gpio {} { my set_backend gpio }
    method using_gpio {} { return [expr {$_backend eq "gpio"}] }

    method find {} {
	set model_name juicer
	set devices [glob -nocomplain /dev/ttyACM* /dev/ttyUSB*]
	foreach dev $devices {
	    if {[file exists $dev]} {
		set info [exec udevadm info --query=all --name=$dev 2>/dev/null]
		if {[string match *ID_MODEL=${model_name}* $info]} {
		    return $dev
		}
	    }
	}
	return ""
    }

    method close {} {
	if { $_fd != -1 } {
	    close $_fd
	    set _fd -1
	}
    }
    # Send a command and read the pump's one-line reply with a BOUNDED wait.
    # The old `gets $_fd` was a blocking read with no timeout: a pump that
    # started a dispense then stopped answering (hang / disconnect / a reply
    # stolen by another opener) blocked here forever -- and because ess calls
    # this via a SYNCHRONOUS `send juicer`, that froze all of dserv with the
    # valve open (2026-07-09). Now: non-blocking poll to a deadline; on timeout,
    # fire an abort so a started dispense can't run away, and raise an error the
    # caller can log rather than hang on.
    method do_cmd { cmd } {
	if { $_fd < 0 } { return }
	# write blocking (small command; the OS tty buffer takes it) so it
	# definitely goes out even though the channel is otherwise non-blocking.
	fconfigure $_fd -blocking 1
	if { [catch { puts $_fd $cmd; flush $_fd } werr] } {
	    catch { fconfigure $_fd -blocking 0 }
	    error "juicer: write failed: $werr"
	}
	fconfigure $_fd -blocking 0
	set deadline [expr {[clock milliseconds] + $_timeout_ms}]
	while { [clock milliseconds] < $deadline } {
	    if { [gets $_fd line] >= 0 } { return $line }   ;# complete reply line
	    if { [eof $_fd] } { error "juicer: port closed (EOF)" }
	    after 10   ;# nothing yet; brief pause, bounded by the deadline
	}
	# timed out -> stop the pump so a started dispense can't keep flowing
	catch { fconfigure $_fd -blocking 1; puts $_fd {{"do": "abort"}}; flush $_fd; fconfigure $_fd -blocking 0 }
	error "juicer: no reply within ${_timeout_ms}ms (sent abort)"
    }

    method get { args } {
	# create json request
	set o [yajl create #auto]
	$o map_open map_key get array_open
	foreach arg $args { $o string $arg }
	$o array_close map_close
	set cmd [$o get]
	$o delete
	return [my do_cmd $cmd]
    }

    method set { var val } {
	# create json request
	set o [yajl create #auto]
	$o map_open map_key set map_open map_key $var double $val map_close map_close
	set cmd [$o get]
	$o delete
	return [my do_cmd $cmd]
    }

    method abort {} {
	set cmd  {{"do": "abort"}}
	return [my do_cmd $cmd]
    }

    method reward { v {get_fields ""} } {
	if {$get_fields eq ""} {
	    set get_fields $::juicer_status_fields
	}
	set o [yajl create #auto]
	$o map_open
	$o map_key do map_open map_key reward double $v map_close
	$o map_key get array_open
	foreach f $get_fields { $o string $f }
	$o array_close
	$o map_close
	set cmd [$o get]
	$o delete
	return [my do_cmd $cmd]
    }

    method purge { v } {
	set o [yajl create #auto]
	$o map_open map_key do map_open map_key purge double $v map_close map_close
	set cmd [$o get]
	$o delete
	return [my do_cmd $cmd]
    }

    method calibrate { n on off } {
	set o [yajl create #auto]
	$o map_open map_key do map_open map_key calibration
	$o map_open map_key n number $n map_key on number $on map_key off number $off map_close
	$o map_close map_close
	set cmd [$o get]
	$o delete
	return [my do_cmd $cmd]
    }
}

#
# extio juice-pin discovery (bind path, not the reward path)
#
# A box announces pin roles via state/label/<n> and its active outputs via
# state/pins/out. Labels linger in dserv after unplug, so key existence is
# not proof of life (same rule as extio_discover / ptpconf): candidates come
# only from live boxes -- extio/boxes when the extio subprocess maintains it,
# else a state/watchdog age scan.
#
#   juicer_extio_boxes          -> {hmio-1 ...}                 live boxes
#   juicer_extio_pins ?pat?     -> {{box pin label role} ...}   role = out|other
#   juicer_resolve_extio spec   -> {box pin} | {}   spec = "" | <box> | <box>/<pin>
#
# Glob *juice* matches juice, juicer, juice_left, ...
#
proc juicer_extio_denul {s} { return [lindex [split $s \x00] 0] }

proc juicer_extio_boxes {} {
    set out {}
    if {[dservExists extio/boxes]} {
	set out [dservGet extio/boxes]
    }
    # UNION with a watchdog-age scan, not a fallback: covers a box the extio
    # subprocess doesn't roster (a virtual box spawned as a subprocess) and an
    # extio subprocess that died while its boxes stayed alive
    set now [now]
    foreach k [dservKeys extio/*/state/watchdog] {
	if {![regexp {^extio/([^/]+)/state/watchdog$} $k -> b]} continue
	set age -1
	catch { set age [expr {$now - [dservTimestamp $k]}] }
	# watchdog is 1 Hz; 10 s is generous and still drops a vanished box
	if {$age >= 0 && $age < 10000000} {
	    lappend out $b
	}
    }
    return [lsort -unique $out]
}

proc juicer_extio_out_pins {box} {
    set dp extio/$box/state/pins/out
    if {![dservExists $dp]} { return {} }
    set csv [string trim [juicer_extio_denul [dservGet $dp]]]
    if {$csv eq ""} { return {} }
    set pins {}
    foreach p [split $csv ,] {
	set p [string trim $p]
	if {$p ne ""} { lappend pins $p }
    }
    return $pins
}

proc juicer_extio_pins {{pat *juice*}} {
    set hits {}
    foreach box [juicer_extio_boxes] {
	set outs [juicer_extio_out_pins $box]
	foreach k [lsort [dservKeys extio/$box/state/label/*]] {
	    if {![regexp {^extio/([^/]+)/state/label/([0-9]+)$} $k -> b pin]} continue
	    set lab [string trim [juicer_extio_denul [dservGet $k]]]
	    if {$lab eq ""} continue
	    if {![string match -nocase $pat $lab]} continue
	    set role other
	    if {[lsearch -exact $outs $pin] >= 0} { set role out }
	    lappend hits [list $b $pin $lab $role]
	}
    }
    return $hits
}

# First live *juice* digital out matching spec ("" = any, <box>, <box>/<pin>).
# An explicit pin is a claim about roles, not an override of them: it still
# has to be juice-labeled and an output to resolve.
proc juicer_resolve_extio {spec} {
    set box ""; set pin ""
    if { $spec ne "" && ![regexp {^([^/]+)(?:/([0-9]+))?$} $spec -> box pin] } {
	return {}
    }
    foreach h [juicer_extio_pins] {
	lassign $h b p lab role
	if { $role ne "out" } continue
	if { $box ne "" && $b ne $box } continue
	if { $pin ne "" && $p != $pin } continue
	return [list $b $p]
    }
    return {}
}

#
# settings (local/rig.tcl; the gear next to the Juice button edits these)
#

proc juicer_dest_norm {v} {
    set v [string trim $v]
    switch -nocase -- $v {
	"" - auto { return auto }
	usb       { return usb }
	gpio      { return gpio }
	extio     { return extio }
    }
    if { [regexp {^extio:([^:/[:space:]]+)(?:/([0-9]+))?$} $v] } { return $v }
    error "juicer destination '$v': use auto, usb, gpio, extio, extio:<box> or extio:<box>/<pin>"
}

settings::declare juicer destination -default auto \
    -validate juicer_dest_norm \
    -doc "reward route: auto (usb > extio *juice* out > gpio), usb, gpio, extio, extio:<box>, extio:<box>/<pin>" \
    -apply {::juicer_bind}

proc juicer_msml_norm {v} {
    if { ![string is double -strict $v] } {
	error "juicer ms_per_ml: '$v' is not a number"
    }
    set i [expr {int(round($v))}]
    # dserv_juicer's juicerSetMsToMl silently ignores values outside 0..4999;
    # reject here instead so the file can't hold a number the gpio path drops
    if { $i < 1 || $i > 4999 } {
	error "juicer ms_per_ml: $i out of range 1..4999"
    }
    return $i
}

settings::declare juicer ms_per_ml -default 1667 \
    -validate juicer_msml_norm \
    -doc "timed-output calibration: ms of valve-open per ml (extio and host-gpio routes)" \
    -apply {::juicer_ms_per_ml_apply}

# One number feeds both timed routes: the Tcl-side extio pulse width and the
# C module's gpio timer (which has its own copy).
proc juicer_ms_per_ml_apply {v} {
    set ::juicer_ms_per_ml $v
    catch { juicerSetMsToMl $v }
    dservSet juicer/ms_per_ml $v
}

#
# route binding
#
# Resolve the DECLARED destination to a live backend:
#
#   auto        usb pump if it answered > first live *juice* digital out >
#               host gpio (the pre-extio default; a plain Pi rig with no
#               extio subprocess must land here)
#   usb|gpio    exactly that
#   extio[...]  a live *juice* digital out (any / on <box> / exactly <box>/<pin>)
#
# An explicit destination that is not available does NOT reroute -- the whole
# point of declaring is that presence stops deciding. It binds none, leaves a
# juicer/error breadcrumb, and reward drops loudly (the rpi500 lesson: the
# silent version of this branch costs debugging sessions).
#
proc juicer_bind {args} {
    if { ![info exists ::juicer] } { return }
    set decl [settings::get juicer destination]
    set kind none
    set target {}
    set err ""
    switch -glob -- $decl {
	usb {
	    if { [$::juicer is_open] } {
		set kind usb
	    } else {
		set err "usb pump not present (destination usb)"
	    }
	}
	gpio { set kind gpio }
	extio* {
	    set spec [string range $decl 6 end]   ;# "extio" -> "", "extio:b/p" -> "b/p"
	    set hit [juicer_resolve_extio $spec]
	    if { $hit ne "" } {
		set kind extio
		set target $hit
	    } else {
		set what "no live *juice* digital out"
		if { $spec ne "" } { append what " matching '$spec'" }
		set err "$what (destination $decl)"
	    }
	}
	default {
	    # auto
	    if { [$::juicer is_open] } {
		set kind usb
	    } elseif { [set hit [juicer_resolve_extio ""]] ne "" } {
		set kind extio
		set target $hit
	    } else {
		set kind gpio
	    }
	}
    }
    # rebind on any change, target included: a vanished box with another
    # juice-pin box still live must move the target, not keep the dead one
    if { $kind ne [$::juicer backend] || $target ne [$::juicer extio_target] } {
	$::juicer set_backend $kind $target
	set desc $kind
	if { $target ne "" } { append desc " [join $target /]" }
	puts "juicer: route $desc (destination $decl)"
    }
    dservSet juicer/error $err
    return $kind
}

proc juicer_watch_extio {} {
    dservAddExactMatch extio/boxes
    dpointSetScript extio/boxes juicer_bind
    # labels land after extio/boxes on a (re)announce, and a mode flip changes
    # a pin's role without touching extio/boxes: each event re-resolves
    foreach pat {extio/*/state/label/* extio/*/state/pins/out} {
	dservAddMatch $pat
	dpointSetScript $pat juicer_bind
    }
}

#
# extio pulse serialization
#
# The box's DO pulse is a one-shot fall alarm with no cancel (pico_gpio.h):
# overlapping pulses on one pin truncate at the FIRST alarm, so back-to-back
# rewards would under-deliver. Widths queue here and go out one at a time,
# chained via dservAfter (plain `after ms script` is inert in a subprocess:
# no Tcl event loop).
#
set ::juicer_extio_q {}
set ::juicer_extio_busy 0

proc juicer_extio_pulse {us} {
    if { $::juicer_extio_busy } {
	lappend ::juicer_extio_q $us
	return
    }
    juicer_extio_send $us
}

proc juicer_extio_send {us} {
    if { [$::juicer backend] ne "extio" } {
	# rebound away mid-queue: the remaining widths were meant for extio
	set ::juicer_extio_q {}
	set ::juicer_extio_busy 0
	return
    }
    lassign [$::juicer extio_target] box pin
    if { $box eq "" || $pin eq "" } { return }
    set ::juicer_extio_busy 1
    dservSet extio/$box/cmd/do/$pin/pulse_us $us
    # release just after the fall edge; any queued width then goes out
    dservAfter [expr {$us / 1000 + 20}] juicer_extio_next
}

proc juicer_extio_next {args} {
    set ::juicer_extio_busy 0
    if { [llength $::juicer_extio_q] } {
	set us [lindex $::juicer_extio_q 0]
	set ::juicer_extio_q [lrange $::juicer_extio_q 1 end]
	juicer_extio_send $us
    }
}

proc gpio_init {} {
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

    if { $gpiochip != "" } {
	juicerInit $gpiochip
    }
}

proc init {} {
    set ::juicer [Juicer new]
    if {[set jpath [$::juicer find]] != {}} {
	$::juicer set_path $jpath
	# a pump that enumerates but won't open must not abort the conf:
	# is_open stays false and juicer_bind routes around it
	if { [catch { $::juicer open } err] } {
	    puts stderr "juicer: open $jpath failed: $err"
	}
    }
    # unconditional: gpio is auto's floor and an explicit option even when
    # the pump is present (no-op off Linux)
    gpio_init
    set ::juicer_last_trial_ml 0
}

#
# our "API" commands
#
proc reward { ml } {
    if { ![string is double -strict $ml] || $ml <= 0 } { return }
    switch -- [$::juicer backend] {
	usb {
	    catch { publish_juicer_response [$::juicer reward $ml] }
	}
	extio {
	    set us [expr {int($ml * $::juicer_ms_per_ml * 1000)}]
	    if { $us > 0 } { juicer_extio_pulse $us }
	}
	gpio {
	    # currently assume only a single juicer is configured
	    juicerJuiceAmount 0 $ml
	}
	default {
	    # explicitly declared destination is unavailable: drop loudly,
	    # and do NOT book juice the animal never got
	    dservSet juicer/error "reward $ml dropped: destination [settings::get juicer destination] unavailable"
	    puts stderr "juicer: reward $ml dropped -- destination unavailable"
	    return
	}
    }
    # Notify db subprocesses to accumulate juice in session table
    catch { send db "session_add_juice $ml" }
}


proc pump_voltage { } {
    return [$::juicer get pump_voltage charging]
}

proc calibrate { n on off } {
    return [$::juicer calibrate $n $on $off]
}

proc set_flow_rate { val } {
    return [$::juicer set flow_rate $val]
}

#
# gear-dialog surface (ess_control.html): status snapshot + validated setters.
# Setters persist to local/rig.tcl (settings::put -persist -> -apply -> bind)
# and return the fresh snapshot so the dialog re-renders from the reply.
#
proc juicer_status {} {
    set o [yajl create #auto]
    $o map_open
    $o map_key destination string [settings::get juicer destination]
    $o map_key destination_source string [settings::source_of juicer destination]
    $o map_key backend string [$::juicer backend]
    $o map_key target string [expr {[dservExists juicer/target] ? [dservGet juicer/target] : ""}]
    $o map_key ms_per_ml number [settings::get juicer ms_per_ml]
    $o map_key usb map_open \
	map_key present bool [$::juicer is_open] \
	map_key path string [$::juicer path] map_close
    $o map_key gpio map_open \
	map_key available bool [expr {$::tcl_platform(os) eq "Linux"}] map_close
    $o map_key extio array_open
    foreach h [juicer_extio_pins] {
	lassign $h box pin lab role
	$o map_open
	$o map_key box string $box
	$o map_key pin number $pin
	$o map_key label string $lab
	$o map_key out bool [expr {$role eq "out"}]
	$o map_close
    }
    $o array_close
    $o map_key error string [expr {[dservExists juicer/error] ? [dservGet juicer/error] : ""}]
    $o map_close
    set json [$o get]
    $o delete
    return $json
}

proc juicer_set_destination { v } {
    settings::put juicer destination $v -persist
    return [juicer_status]
}

proc juicer_set_ms_per_ml { v } {
    settings::put juicer ms_per_ml $v -persist
    return [juicer_status]
}

# Re-probe the pump (covers a pump plugged in after boot) and re-resolve.
proc juicer_rescan {} {
    if { ![$::juicer is_open] } {
	if {[set jpath [$::juicer find]] != {}} {
	    $::juicer set_path $jpath
	    catch { $::juicer open }
	}
    }
    juicer_bind
    return [juicer_status]
}

init

# settings land now: settings::get doesn't run -apply, so push the effective
# values once, then watch extio topology and resolve the route
juicer_ms_per_ml_apply [settings::get juicer ms_per_ml]
juicer_watch_extio
juicer_bind

# local system configuration in /usr/local/dserv/local/juicer.tcl
if { [file exists $dspath/local/juicer.tcl] } {
    source $dspath/local/juicer.tcl
}

# legacy migration: local/juicer.tcl used to be ::juicer_ms_per_ml's home.
# Honor it as a runtime override until a human declares the setting (then
# the declaration wins and the local line should be deleted).
if { [info exists ::juicer_ms_per_ml]
     && $::juicer_ms_per_ml != [settings::get juicer ms_per_ml]
     && [settings::source_of juicer ms_per_ml] eq "default" } {
    catch { settings::put juicer ms_per_ml $::juicer_ms_per_ml }
}

puts "Juicer initialized"

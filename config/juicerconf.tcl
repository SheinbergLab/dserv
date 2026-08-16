# JSON support
package require yajltcl

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
    variable _use_gpio
    variable _backend
    variable _extio_target
    variable _timeout_ms

    constructor { { path {} } } {
	set _fd -1
	set _path $path
	set _use_gpio 0
	set _backend ""
	set _extio_target {}
	set _timeout_ms 10000   ;# bound the pump-reply wait (covers a large dispense/purge)
    }

    destructor { my close }

    method set_path { path } { set _path $path }
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

    method use_gpio {} { my set_backend gpio }
    method using_gpio {} { return [expr {$_backend eq "gpio"}] }
    method backend {} { return $_backend }
    method extio_target {} { return $_extio_target }
    method set_backend {kind {target {}}} {
	set _backend $kind
	set _extio_target $target
	set _use_gpio [expr {$kind eq "gpio"}]
	dservSet juicer/backend $kind
	if {$kind eq "extio"} {
	    dservSet juicer/extio [join $target]
	}
    }

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
# A box announces live presence via state/watchdog (1 Hz) and pin roles via
# state/label/<n>. Labels linger in dserv after unplug, so key existence is not
# proof of life (same rule as extio_discover / ptpconf). Prefer extio/boxes,
# the live set extio already publishes.
#
#   juicer_extio_boxes          -> {hmio-1 ...}
#   juicer_extio_pins ?pat?     -> {{box pin label role} ...}  role=out|other
#   juicer_find_extio ?pat?     -> {box pin} of the first digital-out hit, or {}
#
# Glob *juice* matches juice, juicer, ...
#
proc juicer_extio_denul {s} { return [lindex [split $s \x00] 0] }

proc juicer_extio_boxes {} {
    if {[dservExists extio/boxes]} {
	set boxes [dservGet extio/boxes]
	if {$boxes ne ""} { return $boxes }
    }
    set out {}
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
	foreach k [dservKeys extio/$box/state/label/*] {
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

# First *juice* pin that is currently a digital out. Empty if none.
proc juicer_find_extio {{pat *juice*}} {
    foreach h [juicer_extio_pins $pat] {
	lassign $h box pin lab role
	if {$role eq "out"} { return [list $box $pin] }
    }
    return {}
}

# Bind USB / extio / gpio once. Called from init (USB) and on extio/boxes.
proc juicer_choose_backend {args} {
    if {![info exists ::juicer]} { return }
    set cur [$::juicer backend]
    if {$cur eq "usb"} { return }
    set hit [juicer_find_extio]
    if {$hit ne ""} {
	if {$cur ne "extio"} {
	    $::juicer set_backend extio $hit
	    puts "juicer: backend extio {$hit}"
	}
	return
    }
    if {$cur eq "extio"} { return }
    set boxes {}
    if {[dservExists extio/boxes]} { set boxes [dservGet extio/boxes] }
    if {$boxes eq ""} { return }
    if {$cur ne "gpio"} {
	$::juicer set_backend gpio
	puts "juicer: backend gpio (no *juice* digital out on {$boxes})"
    }
}

proc juicer_watch_extio {} {
    dservAddExactMatch extio/boxes
    dpointSetScript extio/boxes juicer_choose_backend
    # labels can land after extio/boxes is first published; without this
    # choose_backend binds gpio on an empty hunt and never retries.
    dservAddMatch extio/*/state/label/*
    dpointSetScript extio/*/state/label/* juicer_choose_backend
    juicer_choose_backend
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
    set ::juicer_ms_per_ml 1667
    set ::juicer [Juicer new]
    if {[set jpath [$::juicer find]] != {}} {
	$::juicer set_path $jpath
	$::juicer open
	$::juicer set_backend usb
	puts "juicer: backend usb $jpath"
    } else {
	gpio_init
	juicer_watch_extio
    }
    set ::juicer_last_trial_ml 0
}

#
# our "API" commands
#
proc reward { ml } {
    switch -- [$::juicer backend] {
	usb {
	    catch { publish_juicer_response [$::juicer reward $ml] }
	}
	extio {
	    lassign [$::juicer extio_target] box pin
	    set us [expr {int($ml * $::juicer_ms_per_ml * 1000)}]
	    if {$us > 0 && $box ne "" && $pin ne ""} {
		set dp extio/$box/cmd/do/$pin/pulse_us
		# retained cmd leaves do not re-push an unchanged value
		if {[dservExists $dp] && [dservGet $dp] == $us} {
		    dservSet $dp 0
		}
		dservSet $dp $us
	    }
	}
	gpio {
	    juicerJuiceAmount 0 $ml
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

init

# local system configuration in /usr/local/dserv/local/juicer.tcl
if { [file exists $dspath/local/juicer.tcl] } {
    source $dspath/local/juicer.tcl
}

puts "Juicer initialized"


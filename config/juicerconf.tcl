puts "initializing juicer"

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
    
    constructor { { path {} } } {
	set _fd -1
	set _path $path
	set _use_gpio 0
    }
    
    destructor { my close }

    method set_path { path } { set _path $path }
    method open {} {
	set _fd [open $_path RDWR]
	fconfigure $_fd -buffering line
    }

    method use_gpio {} { set _use_gpio 1 }
    method using_gpio {} { return $_use_gpio }
    
    method find {} {
	set model_name juicer
	set devices [glob /dev/ttyACM* /dev/ttyUSB*]
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
    method do_cmd { cmd } {
	if { $_fd < 0 } { return }
	puts $_fd $cmd
	set result [gets $_fd]
	return $result
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

    method reward { v } {
	set o [yajl create #auto]
	$o map_open map_key do map_open map_key reward double $v map_close map_close
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
	$::juicer open
    } else {
	gpio_init
	$::juicer use_gpio
    }
    set ::juicer_last_trial_ml 0
}

#
# our "API" commands
#
proc reward { ml } {
    if {[$::juicer using_gpio]} {
	# currently assume only a single juicer is configured
	juicerJuiceAmount 0 $ml
    } else {
	$::juicer reward $ml
    }
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

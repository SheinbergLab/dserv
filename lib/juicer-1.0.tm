# -*- mode: tcl -*-

###
### Juicer API
###

### example usage:
# set j [Juicer new /dev/ttyACM0]
# $j open
# $j reward 0.75
# $j close
#

 # Available API Commands (defined by arduino firmware)

 # 1. Set Commands:
 #    - {"set": {"flow_rate": <float>}}
 #      - Sets the flow rate (must be > 0).
 #    - {"set": {"purge_vol": <float>}}
 #      - Sets the purge volume (must be > 0).
 #    - {"set": {"target_rps": <float>}} {"set": {"target_rps": 3}}
 #      - Sets the target revolutions per second (must be > 0 and <= MAX_RPS).

 # 2. Do Commands (only 1 allowed per request):
 #    - {"do": "abort"}
 #      - Aborts the current operation, stops the pump, and updates reward metrics.
 #    - {"do": {"reward": <float>}} {"do": {"reward": 0.8}}
 #      - Dispenses a reward of the specified amount (must be > 0).
 #    - {"do": {"purge": <float>}}
 #      - Starts a purge operation for the specified amount (must be > 0).
 #    - {"do": {"calibration": {"n": <int>, "on": <int>, "off": <int>}}}
 #         e.g.  {"do": {"calibration": {"n": 10, "on": 500, "off": 500}}}
 #      - Runs a calibration cycle with:
 #        - n: Number of cycles (must be > 0).
 #        - on: Duration in milliseconds for the pump to be ON (must be > 0).
 #        - off: Duration in milliseconds for the pump to be OFF (must be > 0).

 # 3. Get Commands:
 #    - {"get": ["flow_rate"]}
 #      - Retrieves the current flow rate.
 #    - {"get": ["purge_vol"]}
 #      - Retrieves the current purge volume.
 #    - {"get": ["target_rps"]}
 #      - Retrieves the current target revolutions per second.
 #    - {"get": ["reward_mls"]}
 #      - Retrieves the total amount of reward dispensed in milliliters.
 #    - {"get": ["reward_number"]}
 #      - Retrieves the total number of rewards dispensed.
 #    - {"get": ["<unknown_parameter>"]}
 #      - Returns "Unknown parameter" for any unrecognized parameter.

 # Example Combined Request:
 #    {"set": {"target_rps": 2, "flow_rate": 0.65}, "do": {"reward": 1},"get": ["reward_mls", "reward_number"]}

 # Expected Response:
 #    {
 #      "status": "success",
 #      "reward_mls": <float>,
 #      "reward_number": <int>
 #    }


set dlshlib [file join /usr/local dlsh dlsh.zip]
if [file exists $dlshlib] {
    set base [file join [zipfs root] dlsh]
    if { [lsearch [zipfs mount] $dlshlib] == -1 } {
	zipfs mount $dlshlib $base
	set auto_path [linsert $auto_path [set auto_path 0] $base/lib]
    }
}

package require yajltcl

catch { Juicer destroy }

oo::class create Juicer {
    variable _fd 
    variable _path /dev/ttyACM0

    constructor { path } {
	set _fd -1
	set _path $path
    }
    
    destructor { my close }

    method set_path { path } { set _path $path }
    method open {} {
	set _fd [open $_path RDWR]
	fconfigure $_fd -buffering line
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
	$o map_open map_key do map_open map_key calibrate
	$o map_open map_key n number $n map_key on number $on map_key off number $off map_close
	$o map_close map_close
	set cmd [$o get]
	$o delete
	return [my do_cmd $cmd]
    }
}

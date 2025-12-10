# interacts with power monitor module
# deals entirely in json requests and responses
# example get: {"get": ["v", "a", "w", "pct", "charging"]}
# example set: {"set": {"min_v": 21.0, "max_v": 32.2}}
# example response {"v": 28.523, "a": 0.1234, "w": 3.5123, "pct": 67.12, "charging": true}
# or manually from essctrl: send powermon get_vals pct charging
# or send powermon set_vals min_v 21.0 max_v 32.2

package require dlsh
package require yajltcl

tcl::tm::add $dspath/lib

errormon enable

set interval_ms 10000; # update powermon readings every 10 seconds
set poll_fields {v a w pct charging}
set device ""
set timer_prefix powermonTimer

set ess_modules "timer"
foreach f $ess_modules {
    load ${dspath}/modules/dserv_${f}[info sharedlibextension]
}

proc build_get_payload {fields} {
    set builder [yajl create #auto]
    $builder map_open
    $builder map_key get
    $builder array_open
    foreach key $fields {
        $builder string $key
    }
    $builder array_close
    $builder map_close
    set payload [$builder get]
    $builder delete
    return $payload
}

proc build_set_payload {pairs} {
    set builder [yajl create #auto]
    $builder map_open
    $builder map_key set
    $builder map_open
    foreach {key value} $pairs {
        $builder map_key $key
        if {[string is double -strict $value]} {
            $builder number $value
        } elseif {[string is boolean -strict $value]} {
            $builder bool $value
        } else {
            $builder string $value
        }
    }
    $builder map_close
    $builder map_close
    set payload [$builder get]
    $builder delete
    return $payload
}

proc parse_json_response {response} {
    if {[catch {::yajl::json2dict $response} parsed err]} {
        error "Power monitor JSON parse failed: $err"
    }
    return $parsed
}

proc powermon_send {payload {publish_fields ""}} {
    if {[catch {send_request $payload} response err]} {
        error "Power monitor request failed: $err"
    }
    return [powermon_process_response $response $publish_fields]
}

proc powermon_process_response {response publish_fields} {
    if {$response eq ""} {
        return {}
    }
    set parsed [parse_json_response $response]
    if {$publish_fields eq ""} {
        set publish_fields [dict keys [extract_payload $parsed]]
    }
    publish_selected $parsed $publish_fields
    return $parsed
}

proc set_vals {args} {
    # send arbitrary {"set": {...}} payloads to the power monitor
    # example: send powermon set_config min_v 21.0 max_v 32.2
    if {[llength $args] == 0} {
        error "set_vals requires at least one key value pair"
    }
    if {[expr {[llength $args] % 2}] != 0} {
        error "set_vals arguments must be key value pairs"
    }
    set payload [build_set_payload $args]
    return [powermon_send $payload]
}

proc get_vals {fields} {
    # request arbitrary fields, publish to powermon/<key>
    # example: get_vals {v a w pct charging min_v max_v}
    if {[llength $fields] == 0} {
        error "get_vals requires at least one field name"
    }
    set payload [build_get_payload $fields]
    return [powermon_send $payload $fields]
}

proc set_config {args} {
    return [set_vals $args]
}

proc get_values {args} {
    return [get_vals $args]
}

proc extract_payload {parsed} {
    foreach key {get data result} {
        if {[dict exists $parsed $key]} {
            return [dict get $parsed $key]
        }
    }
    return $parsed
}

proc publish_selected {parsed keys} {
    set payload [extract_payload $parsed]
    foreach key $keys {
        if {![dict exists $payload $key]} {
            continue
        }
        set value [dict get $payload $key]
        if {$key eq "charging" && [string is boolean -strict $value]} {
            set value [expr {$value ? 1 : 0}]
        }
        dservSet powermon/${key} $value
    }
}

proc find_device {} {
    set links [glob -nocomplain -types l /dev/serial/by-id/*]
    foreach l $links {
        set tail [file tail $l]
        if {[string match *power_monitor* $tail]} {
            return $l
        }
        if {![catch {file readlink $l} target] && [string match *power_monitor* $target]} {
            return $l
        }
    }
    return ""
}

proc configure_device {} {
    global device
    set device [find_device]
    if {$device eq ""} {
        puts stderr "Power monitor not found under /dev/serial/by-id"
        exit
    }
    puts "Power monitor device: $device"
}

proc send_request {payload} {
    global device
    set fd ""
    set response ""
    try {
        set fd [open $device r+]
        fconfigure $fd -buffering none -translation binary -blocking 1
        puts -nonewline $fd $payload
        flush $fd
        if {[gets $fd response] < 0} {
            error "no response from $device"
        }
    } finally {
        if {$fd ne ""} {
            catch {close $fd}
        }
    }
    return [string trim $response]
}

proc poll {} {
    global poll_fields
    if {[catch {get_vals $poll_fields} err]} {
        puts stderr "Power monitor poll error: $err"
    }
}

proc timer_callback {dpoint data} {
    poll
}

proc setup_timer {} {
    global timer_prefix
    timerPrefix $timer_prefix
    dservRemoveAllMatches
    dservAddExactMatch ${timer_prefix}/0
    dpointSetScript ${timer_prefix}/0 timer_callback
}

proc start {{poll_interval_ms 0}} {
    global interval_ms
    if {$poll_interval_ms <= 0} {
        set poll_interval_ms $interval_ms
    }
    timerTickInterval $poll_interval_ms $poll_interval_ms
}

proc stop {} {
    global interval_ms
    if {$interval_ms <= 0} {
        return
    }
    timerStop
}

configure_device
setup_timer
start

puts "Power monitor initialized"
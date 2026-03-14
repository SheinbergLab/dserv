#
# openephysconf.tcl - Open Ephys integration subprocess
#
# Connects dserv to an Open Ephys GUI instance via its HTTP API (port 37497).
# Automatically syncs recording filenames and start/stop recording
# when ESS datafiles are opened/closed.
#
# Configuration:
#   Set openephys/ipaddr datapoint or use local/openephys.tcl
#
# Datapoints published:
#   openephys/status      - IDLE, ACQUIRE, RECORD, or DISCONNECTED
#   openephys/connected   - 1 or 0
#   openephys/recording   - current recording prepend_text (filename prefix)
#   openephys/info        - JSON recording config from Open Ephys
#   openephys/processors  - JSON processor/signal chain info
#
# Datapoints watched:
#   ess/datafile          - triggers recording start/stop and filename sync
#   openephys/ipaddr      - IP address of Open Ephys machine
#
# Timers:
#   oeTimer/0  - periodic status polling
#   oeTimer/1  - delayed ACQUIRE->RECORD transition (one-shot)
#
# Commands (via essctrl "send openephys <cmd>"):
#   status                  - query and publish current status
#   acquire                 - set mode ACQUIRE
#   record                  - set mode RECORD
#   idle                    - set mode IDLE
#   set_prepend <text>      - set recording filename prefix
#   set_parent_dir <path>   - set recording parent directory
#   new_directory           - start a new recording directory
#   get_recording_info      - query /api/recording config
#   get_processors          - query /api/processors
#   send_config <id> <text> - send config message to processor
#   broadcast <text>        - broadcast message to all processors
#   get_info                - return current local config as dict
#

puts "Initializing Open Ephys integration"

# Disable exit for subprocess
proc exit {args} { error "exit not available for this subprocess" }

package require dlsh
package require yajltcl
package require http

# Load timer module (gives us 8 timers: oeTimer/0 through oeTimer/7)
load ${dspath}/modules/dserv_timer[info sharedlibextension]

#################################################################
# Configuration
#################################################################

# Open Ephys host - override via openephys/ipaddr datapoint or local/openephys.tcl
set oe_ipaddr ""
set oe_port 37497

# Polling interval (ms) for status checks
set oe_poll_interval 5000

# Delay (ms) between entering ACQUIRE and starting RECORD
# Open Ephys needs a moment after ACQUIRE before it accepts RECORD
set oe_acquire_settle_ms 500

# Auto-record: start/stop recording on datafile open/close
set oe_auto_record 1

# Auto-acquire: enter ACQUIRE mode when connected (if currently IDLE)
set oe_auto_acquire 1

# Current state tracking
set oe_connected 0
set oe_status "DISCONNECTED"
set oe_current_prepend ""
set oe_current_datafile ""

# Pending record flag: set when waiting for ACQUIRE to settle
# before transitioning to RECORD via timer 1
set oe_pending_record 0

#################################################################
# HTTP helpers
#################################################################

proc oe_url {path} {
    global oe_ipaddr oe_port
    return "http://${oe_ipaddr}:${oe_port}${path}"
}

proc oe_get {path} {
    set url [oe_url $path]
    set tok [::http::geturl $url -timeout 3000]
    set body [::http::data $tok]
    set code [::http::ncode $tok]
    ::http::cleanup $tok
    if {$code != 200} {
        error "HTTP GET $path returned $code"
    }
    return $body
}

proc oe_put {path json_body} {
    set url [oe_url $path]
    set tok [::http::geturl $url -timeout 3000 \
                 -type "application/json" \
                 -query $json_body \
                 -method PUT]
    set body [::http::data $tok]
    set code [::http::ncode $tok]
    ::http::cleanup $tok
    if {$code != 200} {
        error "HTTP PUT $path returned $code"
    }
    return $body
}

#################################################################
# Open Ephys API - Status
#################################################################

proc oe_check_status {} {
    global oe_ipaddr oe_connected oe_status

    if {$oe_ipaddr eq ""} {
        set_oe_state "DISCONNECTED" 0
        return "DISCONNECTED"
    }

    if {[catch {
        set res [oe_get /api/status]
        set d [::yajl::json2dict $res]
        set mode [dict get $d mode]
    } err]} {
        set_oe_state "DISCONNECTED" 0
        return "DISCONNECTED"
    }

    set_oe_state $mode 1
    return $mode
}

proc oe_set_mode {mode} {
    global oe_ipaddr

    if {$oe_ipaddr eq ""} {
        error "Open Ephys IP not configured"
    }

    set body [format {{"mode":"%s"}} $mode]
    set res [oe_put /api/status $body]
    set d [::yajl::json2dict $res]
    set new_mode [dict get $d mode]

    # Update local state
    oe_check_status
    return $new_mode
}

#################################################################
# Open Ephys API - Recording Configuration
#################################################################

proc oe_get_recording_info {} {
    global oe_ipaddr

    if {$oe_ipaddr eq ""} {
        error "Open Ephys IP not configured"
    }

    set res [oe_get /api/recording]
    dservSet openephys/info $res
    return $res
}

proc oe_set_recording_config {args} {
    global oe_ipaddr oe_current_prepend

    if {$oe_ipaddr eq ""} {
        error "Open Ephys IP not configured"
    }

    # Parse args as key-value pairs, with defaults
    set prepend_text ""
    set base_text "auto"
    set append_text ""
    set parent_directory ""
    set new_dir false

    foreach {key val} $args {
        switch -- $key {
            -prepend    { set prepend_text $val }
            -base       { set base_text $val }
            -append     { set append_text $val }
            -parent_dir { set parent_directory $val }
            -new_dir    { set new_dir $val }
        }
    }

    # Build JSON payload
    set json [yajl create #auto]
    $json map_open
    $json string "prepend_text" string $prepend_text
    $json string "base_text"    string $base_text
    $json string "append_text"  string $append_text
    if {$parent_directory ne ""} {
        $json string "parent_directory" string $parent_directory
    }
    if {$new_dir} {
        $json string "start_new_directory" string "true"
    }
    $json map_close
    set body [$json get]
    $json delete

    set res [oe_put /api/recording $body]
    set oe_current_prepend $prepend_text
    dservSet openephys/recording $prepend_text

    puts "openephys: Recording config set: prepend=$prepend_text new_dir=$new_dir"
    return $res
}

#################################################################
# Open Ephys API - Processors / Signal Chain
#################################################################

proc oe_get_processors {{processor_id ""}} {
    global oe_ipaddr

    if {$oe_ipaddr eq ""} {
        error "Open Ephys IP not configured"
    }

    if {$processor_id ne ""} {
        set res [oe_get /api/processors/$processor_id]
    } else {
        set res [oe_get /api/processors]
        dservSet openephys/processors $res
    }
    return $res
}

proc oe_send_config {processor_id text} {
    global oe_ipaddr

    if {$oe_ipaddr eq ""} {
        error "Open Ephys IP not configured"
    }

    set json [yajl create #auto]
    $json map_open
    $json string "text" string $text
    $json map_close
    set body [$json get]
    $json delete

    return [oe_put /api/processors/$processor_id/config $body]
}

#################################################################
# Open Ephys API - Broadcast Messages
#################################################################

proc oe_broadcast {text} {
    global oe_ipaddr

    if {$oe_ipaddr eq ""} {
        error "Open Ephys IP not configured"
    }

    set json [yajl create #auto]
    $json map_open
    $json string "text" string $text
    $json map_close
    set body [$json get]
    $json delete

    return [oe_put /api/message $body]
}

#################################################################
# State management
#################################################################

proc set_oe_state {mode connected} {
    global oe_status oe_connected

    set changed_status  [expr {$mode ne $oe_status}]
    set changed_connect [expr {$connected != $oe_connected}]

    set oe_status $mode
    set oe_connected $connected

    if {$changed_status}  { dservSet openephys/status    $mode }
    if {$changed_connect} { dservSet openephys/connected $connected }
}

#################################################################
# Datafile event handler - the core integration
#################################################################

proc process_ess_datafile {dpoint data} {
    global oe_current_datafile

    set new_datafile $data

    # Datafile closed (was something, now empty)
    if {$oe_current_datafile ne "" && $new_datafile eq ""} {
        on_datafile_closed $oe_current_datafile
    }

    # Datafile opened (was empty or different, now something new)
    if {$new_datafile ne "" && $new_datafile ne $oe_current_datafile} {
        on_datafile_opened $new_datafile
    }

    set oe_current_datafile $new_datafile
}

proc on_datafile_opened {datafile} {
    global oe_auto_record oe_connected oe_pending_record oe_acquire_settle_ms

    if {!$oe_connected} {
        puts "openephys: Datafile opened but not connected"
        return
    }

    # ess/datafile is already the bare name (e.g. Dex_fixcal_250314_1423)
    # so use it directly as the Open Ephys recording prefix
    puts "openephys: Datafile opened: $datafile"

    # Set the filename and start a new recording directory
    if {[catch {
        oe_set_recording_config -prepend $datafile -new_dir true
    } err]} {
        puts "openephys: WARNING: Failed to set recording config: $err"
        return
    }

    if {!$oe_auto_record} { return }

    set cur [oe_check_status]

    if {$cur eq "IDLE"} {
        # Need to go through ACQUIRE first, then RECORD after
        # a settle delay via timer 1 (one-shot)
        puts "openephys: Starting acquisition..."
        if {[catch {oe_set_mode "ACQUIRE"} err]} {
            puts "openephys: WARNING: Failed to start acquisition: $err"
            return
        }
        set oe_pending_record 1
        timerTick 1 $oe_acquire_settle_ms
    } elseif {$cur eq "ACQUIRE"} {
        # Already acquiring, go straight to RECORD
        puts "openephys: Starting recording..."
        if {[catch {oe_set_mode "RECORD"} err]} {
            puts "openephys: WARNING: Failed to start recording: $err"
        }
    }
    # If already RECORD, the new directory/prepend is enough
}

proc on_datafile_closed {datafile} {
    global oe_auto_record oe_connected oe_pending_record

    # Cancel any pending ACQUIRE->RECORD transition
    if {$oe_pending_record} {
        timerStop 1
        set oe_pending_record 0
    }

    if {!$oe_connected} { return }

    puts "openephys: Datafile closed: $datafile"

    if {!$oe_auto_record} { return }

    if {[catch {
        set cur [oe_check_status]
        if {$cur eq "RECORD"} {
            puts "openephys: Stopping recording (-> ACQUIRE)..."
            oe_set_mode "ACQUIRE"
        }
    } err]} {
        puts "openephys: WARNING: Failed to stop recording: $err"
    }
}

# Timer 1 callback: delayed ACQUIRE -> RECORD transition
proc oe_delayed_record_callback {dpoint data} {
    global oe_pending_record oe_connected

    if {!$oe_pending_record} { return }
    set oe_pending_record 0

    if {!$oe_connected} { return }

    if {[catch {
        set cur [oe_check_status]
        if {$cur eq "ACQUIRE"} {
            puts "openephys: Starting recording (after settle delay)..."
            oe_set_mode "RECORD"
        } else {
            puts "openephys: Skipping delayed record - status is $cur"
        }
    } err]} {
        puts "openephys: WARNING: Delayed record failed: $err"
    }
}

#################################################################
# IP address configuration handler
#################################################################

proc process_oe_ipaddr {dpoint data} {
    global oe_ipaddr oe_auto_acquire

    if {$data eq ""} {
        set oe_ipaddr ""
        set_oe_state "DISCONNECTED" 0
        puts "openephys: IP address cleared"
        return
    }

    set oe_ipaddr $data
    puts "openephys: IP address set to $data, checking connection..."

    set mode [oe_check_status]
    if {$mode ne "DISCONNECTED"} {
        puts "openephys: Connected! Status: $mode"
        if {$oe_auto_acquire && $mode eq "IDLE"} {
            puts "openephys: Auto-acquiring..."
            catch {oe_set_mode "ACQUIRE"}
        }
    } else {
        puts "openephys: Could not connect to $data"
    }
}

#################################################################
# Timer setup
#################################################################

proc oe_poll_callback {dpoint data} {
    oe_check_status
}

proc oe_setup_timers {} {
    timerPrefix oeTimer

    # Timer 0: periodic status polling
    dservAddExactMatch oeTimer/0
    dpointSetScript oeTimer/0 oe_poll_callback

    # Timer 1: one-shot delayed ACQUIRE->RECORD transition
    dservAddExactMatch oeTimer/1
    dpointSetScript oeTimer/1 oe_delayed_record_callback
}

proc oe_start_polling {{interval_ms 0}} {
    global oe_poll_interval
    if {$interval_ms > 0} {
        set oe_poll_interval $interval_ms
    }
    timerTickInterval 0 $oe_poll_interval $oe_poll_interval
    puts "openephys: Status polling started (${oe_poll_interval}ms)"
}

proc oe_stop_polling {} {
    timerStop 0
    puts "openephys: Status polling stopped"
}

#################################################################
# Manual commands (via essctrl: send openephys <cmd>)
#################################################################

proc status {} {
    return [oe_check_status]
}

proc acquire {} {
    return [oe_set_mode "ACQUIRE"]
}

proc record {} {
    return [oe_set_mode "RECORD"]
}

proc idle {} {
    return [oe_set_mode "IDLE"]
}

proc set_prepend {text} {
    return [oe_set_recording_config -prepend $text]
}

proc set_parent_dir {path} {
    return [oe_set_recording_config -parent_dir $path]
}

proc new_directory {} {
    global oe_current_prepend
    return [oe_set_recording_config -prepend $oe_current_prepend -new_dir true]
}

proc get_recording_info {} {
    return [oe_get_recording_info]
}

proc get_processors {{processor_id ""}} {
    return [oe_get_processors $processor_id]
}

proc send_config {processor_id text} {
    return [oe_send_config $processor_id $text]
}

proc broadcast {text} {
    return [oe_broadcast $text]
}

proc get_info {} {
    global oe_ipaddr oe_port oe_connected oe_status
    global oe_current_prepend oe_auto_record oe_auto_acquire oe_poll_interval

    return [dict create \
        ipaddr       $oe_ipaddr \
        port         $oe_port \
        connected    $oe_connected \
        status       $oe_status \
        recording    $oe_current_prepend \
        auto_record  $oe_auto_record \
        auto_acquire $oe_auto_acquire \
        poll_ms      $oe_poll_interval \
    ]
}

#################################################################
# Initialize
#################################################################

# Subscribe to datafile changes (bare name, no path/extension)
dservAddExactMatch ess/datafile
dpointSetScript    ess/datafile process_ess_datafile

# Subscribe to IP address config
dservAddExactMatch openephys/ipaddr
dpointSetScript    openephys/ipaddr process_oe_ipaddr

# Local configuration overrides
# Use local/openephys.tcl to set:
#   set oe_ipaddr "192.168.x.x"
#   set oe_auto_record 1
#   set oe_auto_acquire 1
#   set oe_poll_interval 5000
#   set oe_acquire_settle_ms 500
if {[file exists $dspath/local/openephys.tcl]} {
    source $dspath/local/openephys.tcl
}

# Try to get IP from datapoint if not set locally
if {$oe_ipaddr eq ""} {
    catch {
        set addr [dservGet openephys/ipaddr]
        if {$addr ne ""} {
            set oe_ipaddr $addr
        }
    }
}

# Setup timers and start polling
oe_setup_timers
oe_start_polling

# Initial status check
oe_check_status

# Touch datafile to pick up current state if ess is already running
catch {dservTouch ess/datafile}

puts "openephys: Open Ephys integration ready"
puts "  IP: [expr {$oe_ipaddr ne {} ? $oe_ipaddr : {(not configured - set openephys/ipaddr)}}]"
puts "  Status: $oe_status"
puts "  Auto-record: [expr {$oe_auto_record ? {on} : {off}}]"
puts "  Auto-acquire: [expr {$oe_auto_acquire ? {on} : {off}}]"
puts "  Settle delay: ${oe_acquire_settle_ms}ms"
puts "  Poll interval: ${oe_poll_interval}ms"
puts ""
puts "Commands:"
puts "  status                    - check Open Ephys status"
puts "  acquire                   - start acquisition"
puts "  record                    - start recording"
puts "  idle                      - stop everything"
puts "  set_prepend <text>        - set recording filename prefix"
puts "  set_parent_dir <path>     - set recording parent directory"
puts "  new_directory             - start new recording directory"
puts "  get_recording_info        - query recording configuration"
puts "  get_processors ?id?       - query signal chain / processor info"
puts "  send_config <id> <text>   - send config to a processor"
puts "  broadcast <text>          - broadcast message (while acquiring)"
puts "  get_info                  - show current local configuration"

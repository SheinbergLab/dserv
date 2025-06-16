# triggers.tcl

puts "initializing triggers"

set dspath [file dir [info nameofexecutable]]

proc update_dio { name data } {
    set eds [expr { [lindex $data 0] & 0xFFFFFFFF}]
    set lev [expr { [lindex $data 1] & 0xFFFFFFFF}]
    dservSet ess/dio "$eds $lev"
}

proc update_joystick { name data } {
    dservSet ess/joystick [expr {31-$data}]
}

proc update_touch { name data } {
    dservSet ess/touch "[lindex $data 0] [lindex $data 1]"
}

proc update_ain { name data } {
    set em_pnts_per_deg_x 200
    set em_pnts_per_deg_y 200
    set v1 [expr { [lindex $data 0] & 0xFFFF}]
    set v2 [expr { [lindex $data 1] & 0xFFFF}]
    set v2a [format %.3f [expr {(($v2-2048.)/$em_pnts_per_deg_y)}]]
    set v1a [format %.3f [expr {((2048.-$v1)/$em_pnts_per_deg_x)}]]
    dservSet ess/em_pos "$v2 $v1 $v2a $v1a"
}

proc update_events { name data } {
    set einfo [split $name :]
    set type [lindex $einfo 1]
    set sub [lindex $einfo 2]

    switch $type { 
	3 { switch $sub {
	    0 { dservSet ess/state Running }
	    1 { dservSet ess/state Stopped }
	    4 { dservSet ess/state Stopped }
	    2 { dservSet ess/reset 1 }
	  }
	}
	7 { switch $sub {
	    0 { dservSet ess/state Stopped }
	    1 { dservSet ess/state Running }
	    2 { dservSet ess/state Inactive }
	  }
	}
	18 { switch $sub {
	    0 { dservSet ess/system $data }
	    1 { dservSet ess/subject $data }
	    2 { dservSet ess/protocol [lindex [split $data :] 1] }
	    3 { dservSet ess/variant [lindex [split $data :] 2] }
	}
	    dservSet ess/state Stopped
	}
	19 {
	    dservSet ess/obs_active 1
	    dservSet ess/obs_id [lindex $data 0]
	    dservSet ess/obs_total [lindex $data 1]
	}
	20 {
	    dservSet ess/obs_active 0
	}
	29 {
	    if { $sub == 1 } {
		dservSet ess/stimtype [lindex $data 0]
	    }
	}
    }
}

proc update_state { name data } {
    if { $data == "Running" } {
	dservSet ess/running 1
    } else {
	dservSet ess/running 0
    }
}

proc update_ain_window_settings { name data } {
    dservSet ess/em_region_setting $data
}

proc update_ain_window_status { name data } {
    dservSet ess/em_region_status $data
}

proc update_touch_window_settings { name data } {
    dservSet ess/touch_region_setting $data
}

proc update_touch_window_status { name data } {
    dservSet ess/touch_region_status $data
}

proc init_vars { } {
    dservSet ess/time [now]
    dservSet ess/dio "0 0"
    dservSet ess/em_pos "2048 2048 0. 0."
    dservSet ess/state "Inactive"
    
    dservSet ess/name classify
    dservSet ess/executable ess
    dservSet ess/remote 100.0.0.20
    dservSet ess/subject tiger
    
    dservSet ess/obs_active 0
    dservSet ess/obs_id 1
    dservSet ess/obs_total 100
    
    dservSet ess/datafile ""
    dservSet ess/lastfile ""
}


# reset all matches and triggers
triggerRemoveAll

init_vars

triggerAdd rpio/vals                   1  update_dio
triggerAdd pca9538/vals                1  update_joystick
triggerAdd ain/vals                    1  update_ain
triggerAdd mtouch/touchvals            1  update_touch
triggerAdd proc/windows/status         1  update_ain_window_status
triggerAdd proc/windows/settings       1  update_ain_window_settings
triggerAdd proc/touch_windows/status   1  update_touch_window_status
triggerAdd proc/touch_windows/settings 1  update_touch_window_settings
triggerAdd eventlog/events             1  update_events
triggerAdd ess/state                   1  update_state

set path [file dir [info nameofexecutable]]

# add windows processor for eye movements
processLoad [file join \
		 $path processors windows[info sharedlibextension]] windows
processAttach windows ain/vals windows

# add touch_windows processor for touch regions
processLoad [file join \
		 $path processors touch_windows[info sharedlibextension]] \
    touch_windows
processAttach touch_windows mtouch/touchvals touch_windows

# look for any .tcl configs in local/*.tcl
foreach f [glob [file join $dspath local trigger-*.tcl]] {
    source $f
}

# qpcsvars.tcl

puts "initializing qpcsvars"

proc update_dio { name data } {
    set eds [expr { [lindex $data 0] & 0xFFFFFFFF}]
    set lev [expr { [lindex $data 1] & 0xFFFFFFFF}]
    dservSet qpcs/dio "$eds $lev"
}

proc update_joystick { name data } {
    dservSet qpcs/joystick [expr {31-$data}]
}

proc update_touch { name data } {
    dservSet qpcs/touch "[lindex $data 0] [lindex $data 1]"
}

proc update_ain { name data } {
    set em_pnts_per_deg_x 200
    set em_pnts_per_deg_y 200
    set v1 [expr { [lindex $data 0] & 0xFFFF}]
    set v2 [expr { [lindex $data 1] & 0xFFFF}]
    set v2a [format %.3f [expr {(($v2-2048.)/$em_pnts_per_deg_y)}]]
    set v1a [format %.3f [expr {((2048.-$v1)/$em_pnts_per_deg_x)}]]
    dservSet qpcs/em_pos "$v2 $v1 $v2a $v1a"
}

proc update_events { name data } {
    set einfo [split $name :]
    set type [lindex $einfo 1]
    set sub [lindex $einfo 2]

    switch $type { 
	3 { switch $sub {
	    0 { dservSet qpcs/state Running }
	    1 { dservSet qpcs/state Stopped }
	    4 { dservSet qpcs/state Stopped }
	    2 { dservSet qpcs/reset 1 }
	  }
	}
	7 { switch $sub {
	    0 { dservSet qpcs/state Stopped }
	    1 { dservSet qpcs/state Running }
	    2 { dservSet qpcs/state Inactive }
	  }
	}
	18 { switch $sub {
	    0 { dservSet qpcs/system $data }
	    1 { dservSet qpcs/subject $data }
	    2 { dservSet qpcs/protocol [lindex [split $data :] 1] }
	    3 { dservSet qpcs/variant [lindex [split $data :] 2] }
	}
	    dservSet qpcs/state Stopped
	}
	19 {
	    dservSet qpcs/obs_active 1
	    dservSet qpcs/obs_id [lindex $data 0]
	    dservSet qpcs/obs_total [lindex $data 1]
	}
	20 {
	    dservSet qpcs/obs_active 0
	}
	29 {
	    if { $sub == 1 } {
		dservSet qpcs/stimtype [lindex $data 0]
	    }
	}
    }
}

proc update_ain_window_settings { name data } {
    dservSet qpcs/em_region_setting $data
}

proc update_ain_window_status { name data } {
    dservSet qpcs/em_region_status $data
}

proc update_touch_window_settings { name data } {
    dservSet qpcs/touch_region_setting $data
}

proc update_touch_window_status { name data } {
    dservSet qpcs/touch_region_status $data
}

proc init_vars { } {
    dservSet qpcs/time [now]
    dservSet qpcs/dio "0 0"
    dservSet qpcs/em_pos "2048 2048 0. 0."
    dservSet qpcs/state "inactive"
    
    dservSet qpcs/name classify
    dservSet qpcs/executable ess
    dservSet qpcs/remote 100.0.0.20
    dservSet qpcs/subject tiger
    
    dservSet qpcs/obs_active 0
    dservSet qpcs/obs_id 1
    dservSet qpcs/obs_total 100
    
    dservSet qpcs/datafile ""
    dservSet qpcs/lastfile ""
}


# reset all matches and triggers
triggerRemoveAll

init_vars

triggerAdd rpio/vals                   1  update_dio
triggerAdd pca9538/vals                1  update_joystick
triggerAdd ain/vals                    20 update_ain
triggerAdd mtouch/touchvals            1  update_touch
triggerAdd proc/windows/status         1  update_ain_window_status
triggerAdd proc/windows/settings       1  update_ain_window_settings
triggerAdd proc/touch_windows/status   1  update_touch_window_status
triggerAdd proc/touch_windows/settings 1  update_touch_window_settings
triggerAdd eventlog/events             1  update_events

set path [file dir [info nameofexecutable]]

# add windows processor for eye movements
processLoad [file join $path processors windows[info sharedlibextension]] windows
processAttach windows ain/vals windows

# add touch_windows processor for touch regions
processLoad [file join $path processors touch_windows[info sharedlibextension]] touch_windows
processAttach touch_windows mtouch/touchvals touch_windows

#
# qpcssvars.tcl
#  format and update datapoints related to qpcs from GPIO and DAQ devices
#
puts "loading qpcsvars.tcl"

proc update_dio {} {
    set eds [expr { [lindex $::dpoint(data) 0] & 0xFFFFFFFF}]
    set lev [expr { [lindex $::dpoint(data) 1] & 0xFFFFFFFF}]
    triggerSet qpcs/dio "$eds $lev"
}

proc update_joystick {} {
    triggerSet qpcs/joystick [expr {31-$::dpoint(data)}]
}

proc update_ain {} {
    set em_pnts_per_deg_x 200
    set em_pnts_per_deg_y 200
    set v1 [expr { [lindex $::dpoint(data) 0] & 0xFFFF}]
    set v2 [expr { [lindex $::dpoint(data) 1] & 0xFFFF}]
    set v2a [format %.3f [expr {(($v2-2048.)/$em_pnts_per_deg_y)}]]
    set v1a [format %.3f [expr {((2048.-$v1)/$em_pnts_per_deg_x)}]]
    triggerSet qpcs/em_pos "$v2 $v1 $v2a $v1a"
}

proc update_ain_window_settings {} {
    triggerSet qpcs/em_region_setting $::dpoint(data)
}

proc update_events {} {
    set einfo [split $::dpoint(varname) :]
    set type [lindex $einfo 1]
    set sub [lindex $einfo 2]
    switch $type { 
	3 { switch $sub {
	    0 { triggerSet qpcs/state Running }
	    1 { triggerSet qpcs/state Stopped }
	    4 { triggerSet qpcs/state Stopped }
	  }
	}
	7 { switch $sub {
	    0 { triggerSet qpcs/state Stopped }
	    1 { triggerSet qpcs/state Running }
	    2 { triggerSet qpcs/state Inactive }
	  }
	}
	18 { switch $sub {
	      0 { triggerSet qpcs/system $::dpoint(data) }
	      1 { triggerSet qpcs/subject $::dpoint(data) }
	     }
	     triggerSet qpcs/state Stopped
	}
	19 {
	    triggerSet qpcs/obs_active 1
	    triggerSet qpcs/obs_id [lindex $::dpoint(data) 0]
	    triggerSet qpcs/obs_total [lindex $::dpoint(data) 1]
	}
	20 {
	    triggerSet qpcs/obs_active 0
	}
    }
}

proc update_ain_window_status {} {
    triggerSet qpcs/em_region_status $::dpoint(data)
}

proc init_vars { } {
    triggerSet qpcs/time [clock format [clock seconds]]
    triggerSet qpcs/dio "0 0"
    triggerSet qpcs/em_pos "2048 2048 0. 0."
    triggerSet qpcs/state "inactive"
    
    triggerSet qpcs/name classify
    triggerSet qpcs/executable ess
    triggerSet qpcs/remote 100.0.0.20
    triggerSet qpcs/subject tiger
    
    triggerSet qpcs/obs_active 0
    triggerSet qpcs/obs_id 1
    triggerSet qpcs/obs_total 100
    
    triggerSet qpcs/datafile ""
    triggerSet qpcs/lastfile ""
}


# reset all matches and triggers
triggerClearAll

init_vars

triggerAddMatch         rpio/vals 1
triggerAddTrigger       rpio/vals
triggerAddTriggerScript rpio/vals update_dio

triggerAddMatch         pca9538/vals 1
triggerAddTrigger       pca9538/vals
triggerAddTriggerScript pca9538/vals update_joystick

triggerAddMatch         ain/vals  20
triggerAddTrigger       ain/vals
triggerAddTriggerScript ain/vals  update_ain

triggerAddMatch         ain/proc/windows/settings 1
triggerAddTrigger       ain/proc/windows/settings
triggerAddTriggerScript ain/proc/windows/settings update_ain_window_settings

triggerAddMatch         ain/proc/windows/status 1
triggerAddTrigger       ain/proc/windows/status
triggerAddTriggerScript ain/proc/windows/status update_ain_window_status

triggerAddMatch         eventlog/events 1
triggerAddTrigger       eventlog/events
triggerAddTriggerScript eventlog/events update_events

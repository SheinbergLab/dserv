# qpcsvars.tcl

puts "initializing qpcsvars"

proc update_dio {} {
    set eds [expr { [lindex [triggerData] 0] & 0xFFFFFFFF}]
    set lev [expr { [lindex [triggerData] 1] & 0xFFFFFFFF}]
    dservSet qpcs/dio "$eds $lev"
}

proc update_joystick {} {
    dservSet qpcs/joystick [expr {31-[triggerData]}]
}

proc update_ain {} {
    set em_pnts_per_deg_x 200
    set em_pnts_per_deg_y 200
    set v1 [expr { [lindex [triggerData] 0] & 0xFFFF}]
    set v2 [expr { [lindex [triggerData] 1] & 0xFFFF}]
    set v2a [format %.3f [expr {(($v2-2048.)/$em_pnts_per_deg_y)}]]
    set v1a [format %.3f [expr {((2048.-$v1)/$em_pnts_per_deg_x)}]]
    dservSet qpcs/em_pos "$v2 $v1 $v2a $v1a"
}

proc update_ain_window_settings {} {
    dservSet qpcs/em_region_setting [triggerData]
}

proc update_events {} {

    set einfo [split [triggerName] :]
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
	      0 { dservSet qpcs/system [triggerData] }
	      1 { dservSet qpcs/subject [triggerData] }
	     }
	     dservSet qpcs/state Stopped
	}
	19 {
	    dservSet qpcs/obs_active 1
	    dservSet qpcs/obs_id [lindex [triggerData] 0]
	    dservSet qpcs/obs_total [lindex [triggerData] 1]
	}
	20 {
	    dservSet qpcs/obs_active 0
	}
    }
}

proc update_ain_window_status {} {
    dservSet qpcs/em_region_status [triggerData]
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

triggerAdd rpio/vals                 1  update_dio
triggerAdd pca9538/vals              1  update_joystick
triggerAdd ain/vals                  20 update_ain
triggerAdd ain/proc/windows/settings 1  update_ain_window_settings
triggerAdd ain/proc/windows/status   1  update_ain_window_status
triggerAdd eventlog/events           1  update_events

	
processLoad processors/up_down_left_right.so udlr
processAttach udlr ain/vals udlr

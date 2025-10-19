#
# Handle em incoming data
#

set dspath [file dir [info nameofexecutable]]
set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

package require dlsh
package require yajltcl

namespace eval em {
    # parameters for converting from raw values to [0,4095] (2^12)
    # raw_center_h/v define what p1-p4 pixel difference corresponds to center gaze (2048)
    # scale_h/v define sensitivity (output units per pixel of p1-p4 difference)
    variable settings [dict create \
			   scale_h 1 \
			   scale_v 1 \
			   raw_center_h 0.0 \
			   raw_center_v 0.0 \
			   invert_h 0 \
			   invert_v 0 \
			   to_deg_h 8.0 \
			   to_deg_v 8.0]
    
    # Track current raw values for "set center" functionality
    variable current_raw_h 0.0
    variable current_raw_v 0.0

    proc update_settings {} {
	variable settings
	dservSet em/settings $settings
    }

    # set param values in our settings directory by name
    proc set_param {param_name value} {
        variable settings
        dict set settings $param_name $value
        update_settings
    }
    
    proc set_scale_h {s} { set_param scale_h $s }
    proc set_scale_v {s} { set_param scale_v $s }
    proc set_raw_center_h {o} { set_param raw_center_h $o }
    proc set_raw_center_v {o} { set_param raw_center_v $o }
    proc set_invert_h {o} { set_param invert_h $o }
    proc set_invert_v {o} { set_param invert_v $o }
    proc set_to_deg_h {d} { set_param to_deg_h $d }
    proc set_to_deg_v {d} { set_param to_deg_v $d }

    # Set current eye position as center - call this when subject is fixating center
    proc set_current_as_center {} {
        variable current_raw_h
        variable current_raw_v
        
        set_raw_center_h $current_raw_h
        set_raw_center_v $current_raw_v
    }

    proc process { dpoint data } {
        variable settings
	variable last_valid_h 2048  ;# Remember last valid position
	variable last_valid_v 2048
        variable current_raw_h
        variable current_raw_v

        lassign $data frame_id frame_time pupil_x pupil_y pupil_r p1_x p1_y p4_x p4_y \
            blink p1_detected p4_detected

	# Store raw positions
	set pupil "$pupil_x $pupil_y"
	set p1 "$p1_x $p1_y"
	set p4 "$p4_x $p4_y"
	foreach v "pupil p1 p4" {
	    set fvals [binary format "ff" [set ${v}_x] [set ${v}_y]]
	    dservSetData em/$v 0 2 $fvals
	}
	set fvals [binary format "f" $pupil_r]
	dservSetData em/pupil_r 0 2 $fvals
	
	set seconds_binary [binary format d $frame_time]
	dservSetData em/time 0 3 $seconds_binary
	
	foreach v "blink p1_detected p4_detected" {
	    set val [binary format c [expr {int([set $v])}]]
	    dservSetData em/$v 0 0 $val
	}
	
	# Only compute eye position if BOTH reflections detected
	if {$p1_detected > 0 && $p4_detected > 0} {
	    # Calculate raw eye position in pixels (P1-P4 difference)
	    set raw_h [expr {$p1_x - $p4_x}]
	    set raw_v [expr {$p1_y - $p4_y}]
	    
	    # Store for "set center" functionality
	    set current_raw_h $raw_h
	    set current_raw_v $raw_v
	    
	    # Apply scaling relative to center point, then add offset to 2048
	    dict with settings {
		if { $invert_h } {
		    set s_h [expr {-1*$scale_h}]
		} else {
		    set s_h $scale_h
		}
		
		if { $invert_v } {
		    set s_v [expr {-1*$scale_v}]
		} else {
		    set s_v $scale_v
		}
		
		set h [expr {int(($s_h * ($raw_h - $raw_center_h)) + 2048)}]
		set v [expr {int(($s_v * ($raw_v - $raw_center_v)) + 2048)}]
	    }
	    
	    # Remember valid position
	    set last_valid_h $h
	    set last_valid_v $v
	} else {
	    # Use last valid position (hold) or center
	    set h $last_valid_h
	    set v $last_valid_v
	}
	
	# Send ain/vals (even if held)
	set ainvals [binary format ss $v $h]
	dservSetData ain/vals 0 4 $ainvals
	
	# Compute degrees
	dict with settings {
	    set h_deg [expr {(2048. - $h) / $to_deg_h}]
	    set v_deg [expr {($v - 2048.) / $to_deg_v}]
	}

	dservSet ess/em_pos "$v $h $h_deg $v_deg"
    }    

    # Process virtual eye data (already in ADC format [0-4095])
    proc process_virtual { dpoint data } {
        variable settings
        
        lassign $data adc_v adc_h
        
        # Virtual data is already in ADC space, just pass through
        set ainvals [binary format ss $adc_v $adc_h]
        dservSetData ain/vals 0 4 $ainvals
        
        # Compute degrees for visualization
	dict with settings {
	    set h_deg [expr {($adc_h - 2048.) / $to_deg_h}]
	    set v_deg [expr {(2048. - $adc_v) / $to_deg_v}]
	}	
        
        dservSet ess/em_pos "$adc_v $adc_h $h_deg $v_deg"
    }
    
    update_settings
}

dservAddExactMatch eyetracking/virtual
dpointSetScript    eyetracking/virtual em::process_virtual

dservAddExactMatch eyetracking/results
dpointSetScript    eyetracking/results em::process

puts "Eye movement subprocessor started"

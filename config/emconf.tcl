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
    variable settings [dict create \
			   scale_h 2 \
			   scale_v 2 \
			   offset_h 2000 \
			   offset_v 2000 \
			   invert_h 0 \
			   invert_v 0]

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
    proc set_offset_h {o} { set_param offset_h $o }
    proc set_offset_v {o} { set_param offset_v $o }
    proc set_invert_h {o} { set_param invert_h $o }
    proc set_invert_v {o} { set_param invert_v $o }

    proc process { dpoint data } {
        variable settings
        variable to_deg_h 200.
        variable to_deg_v 200.
	variable last_valid_h 2048  ;# Remember last valid position
	variable last_valid_v 2048

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
	    
	    # Apply scaling and offset for ain/vals
	    dict with settings {
		if { $invert_h } { set s_h [expr {-1*$scale_h}] } { set s_h $scale_h }
		if { $invert_v } { set s_v [expr {-1*$scale_v}] } { set s_v $scale_v }
		
		set h [expr {int(($s_h * $raw_h) + $offset_h)}]
		set v [expr {int(($s_v * $raw_v) + $offset_v)}]
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
	set h_deg [expr {(2048. - $h) / $to_deg_h}]
	set v_deg [expr {($v - 2048.) / $to_deg_v}]

	dservSet ess/em_pos "$v $h $h_deg $v_deg"
    }    
    
    update_settings
}

dservAddExactMatch eyetracking/results
dpointSetScript    eyetracking/results em::process

puts "Eye movement subprocessor started"

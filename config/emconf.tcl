#
# Handle em incoming data - degrees output with calibration
#

set dspath [file dir [info nameofexecutable]]
set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

package require dlsh
package require yajltcl

package require math::linearalgebra

namespace eval biquadratic {
    namespace import ::math::linearalgebra::*
    
    proc create_design_matrix {x_coords y_coords} {
        set n [llength $x_coords]
        set A {}
        
        for {set i 0} {$i < $n} {incr i} {
            set x [lindex $x_coords $i]
            set y [lindex $y_coords $i]
            
            set x2 [expr {$x * $x}]
            set y2 [expr {$y * $y}]
            set xy [expr {$x * $y}]
            set x2y [expr {$x2 * $y}]
            set xy2 [expr {$x * $y2}]
            set x2y2 [expr {$x2 * $y2}]
            
            lappend A [list 1.0 $x $y $x2 $y2 $xy $x2y $xy2 $x2y2]
        }
        return $A
    }
    
    proc fit_single {x_coords y_coords target_values} {
        set n [llength $x_coords]
        set A [create_design_matrix $x_coords $y_coords]
        
        set z_vector {}
        foreach z $target_values {
            lappend z_vector [list $z]
        }
        
        if {$n > 9} {
            set At [transpose $A]
            set AtA [matmul $At $A]
            set Atz [matmul $At $z_vector]
            set coeffs [solveGauss $AtA $Atz]
        } else {
            set coeffs [solveGauss $A $z_vector]
        }
        
        set coeff_list {}
        foreach row $coeffs {
            lappend coeff_list [lindex $row 0]
        }
        return $coeff_list
    }
    
    proc fit {raw_x raw_y target_x target_y} {
        set x_coeffs [fit_single $raw_x $raw_y $target_x]
        set y_coeffs [fit_single $raw_x $raw_y $target_y]
        return [list $x_coeffs $y_coeffs]
    }
}

namespace eval em {
    proc do_fit { raw_x raw_y target_x target_y } {
	lassign [biquadratic::fit $raw_x $raw_y $target_x $target_y] x_coeffs y_coeffs
	set_bq_h_coeffs $x_coeffs
	set_bq_v_coeffs $y_coeffs
    }

    # Parameters for converting from raw Purkinje reflection differences to degrees
    # Linear model: degrees = scale * (raw_diff - raw_center)
    # For biquadratic: add higher-order terms after running calibration
    variable settings [dict create \
        scale_h 1.0 \
        scale_v 1.0 \
        raw_center_h 0.0 \
        raw_center_v 0.0 \
        invert_h 0 \
        invert_v 0 \
        use_biquadratic 0 \
        bq_h_coeffs {0 0 0 0 0 0 0 0 0} \
        bq_v_coeffs {0 0 0 0 0 0 0 0 0}]
    
    # Track current raw values for "set center" functionality
    variable current_raw_h 0.0
    variable current_raw_v 0.0
    
    # Track last valid position in degrees
    variable last_valid_h 0.0
    variable last_valid_v 0.0

    proc update_settings {} {
        variable settings
        dservSet em/settings $settings
    }

    # Set param values in our settings directory by name
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
    proc set_use_biquadratic {b} { set_param use_biquadratic $b }
    proc set_bq_h_coeffs {coeffs} { 
        if {[llength $coeffs] != 9} {
            error "bq_h_coeffs requires 9 coefficients"
        }
        set_param bq_h_coeffs $coeffs 
    }
    proc set_bq_v_coeffs {coeffs} { 
        if {[llength $coeffs] != 9} {
            error "bq_v_coeffs requires 9 coefficients"
        }
        set_param bq_v_coeffs $coeffs 
    }

    # Set current eye position as center - call this when subject is fixating center
    proc set_current_as_center {} {
        variable current_raw_h
        variable current_raw_v
        
        set_raw_center_h $current_raw_h
        set_raw_center_v $current_raw_v
    }

    # Convert raw sub-pixel difference to degrees using full biquadratic fit
    # deg = c0 + c1*x + c2*y + c3*x² + c4*y² + c5*xy + c6*x²y + c7*xy² + c8*x²y²
    proc biquadratic_transform {x y coeffs} {
        lassign $coeffs c0 c1 c2 c3 c4 c5 c6 c7 c8
        set x2 [expr {$x * $x}]
        set y2 [expr {$y * $y}]
        set xy [expr {$x * $y}]
        return [expr {$c0 + $c1*$x + $c2*$y + $c3*$x2 + $c4*$y2 + $c5*$xy + 
                      $c6*$x2*$y + $c7*$x*$y2 + $c8*$x2*$y2}]
    }

    proc process { dpoint data } {
        variable settings
        variable last_valid_h
        variable last_valid_v
        variable current_raw_h
        variable current_raw_v

        lassign $data frame_id frame_time pupil_x pupil_y pupil_r \
            p1_x p1_y p4_x p4_y \
            blink p1_detected p4_detected

        # Get timestamp to use for all the eye data
        set cur_t [now]

        # Store raw detection data as floats (sub-pixel positions)
        set pupil "$pupil_x $pupil_y"
        set p1 "$p1_x $p1_y"
        set p4 "$p4_x $p4_y"
        foreach v "pupil p1 p4" {
            set fvals [binary format "ff" [set ${v}_x] [set ${v}_y]]
            dservSetData em/$v $cur_t 2 $fvals  ;# type 2 = DSERV_FLOAT
        }
        set fvals [binary format "f" $pupil_r]
        dservSetData em/pupil_r $cur_t 2 $fvals

        set frame_id_binary [binary format i [expr {int($frame_id)}]]
        dservSetData em/frame_id $cur_t 5 $frame_id_binary

        set seconds_binary [binary format d $frame_time]
        dservSetData em/time $cur_t 3 $seconds_binary
                      
        foreach v "blink p1_detected p4_detected" {
            set val [binary format c [expr {int([set $v])}]]
            dservSetData em/$v $cur_t 0 $val
        }
        
        # Only compute eye position if BOTH reflections detected
        dict with settings {
            if {$p1_detected > 0 && $p4_detected > 0} {
                # Calculate raw eye position in sub-pixels (P1-P4 difference)
                set raw_h [expr {$invert_h ? ($p1_x-$p4_x) : ($p4_x-$p1_x)}]
                set raw_v [expr {$invert_v ? ($p1_y-$p4_y) : ($p4_y-$p1_y)}]
                
                # Store for "set center" functionality
                set current_raw_h $raw_h
                set current_raw_v $raw_v
                
                # Convert to degrees
                if {$use_biquadratic} {
                    # Biquadratic fit: deg = f(raw_h, raw_v)
                    set h_deg [biquadratic_transform $raw_h $raw_v $bq_h_coeffs]
                    set v_deg [biquadratic_transform $raw_h $raw_v $bq_v_coeffs]
                } else {
                    # Simple linear mapping: degrees = scale * (raw - center)
                    # scale is in degrees per sub-pixel
                    set h_deg [expr {$scale_h * ($raw_h - $raw_center_h)}]
                    set v_deg [expr {$scale_v * ($raw_v - $raw_center_v)}]
                }
                
                # Remember valid position
                set last_valid_h $h_deg
                set last_valid_v $v_deg
            } else {
                # Use last valid position (hold during blinks)
                set h_deg $last_valid_h
                set v_deg $last_valid_v
            }
        }
        
        # Send eye position in degrees as floats [h, v] convention
        set eyevals [binary format ff $h_deg $v_deg]
        dservSetData eyetracking/position $cur_t 2 $eyevals ;# 2 = DSERV_FLOAT
        
        # Also send raw differences for calibration/debugging
        set rawvals [binary format ff $current_raw_h $current_raw_v]
        dservSetData eyetracking/raw $cur_t 2 $rawvals
        
        # Send to visualization/monitoring
        dservSet ess/em_pos "$h_deg $v_deg"
    }    

    # Process virtual eye data (already in degrees)
    proc process_virtual { dpoint data } {
        lassign $data h_deg v_deg
        
        # Virtual data is already in degrees, just pass through
        set eyevals [binary format ff $h_deg $v_deg]
        dservSetData eyetracking/position 0 2 $eyevals

	# We don't have real "raw" values so just use actual
        dservSetData eyetracking/raw 0 2 $eyevals
        
        # Also send to visualization
        dservSet ess/em_pos "$h_deg $v_deg"
    }
    
    update_settings
}

dservAddExactMatch eyetracking/virtual
dpointSetScript    eyetracking/virtual em::process_virtual

dservAddExactMatch eyetracking/results
dpointSetScript    eyetracking/results em::process

puts "Eye movement subprocessor started"

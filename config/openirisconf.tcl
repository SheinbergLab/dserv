#
# Handle openiris incoming data
#

set dspath [file dir [info nameofexecutable]]
set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

package require dlsh
package require yajltcl

namespace eval openiris {
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
	dservSet openiris/settings $settings
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
	
	lassign $data frame seconds \
	    r_pupil_x r_pupil_y \
	    r_cr1_x r_cr1_y \
	    r_cr4_x r_cr4_y \
	    int0 int1
	
	set r_pupil "$r_pupil_x $r_pupil_y"
	set r_cr1 "$r_cr1_x $r_cr1_y"
	set r_cr4 "$r_cr4_x $r_cr4_y"

	foreach v "pupil cr1 cr4" {
	    set fvals [binary format "ff" [set r_${v}_x] [set r_${v}_y]]
	    dservSetData openiris/right/$v 0 2 $fvals
	}
	
	set seconds_binary [binary format d $seconds]
	dservSetData openiris/time 0 3 $fvals

	foreach v "frame int0 int1" {
	    set ival [binary format i [expr {int([set $v])}]]
	    dservSetData openiris/$v 0 5 $ival
	}

	# do actual conversion using params in settings
	dict with settings {
	    # set ain/vals as shorts to be compatible with other eye inputs
	    if { $invert_h } { set s_h [expr {-1*$scale_h}] } { set s_h $scale_h }
	    if { $invert_v } { set s_v [expr {-1*$scale_v}] } { set s_v $scale_v }
	    set h [expr {int(($s_h*($r_cr1_x-$r_cr4_x))+$offset_h)}]
	    set v [expr {int(($s_v*($r_cr1_y-$r_cr4_y))+$offset_v)}]
	}

	# convert vals to binary and set the ain/vals points as two shorts
	set ainvals [binary format ss $v $h]
	dservSetData ain/vals 0 4 $ainvals
	
	set h_deg [expr {(2048.-$h)/$to_deg_h}]
	set v_deg [expr {($v-2048.)/$to_deg_v}]

	# because of old ADC ordering, v comes before h for raw values
	dservSet ess/em_pos "$v $h $h_deg $v_deg"
    }

    update_settings
}

dservAddExactMatch openiris/frameinfo
dpointSetScript    openiris/frameinfo openiris::process

puts "Openiris subprocessor started"

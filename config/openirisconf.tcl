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
			   offset_v 2000]

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

	dservSet openiris/right/pupil $r_pupil
	dservSet openiris/right/cr1   $r_cr1
	dservSet openiris/right/cr4   $r_cr4
	dservSet openiris/frame       [expr {int($frame)}]
	dservSet openiris/time        $seconds
	dservSet openiris/int0        [expr {int($int0)}]
	dservSet openiris/int1        [expr {int($int1)}]

	# do actual conversion using params in settings
	dict with settings {
	    # set ain/vals as shorts to be compatible with other eye inputs
	    dl_local avals [dl_reverse [dl_short \
					    [dl_add \
						 "$offset_h $offset_v" \
						 [dl_mult \
						      [dl_sub $r_cr1 $r_cr4] \
						      "$scale_h $scale_v"]]]]
	}

	# convert vals to binary and set the ain/vals points as two shorts
	dl_toString $avals ainvals
	dservSetData ain/vals 0 4 $ainvals
	
	lassign [dl_tcllist $avals] h v
	set h [expr {(2048.-$h)/$to_deg_h}]
	set v [expr {($v-2048.)/$to_deg_v}]
	
	dservSet ess/em_pos "[dl_tcllist $avals] $h $v"
    }

    update_settings
}

dservAddExactMatch openiris/frameinfo
dpointSetScript    openiris/frameinfo openiris::process

puts "Openiris subprocessor started"

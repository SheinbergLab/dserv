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
    variable scale_h 2
    variable scale_v 2
    variable offset_h 2000
    variable offset_v 2000
    
    proc process { dpoint data } {
	variable scale_h
	variable scale_v
	variable offset_h
	variable offset_v

	variable to_deg_h 200.
	variable to_deg_v 200.
	
	lassign $data frame seconds \
	    r_pupil_x r_pupil_y \
	    r_cr1_x r_cr1_y \
	    r_cr4_x r_cr4_y \
	    extra_1 extra_2

	set r_pupil "$r_pupil_x $r_pupil_y"
	set r_cr1 "$r_cr1_x $r_cr1_y"
	set r_cr4 "$r_cr4_x $r_cr4_y"
	dservSet openiris/right/pupil $r_pupil
	dservSet openiris/right/cr1   $r_cr1
	dservSet openiris/right/cr4   $r_cr4
	dservSet openiris/frame       [expr {int($frame)}]
	dservSet openiris/time        $seconds
	dservSet openiris/extra       [expr {int($extra_2)*2+int($extra_1)}]
	
	# set ain/vals as shorts to be compatible with other eye inputs
	dl_local avals [dl_reverse [dl_short \
			 [dl_add \
			      "$offset_h $offset_v" \
			      [dl_mult \
				   [dl_sub $r_cr1 $r_cr4] \
				   "$scale_h $scale_v"]]]]
	dl_toString $avals ainvals
	dservSetData ain/vals 0 4 $ainvals

	lassign [dl_tcllist $avals] h v
	set h [expr {(2048.-$h)/$to_deg_h}]
	set v [expr {($v-2048.)/$to_deg_v}]

	dservSet ess/em_pos "[dl_tcllist $avals] $h $v"
    }
}

dservAddExactMatch openiris/frameinfo
dpointSetScript    openiris/frameinfo openiris::process

puts "Openiris subprocessor started"

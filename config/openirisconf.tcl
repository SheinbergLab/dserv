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
    variable scale_h 15
    variable scale_v 15
    variable offset_h 2000
    variable offset_v 2000
    
    proc process { dpoint data } {
	variable scale_h
	variable scale_v
	variable offset_h
	variable offset_v

	variable to_deg_h 200.
	variable to_deg_v 200.
	
	set d [yajl::json2dict $data]
	set left [dict get $d Left]
	set pupil [dict get [dict get $left Pupil] Center]
	set l_pupil [list [dict get $pupil X] [dict get $pupil Y]] 
	set crs [dict get $left CRs]
	set cr1 [lindex $crs 0]
	set l_cr1 [list [dict get $cr1 X] [dict get $cr1 Y]]
	set cr4 [lindex $crs 3]
	set l_cr4 [list [dict get $cr4 X] [dict get $cr4 Y]]
	
	dservSet openiris/left/pupil $l_pupil
	dservSet openiris/left/cr1   $l_cr1
	dservSet openiris/left/cr4   $l_cr4

	# set ain/vals as shorts to be compatible with other eye inputs
	dl_local avals [dl_reverse [dl_short \
			 [dl_add \
			      "$offset_h $offset_v" \
			      [dl_mult \
				   [dl_sub $l_cr1 $l_cr4] \
				   "$scale_h $scale_v"]]]]
	dl_toString $avals ainvals
	dservSetData ain/vals 0 4 $ainvals

	lassign [dl_tcllist $avals] h v
	set h [expr {2048.-$h}/$to_deg_h]
	set v [expr {$v-2048.}/$to_deg_y]

	dservSet ess/em_pos "[dl_tcllist $avals] $h $v"
    }
}

dservAddExactMatch openiris/frameinfo
dpointSetScript    openiris/frameinfo openiris::process

puts "Openiris subprocessor started"

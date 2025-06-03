#
# Handle openiris incoming data
#

set dspath [file dir [info nameofexecutable]]
set base [file join [zipfs root] dlsh]
set auto_path [linsert $auto_path [set auto_path 0] $base/lib]

package require dlsh
package require yajltcl

namespace eval openiris {
    proc process { dpoint data } {
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
    }
}

dservAddExactMatch openiris/frameinfo
dservSetScript  openiris/frameinfo openiris::process

puts "Openiris subprocessor started"

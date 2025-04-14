# -*- mode: tcl -*-

#
# haptic.tcl
#   tools for managing haptic shape data
#

package require dlsh
package require sqlite3

namespace eval haptic {
    variable shape_ids "2041 2013 2037 2036 2021 2015 2067 2092 2066 2019 \
   	                2035 2099 2070 2060 2050 2027 2051 2022 2065 2064 \
                        2014 2031 2023 2058 2087 2007 2018 2062 2032 2033 \
                        2030 2053 2016 2069 2024 2082 2074 2102 2061"

    # Given a source shape_list and target shape_ids, return indices of targets
    proc get_shape_indices { shape_list shape_ids } {
#	dl_return [dl_findIndices $shape_list $shape_ids]
	dl_local inds [dl_ilist]
	foreach s [dl_tcllist $shape_ids] {
	    dl_append $inds [dl_find $shape_list $s]
	}
	dl_return $inds
    }

    # Create shape database give shape ids by extracting coords
    proc create_shape_db { db } {
	variable shape_ids
	set dg [dg_create]
	dl_set $dg:id [dl_ilist]
	dl_set $dg:x  [dl_llist]
	dl_set $dg:y  [dl_llist]

	set dbfile $db
	
	if {![file exists $dbfile]} { error "db file not found" }
	sqlite3 hapticvis_shapedb $dbfile -readonly 1
	
	# get coords for each shape
	dl_local coord_x [dl_llist]
	dl_local coord_y [dl_llist]
	foreach s $shape_ids {
	    dl_append $dg:id $s
	    
	    set c [hapticvis_shapedb eval "SELECT x,y FROM shapeTable${s}"]
	    dl_local reshaped \
		[dl_transpose [dl_reshape [dl_flist {*}$c] - 2]]
	    dl_append $dg:x $reshaped:0
	    dl_append $dg:y $reshaped:1
	}
	
	# close the sqlite3 db
	hapticvis_shapedb close

	return $dg
    }

    # create persistent identity trials for subjects
    proc add_identity_trials { shapedb_file trialdb_file n { n_per_set 4 } { n_sets 4 } } {

	# shape coords are in shapedb_file
	if {![file exists $shapedb_file]} { error "db file not found" }
	if { [dg_exists shapedb] } { dg_delete shapedb }
	dg_rename [dg_read $shapedb_file] shapedb
	
	# trial info in trialdb_file
	if { [dg_exists trialdb] } { dg_delete trialdb }
	if {![file exists $trialdb_file]} {
	    dg_rename [dg_create] trialdb
	    dl_set trialdb:subject    [dl_ilist]
	    dl_set trialdb:target_ids [dl_llist]
	    dl_set trialdb:dist_ids   [dl_llist]
	} else {
	    dg_rename [dg_read $trialdb_file] trialdb
	}
	
	for { set subject_id 0 } { $subject_id < $n } { incr subject_id } {
	    set row [dl_find trialdb:subject $subject_id]
	    
	    # only add if row not in table
	    if { $row < 0 } {
		dl_local shuffled [dl_shuffle shapedb:id]
		set ntargs [expr $n_per_set*$n_sets]
		set ndists $ntargs
		if { [expr {$ndists+$ntargs>[dl_length shapedb:id]}] } {
		    error "not enough shapes in db"
		}
		dl_local ts [dl_choose $shuffled [dl_fromto 0 $ntargs]]
		dl_local ds [dl_choose $shuffled \
				 [dl_fromto $ntargs [expr {$ntargs+$ndists}]]]
		dl_append trialdb:subject $subject_id
		dl_append trialdb:target_ids [dl_reshape $ts $n_sets $n_per_set]
		dl_append trialdb:dist_ids [dl_reshape $ds $n_sets $n_per_set]
	    }
	}
	dg_write trialdb $trialdb_file
	dg_delete trialdb
    }

    # create persistent contour trials for subjects
    proc add_contour_trials { shapedb_file trialdb_file n { n_per_set 4 } { n_sets 8 } } {

	# shape coords are in shapedb_file
	if {![file exists $shapedb_file]} { error "db file not found" }
	if { [dg_exists shapedb] } { dg_delete shapedb }
	dg_rename [dg_read $shapedb_file] shapedb
	
	# trial info in trialdb_file
	if { [dg_exists trialdb] } { dg_delete trialdb }
	if {![file exists $trialdb_file]} {
	    dg_rename [dg_create] trialdb
	    dl_set trialdb:subject    [dl_ilist]
	    dl_set trialdb:target_ids [dl_llist]
	} else {
	    dg_rename [dg_read $trialdb_file] trialdb
	}
	
	for { set subject_id 0 } { $subject_id < $n } { incr subject_id } {
	    set row [dl_find trialdb:subject $subject_id]
	    
	    # only add if row not in table
	    if { $row < 0 } {
		dl_local shuffled [dl_shuffle shapedb:id]
		set ntargs [expr $n_per_set*$n_sets]
		if { [expr {$ntargs>[dl_length shapedb:id]}] } {
		    error "not enough shapes in db"
		}
		dl_local ts [dl_choose $shuffled [dl_fromto 0 $ntargs]]
		dl_append trialdb:subject $subject_id
		dl_append trialdb:target_ids [dl_reshape $ts $n_sets $n_per_set]
	    }
	}
	dg_write trialdb $trialdb_file
	dg_delete trialdb
    }

    proc haptic_process_available { dpoint data } {
	dservSet ess/grasp/available $data
	::ess::do_update
    }

    # listen for available message and trigger update
    proc init {} {
	dservAddExactMatch grasp/available
	dpointSetScript grasp/available ::haptic::haptic_process_available
	dservSet ess/grasp/available 0
    }
    
}





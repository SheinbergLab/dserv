#
# VARIANTS
#   match_to_sample phd
#
# DESCRIPTION
#   variant dictionary
#

namespace eval match_to_sample::phd {

    # state system parameters
    variable params_defaults { delay_time 100 }
    variable params_VV { sample_time 2000 }
    variable params_HV { sample_time 10000 }

    
    # variant description
    # find database
    set db {}
    set paths [list \
		   /shared/qpcs/stimuli/graspomatic/Grasp3Shapes.db \
		   ${::ess::system_path}/$::ess::current(project)/match_to_sample/phd/data/Grasp3Shapes.db]
    foreach p $paths {
	if [file exists $p] { set db $p; break }
    }

    variable variants {
	VV {
	    description "visual visual shape MTS"	    
	    loader_proc setup_trials
	    loader_options {
		dbfile { { Grasp3Shapes.db $db } }
		trial_type VV
		filled 1
		limit -1
	    }
	}
	HV {
	    description "haptic visual shape MTS"	    
	    loader_proc setup_trials {
		dbfile { { Grasp3Shapes.db $db } }
		trial_type HV
		filled 1
		limit -1
	    }
	}
    }

    # substitute variables ($db) in variant description above
    set variants [subst $variants]
    
    proc variants_init { s } {

        $s add_method VV_init {} {
            rmtSend "setBackground 100 200 100"
        }

        $s add_method HV_init {} {
            rmtSend "setBackground 100 100 100"
        }

	#
	# extract qrs representation for a given shape
	#
	$s add_method get_shape_qrs { db shapeID filled { displayID 51 } } {
    
	    # extract qrs hex location for each pin
	    set sqlcmd { SELECT q,r,s FROM pistonAddressesTable \
			     WHERE DisplayID=$displayID }
	    set qrsvals [$db eval $sqlcmd]
	    dl_local qrs [dl_reshape [dl_ilist {*}$qrsvals] - 3]
	    
	    # get pin bit representation for this shape
	    if { $filled } {
		set sqlcmd {SELECT hex(shapeFilled) FROM shapeTable \
				WHERE shapeID=$shapeID and DisplayID=$displayID}
	    } else {
		set sqlcmd {SELECT hex(shapeOutline) FROM shapeTable \
				WHERE shapeID=$shapeID and DisplayID=$displayID}
	    }
	    set h [$db eval $sqlcmd]
	    
	    # unpack blob into 32 bits words then converted to bits
	    set hex [join [lreverse [join [regexp -all -inline .. $h]]] ""]
	    set word0 [string range $hex 0 7]
	    set word1 [string range $hex 8 15]
	    set word2 [string range $hex 16 23]
	    set word3 [string range $hex 24 31]
	    set bits [format %032b%032b%032b%032b \
			  0x${word0} 0x${word1} 0x${word2} 0x${word3}]
	    set bits [string range $bits 2 end]
	    
	    # turn bitmask into list of 1s and 0s for each pin
	    dl_local pins [dl_ilist {*}[join [regexp -all -inline . $bits]]]
	    
	    # pull qrs location for each pin that is on
	    dl_local hexpos [dl_choose $qrs [dl_indices $pins]]

	    # filled description is missing the center, so we add that here
	    if { $filled } {
		dl_append $hexpos [dl_ilist 0 0 0]
	    }
	    
	    # return list of hex positions (in qrs notation)
	    dl_return $hexpos
	}
	
	$s add_method get_shape_family { db family algo algoargs { n 4 } } {
	    
	    # find all shapes from family shapeFamily with familyAlgo = $algo
	    set sqlcmd {
		SELECT shapeID from shapeTable
		WHERE shapeFamily=$family and familyAlgo=$algo and algoArgsCSV=$algoargs
		LIMIT 4
	    }
	    set shapes [$db eval $sqlcmd]
	    
	    return $shapes
	}

	$s add_method open_grasp_db { dbname srcfile } {
	    # create an in-memory database loaded with graspdb tables
	    package require sqlite3
	    
	    sqlite3 src $srcfile -readonly true
	    sqlite3 $dbname :memory:
	    # get the schema from original table
	    set cmd {
		SELECT sql FROM sqlite_schema
		WHERE type='table'
		ORDER BY name;
	    }
	    
	    # create all tables but sqlite_sequence in memory db
	    set tables_cmd [src eval $cmd]
	    src close
	    set tables {}
	    foreach t $tables_cmd {
		set table_name [lindex $t 2]
		if { $table_name != "sqlite_sequence(name,seq)" } {
		    $dbname eval $t
		    lappend tables $table_name
		}
		
	    }
	    
	    $dbname eval {ATTACH DATABASE $srcfile AS grasp_db}
	    foreach t $tables {
		$dbname eval "INSERT INTO main.$t SELECT * FROM grasp_db.$t;"
	    }
	    
	    return $dbname
	}
	
	$s add_method setup_trials { dbfile trial_type filled limit } {

	    # build our stimdg
	    if { [dg_exists stimdg] } { dg_delete stimdg }
	    set g [dg_create stimdg]
	    dg_rename $g stimdg

	    set choice_scale   3.0
	    set choice_spacing 4.0

	    if {![file exists $dbfile]} { error "db file not found" }
	    my open_grasp_db grasp_db $dbfile
	    
	    dl_set stimdg:stimtype [dl_ilist]
	    dl_set stimdg:family [dl_ilist]
	    dl_set stimdg:distance [dl_ilist]
	    dl_set stimdg:filled [dl_ilist]
	    dl_set stimdg:sample_id [dl_ilist]
	    dl_set stimdg:sample_slot [dl_ilist]
	    dl_set stimdg:choice_ids [dl_llist]
	    dl_set stimdg:trial_type [dl_slist]
	    dl_set stimdg:sample_qrs [dl_llist]
	    dl_set stimdg:choice_qrs [dl_llist]
	    dl_set stimdg:choice_centers [dl_llist]
	    dl_set stimdg:choice_scale [dl_flist]

	    if { $limit > 0 } {
		set sqlcmd { SELECT shapeFamily from shapeTable WHERE familyAlgo="parent" and displayID=51 ORDER BY RANDOM() LIMIT $limit }
	    } else {
		set sqlcmd { SELECT shapeFamily from shapeTable WHERE familyAlgo="parent" and displayID=51 ORDER BY RANDOM() }
	    }
	    set families [grasp_db eval $sqlcmd]
	    dl_set stimdg:family [dl_ilist {*}$families]
	    set nfamilies [dl_length stimdg:family]
	    set nrep [expr ($nfamilies+3)/4]
	    dl_local distance [dl_choose [dl_replicate [dl_shuffle "2 4 8 16"] $nrep] \
				   [dl_fromto 0 $nfamilies]]
	    dl_set stimdg:distance $distance

	    foreach f $families d [dl_tcllist $distance] {
		dl_local nex [dl_ilist]
		set shape_id_list [my get_shape_family grasp_db $f add_pistons2 ${d}]
		dl_local shape_ids [dl_shuffle [dl_ilist {*}$shape_id_list]]
		dl_append stimdg:choice_ids $shape_ids
		dl_local qrs [dl_llist]
		set sample_id [dl_pickone $shape_ids]
		dl_append stimdg:sample_id $sample_id
		
		# add sample qrs
		dl_append stimdg:sample_qrs [my get_shape_qrs grasp_db $sample_id $filled]
		
		# add choice qrs
		dl_local choice_qrs [dl_llist]
		foreach choice_id [dl_tcllist $shape_ids] {
		    dl_append $choice_qrs [my get_shape_qrs grasp_db $choice_id $filled]
		}
		dl_append stimdg:choice_qrs $choice_qrs
	    }
	    
	    set n_obs [dl_length stimdg:sample_qrs]

	    dl_set stimdg:stimtype [dl_fromto 0 $n_obs]
	    dl_set stimdg:trial_type [dl_replicate [dl_slist $trial_type] $n_obs]
	    dl_set stimdg:filled [dl_repeat $filled $n_obs]
	    dl_set stimdg:sample_slot [dl_unpack \
					   [dl_indices [dl_eq stimdg:choice_ids stimdg:sample_id]]]
	    grasp_db close
	    
	    dl_set $g:choice_scale [dl_repeat $choice_scale $n_obs]
	    dl_local locations [dl_llist [dl_ilist -1 1] [dl_ilist 1 1] [dl_ilist -1 -1] [dl_ilist 1 -1]]
	    dl_local locations [dl_mult $locations $choice_spacing]
				
	    dl_set $g:choice_centers [dl_replicate [dl_llist $locations] $n_obs]
	    dl_set $g:remaining [dl_ones $n_obs]
	    return $g
	}
    }
}

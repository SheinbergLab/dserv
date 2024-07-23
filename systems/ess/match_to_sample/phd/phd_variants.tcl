#
# VARIANTS
#   match_to_sample phd
#
# DESCRIPTION
#   variant dictionary
#

namespace eval match_to_sample::phd {
    variable setup_trials_defaults {
	dbfile /usr/local/dserv/systems/ess/match_to_sample/phd/data/Grasp3ShapesRyan.db
	trials_type VV
	filled 1
	limit -1
    }
    variable setup_trials_vv { trial_type   VV }
    variable setup_trials_hv { trial_type   HV }

    variable variants {
	VV     { setup_trials vv "visual visual shape MTS" }
	HV     { setup_trials hv "haptic visual shape MTS" }
    }

    proc variants_init { s } {
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
	
	$s add_method setup_trials { dbfile trial_type filled limit } {

	    # build our stimdg
	    if { [dg_exists stimdg] } { dg_delete stimdg }
	    set g [dg_create stimdg]
	    dg_rename $g stimdg

	    set choice_scale   3.0
	    set choice_spacing 4.0

	    sqlite3 grasp_db $dbfile -readonly true
	    grasp_db timeout 5000
	    
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
	    dl_local distance [dl_choose [dl_replicate [dl_shuffle "1 2 4 8"] $nrep] \
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
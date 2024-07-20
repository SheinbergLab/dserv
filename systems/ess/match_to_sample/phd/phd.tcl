#
# PROTOCOL
#   match_to_sample phd
#
# DESCRIPTION
#   Present a target color and two choices
#

namespace eval match_to_sample::phd {
    proc protocol_init { s } {
	package require sqlite3
	
	$s set_protocol [namespace tail [namespace current]]
	
	$s add_param targ_radius         1.5    variable stim
	$s add_param targ_range          4      variable stim
	$s add_param targ_color          ".2 .9 .9" variable stim
	
	$s add_param rmt_host          $ess::rmt_host   stim ipaddr
	
	$s add_param juice_pin         27       variable int
	$s add_param juice_time      1000       time int
	
	$s add_variable cur_id            -1
	$s add_variable target_slot       -1
	
	$s set_protocol_init_callback {
	    ess::init

	    # open connection to rmt and upload ${protocol}_stim.tcl
	    my configure_stim $rmt_host

	    # initialize touch processor
	    ess::touch_init
	    
	    # configure juice channel pin
	    juicerSetPin 0 $juice_pin

	    soundReset
	    soundSetVoice 81 0    0
	    soundSetVoice 57 17   1
	    soundSetVoice 60 0    2
	    soundSetVoice 42 0    3
	    soundSetVoice 21 0    4
	    soundSetVoice 8  0    5
	    soundSetVoice 113 100 6
	    foreach i "0 1 2 3 4 5 6" { soundVolume 127 $i }
	}
	    
	$s set_protocol_deinit_callback {
	    rmtClose
	}
	
	$s set_reset_callback {
	    dl_set stimdg:remaining [dl_ones [dl_length stimdg:trial_type]]
	    set obs_count 0	    
	    rmtSend reset
	}
	
	$s set_start_callback {
	    set first_time 1
	}
	
	$s set_quit_callback {
	    rmtSend clearscreen
	    ess::end_obs QUIT
	}
	
	$s set_end_callback {
	    ess::evt_put SYSTEM_STATE STOPPED [now]
	    if { $ess::open_datafile != "" } {
		ess::file_close
	    }
	}
	
	$s set_file_open_callback {
	    print "opened datafile $filename"
	}
	
	$s set_file_close_callback {
	    set name [file tail [file root $filename]]
	    #	    set path [string map {-rpi4- {}} [info hostname]]
	    set path {}
	    set output_name [file join /tmp $path $name.csv]
	    #	    set converted [save_data_as_csv $filename $output_name]
	    #	    print "saved data to $output_name"
	    print "closed $name"
	}
	

	######################################################################
	#                         Utility Methods                            #
	######################################################################
	
	$s add_method start_obs_reset {} {
	}
	
	$s add_method n_obs {} { return [dl_length stimdg:trial_type] }
	
	$s add_method nexttrial {} {
	    if { [dl_sum stimdg:remaining] } {
		dl_local left_to_show \
		    [dl_select stimdg:stimtype [dl_gt stimdg:remaining 0]]
		set cur_id [dl_pickone $left_to_show]
		set stimtype [dl_get stimdg:stimtype $cur_id]

		foreach i "0 1 2 3" {
		    # set these touching_response knows where choices are
		    set choice_x [dl_get stimdg:choice_centers:$cur_id:$i 0]
		    set choice_y [dl_get stimdg:choice_centers:$cur_id:$i 1]
		    set choice_r [dl_get stimdg:choice_scale $cur_id]
		    ess::touch_region_off $i
		    ess::touch_win_set $i $choice_x $choice_y $choice_r 0
		}
		set target_slot [dl_get stimdg:sample_slot $cur_id]
		rmtSend "nexttrial $cur_id"
	    }
	}
	$s add_method finished {} {
	    return [expr [dl_sum stimdg:remaining]==0]
	}

	$s add_method endobs {} {
	    dl_put stimdg:remaining $cur_id 0
	    incr obs_count
	}

	
	$s add_method presample {} {
	    soundPlay 1 70 200
	}

	$s add_method sample_on {} {
	    rmtSend "!sample_on"
	}

	$s add_method sample_off {} {
	    rmtSend "!sample_off"
	}

	$s add_method choices_on {} {
	    rmtSend "!choices_on"
	    foreach i "0 1 2 3" { ess::touch_region_on $i }
	}

	$s add_method choices_off {} {
	    rmtSend "!choices_off"
	}

	$s add_method reward {} {
	    soundPlay 3 70 70
	    juicerJuice 0 $juice_time
	    ess::evt_put REWARD DURATION [now] $juice_time
	}

	$s add_method noreward {} {

	}

	$s add_method finale {} {
	    soundPlay 6 60 400
	}
	
	$s add_method responded {} {
	    if { [ess::touch_in_win 0] || [ess::touch_in_win 1] ||
		 [ess::touch_in_win 2] || [ess::touch_in_win 3] } {
		if { [ess::touch_in_win $target_slot] } {
		    return 1
		} else {
		    return -1
		}
	    } else {
		return 0
	    }
	}




	#
	# extract qrs representation for a given shape
	#
	$s add_method get_shape_qrs { db shapeID { displayID 51 } } {
    
	    # extract qrs hex location for each pin
	    set sqlcmd { SELECT q,r,s FROM pistonAddressesTable \
			     WHERE DisplayID=$displayID }
	    set qrsvals [$db eval $sqlcmd]
	    dl_local qrs [dl_reshape [dl_ilist {*}$qrsvals] - 3]
	    
	    # get pin bit representation for this shape
	    set sqlcmd {SELECT hex(shapeOutline) FROM shapeTable \
			    WHERE shapeID=$shapeID and DisplayID=$displayID}
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
	
	$s add_method setup_trials { dbfile trial_type } {

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
	    dl_set stimdg:sample_id [dl_ilist]
	    dl_set stimdg:sample_slot [dl_ilist]
	    dl_set stimdg:choice_ids [dl_llist]
	    dl_set stimdg:trial_type [dl_slist]
	    dl_set stimdg:sample_qrs [dl_llist]
	    dl_set stimdg:choice_qrs [dl_llist]
	    dl_set stimdg:choice_centers [dl_llist]
	    dl_set stimdg:choice_scale [dl_flist]
	    
	    set sqlcmd { SELECT shapeFamily from shapeTable WHERE familyAlgo="parent" and displayID=51}
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
		dl_append stimdg:sample_qrs [my get_shape_qrs grasp_db $sample_id]
		
		# add choice qrs
		dl_local choice_qrs [dl_llist]
		foreach choice_id [dl_tcllist $shape_ids] {
		    dl_append $choice_qrs [my get_shape_qrs grasp_db $choice_id]
		}
		dl_append stimdg:choice_qrs $choice_qrs
	    }
	    
	    set n_obs [dl_length stimdg:sample_qrs]

	    dl_set stimdg:stimtype [dl_fromto 0 $n_obs]
	    dl_set stimdg:trial_type [dl_replicate [dl_slist $trial_type] $n_obs]
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
	
	return
    }
}


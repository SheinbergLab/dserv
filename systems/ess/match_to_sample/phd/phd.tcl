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
	
	$s add_param rmt_host          $::ess::rmt_host   stim ipaddr
	$s add_param juice_ml            0.6      variable float
	
	$s add_variable cur_id            -1
	$s add_variable target_slot       -1
	$s add_variable trial_type         {}
	$s add_variable sample_id          -1
	$s add_variable correct           -1
	
	$s set_protocol_init_callback {
	    ::ess::init

	    # open connection to rmt and upload ${protocol}_stim.tcl
	    my configure_stim $rmt_host

	    # initialize touch processor
	    ::ess::touch_init
	    
	    # configure juice channel pin
	    ::ess::juicer_init

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
	    foreach i "0 1 2 3" { ::ess::touch_region_off $i }
	    rmtSend clearscreen
	    if { $trial_type == "HV"} { my haptic_clear }
	    ::ess::end_obs QUIT
	}
	
	$s set_end_callback {
	    ::ess::evt_put SYSTEM_STATE STOPPED [now]
	}
	
	$s set_file_open_callback {
	    print "opened datafile $filename"
	}
	
	$s set_file_close_callback {
	    my process_data $filename [file root $filename].json
	    print "closed and converted $name"
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
		    ::ess::touch_region_off $i
		    ::ess::touch_win_set $i $choice_x $choice_y $choice_r 0
		}
		set target_slot [dl_get stimdg:sample_slot $cur_id]
		set trial_type [dl_get stimdg:trial_type $cur_id]
		set sample_id [dl_get stimdg:sample_id $cur_id]
		rmtSend "nexttrial $cur_id"

		set correct -1
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

	$s add_method haptic_show { id } {
	    package require rest
	    set ip 192.168.88.254
	    set port 5000

	    # this waits until the pistons are all in place
	    set url http://${ip}:${port}/shape/$id
	    set res [rest::simple $url {}]
	}
	
	$s add_method haptic_clear { } {
	    package require rest
	    set ip 192.168.88.254
	    set port 5000

	    # this returns immediately
	    set url http://${ip}:${port}/suction_sequence/51
	    set res [rest::simple $url {}]
	}
	
	$s add_method sample_on {} {
	    if { $trial_type == "VV" } {
		rmtSend "!sample_on"
	    } elseif { $trial_type == "HV" } {
		my haptic_show $sample_id
	    }
	}

	$s add_method sample_off {} {
	    if { $trial_type == "VV" } {
		rmtSend "!sample_off"
	    } elseif { $trial_type == "HV" } {
		my haptic_clear
	    }
	}

	$s add_method choices_on {} {
	    rmtSend "!choices_on"
	    foreach i "0 1 2 3" { ::ess::touch_region_on $i }
	}

	$s add_method choices_off {} {
	    rmtSend "!choices_off"
	}

	$s add_method reward {} {
	    soundPlay 3 70 70
	    ::ess::reward $juice_ml
	    ::ess::evt_put REWARD MICROLITERS [now] [expr {int($juice_ml*1000)}]
	}

	$s add_method noreward {} {
	    soundPlay 4 90 300
	}

	$s add_method finale {} {
	    soundPlay 6 60 400
	}

	$s add_method response_correct {} {
	    return $correct
	}
	
	$s add_method responded {} {
	    set r -1
	    foreach w "0 1 2 3" {
		if { [::ess::touch_in_win $w] } {
		    set r $w
		    break
		}
	    }
	    if { $r == $target_slot } { set correct 1 } { set correct 0 }
	    return $r
	}

	######################################################################
	#                         Data Processing                            #
	#                                                                    #
	# 1. Open the original ess file                                      #
	# 2. Read using dslogReadESS                                         #
	# 3. Pull out response, response time, and status from valid trials  #
	# 4. Pull out stimtype for all valid trials                          #
	# 5. Use stimtype to pull all trial attributes from stimdg           #
	# 6. Add to new dg                                                   #
	# 7. Convert to JSON and export as new file or return JSON string    #
	#                                                                    #
	######################################################################
	
	$s add_method process_data { essfile { jsonfile {} } } {
	    package require dlsh
	    package require dslog
	    set g [::dslog::readESS $essfile]

	    # get relevant event ids
	    lassign [::ess::evt_id ENDTRIAL ABORT]    endt_id     endt_abort 
	    lassign [::ess::evt_id ENDOBS   COMPLETE] endobs_id   endobs_complete 
	    lassign [::ess::evt_id CHOICES  ON]       choices_id  choices_on
	    lassign [::ess::evt_id RESP]              resp_id 
	    lassign [::ess::evt_id STIMTYPE]          stimtype_id 
	    
	    # valid trials have an endtrial subtype which is 0 or 1
	    dl_local endtrial [dl_select $g:e_subtypes \
				   [dl_eq $g:e_types $endt_id]]
	    dl_local endobs   [dl_select $g:e_subtypes \
				   [dl_eq $g:e_types $endobs_id]]
	    dl_local valid    [dl_sums \
				   [dl_and \
					[dl_eq $endobs $endobs_complete] \
					[dl_lengths $endtrial] \
					[dl_lt $endtrial $endt_abort]]]
	    
	    # extract event types/subtypes/times/params for valid trials
	    foreach v "types subtypes times params" {
		dl_local $v [dl_select $g:e_$v $valid]
	    }
	    
	    # pull out variables of interest
	    dl_local correct  \
		[dl_unpack [dl_select $subtypes [dl_eq $types $endt_id]]]
	    dl_local stimon_t  \
		[dl_unpack [dl_select $times \
				[dl_and \
				     [dl_eq $types $choices_id] \
				     [dl_eq $subtypes $choices_on]]]]
	    dl_local response_t \
		[dl_unpack [dl_select $times [dl_eq $types $resp_id]]]
	    dl_local response \
		[dl_unpack [dl_select $subtypes [dl_eq $types $resp_id]]]
	    dl_local stimtype \
		[dl_unpack [dl_deepUnpack \
				[dl_select $params \
				     [dl_eq $types $stimtype_id]]]]

	    # create table to export
	    set out [dg_create]
	    dl_set $out:status $correct
	    dl_set $out:rt [dl_sub $response_t $stimon_t]
	    dl_set $out:response $response

	    # find all stimdg columns and their names without <stimdg>
	    set stimdg_cols \
		[lsearch -inline -all -glob [dg_tclListnames $g] "<stimdg>*"]
	    set cols [regsub -all <stimdg> $stimdg_cols {}]
	    foreach c $cols {
		dl_set $out:$c [dl_choose $g:<stimdg>${c} $stimtype]
	    }

	    # close original ESS dg
	    dg_delete $g
	    
	    # store as JSON
	    set data [dg_toJSON $out]
	    dg_delete $out
	    if { $jsonfile != "" } {
		set f [open $jsonfile w]
		puts $f $data
		close $f
	    } else {
		return $data
	    }
	}
	    
	return
    }
}


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
	$s add_variable trial_type         {}
	$s add_variable sample_id          -1
	$s add_variable correct           -1
	
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
	    if { $trial_type == "HV"} { my haptic_clear }
	    ess::end_obs QUIT
	}
	
	$s set_end_callback {
	    ess::evt_put SYSTEM_STATE STOPPED [now]
	}
	
	$s set_file_open_callback {
	    print "opened datafile $filename"
	}
	
	$s set_file_close_callback {
	    # set name [file tail [file root $filename]]
	    #	    set path [string map {-rpi4- {}} [info hostname]]
	    #set path {}
	    #set output_name [file join /tmp $path $name.csv]
	    #	    set converted [save_data_as_csv $filename $output_name]
	    #	    print "saved data to $output_name"
	    #print "closed $name"
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
		if { [ess::touch_in_win $w] } {
		    set r $w
		    break
		}
	    }
	    if { $r == $target_slot } { set correct 1 } { set correct 0 }
	    return $r
	}
	
	return
    }
}


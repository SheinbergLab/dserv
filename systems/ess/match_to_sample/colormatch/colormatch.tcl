#
# PROTOCOL
#   match_to_sample colormatch
#
# DESCRIPTION
#   Present a target color and two choices
#

namespace eval match_to_sample::colormatch {
    proc protocol_init { s } {
	$s set_protocol [namespace tail [namespace current]]
	
	$s add_param rmt_host          $::ess::rmt_host   stim ipaddr
	
	$s add_param juice_ml         0.8       variable float
	
	$s add_param use_buttons        1       variable int
	$s add_param left_button       24       variable int
	$s add_param right_button      25       variable int
	
	$s add_variable targ_x             
	$s add_variable targ_y             
	$s add_variable targ_r             
	
	$s add_variable dist_x             
	$s add_variable dist_y             
	$s add_variable dist_r             
	
	$s add_variable buttons_changed    0
	$s add_variable cur_id             0
	$s add_variable correct           -1

	$s set_protocol_init_callback {
	    ::ess::init

	    if { $use_buttons } {
		foreach b "$left_button $right_button" {
		    dservAddExactMatch gpio/input/$b
		    dservTouch gpio/input/$b
		    dpointSetScript gpio/input/$b ess::do_update
		}
	    }
	    
	    # open connection to rmt and upload ${protocol}_stim.tcl
	    my configure_stim $rmt_host

	    # initialize touch processor
	    ::ess::touch_init
	    
	    # configure juicer subsystem
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
	    dl_set stimdg:remaining [dl_ones [dl_length stimdg:stimtype]]
	    set obs_count 0	    
	    rmtSend reset
	}
	
	$s set_start_callback {
	    set first_time 1
	}
	
	$s set_quit_callback {
	    ::ess::touch_region_off 0
	    ::ess::touch_region_off 1
	    rmtSend clearscreen
	    ::ess::end_obs QUIT
	}
	
	$s set_end_callback {
	    ::ess::evt_put SYSTEM_STATE STOPPED [now]
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
	
	$s add_method button_pressed {} {
	    if { $use_buttons } {
		if { [dservGet gpio/input/$left_button] ||
		     [dservGet gpio/input/$right_button] } {
		    return 1
		}
	    }
	    return 0
	}
	
	$s add_method start_obs_reset {} {
	    set buttons_changed 0
	}
	
	$s add_method n_obs {} { return [dl_length stimdg:stimtype] }
	
	$s add_method nexttrial {} {
	    if { [dl_sum stimdg:remaining] } {
		dl_local left_to_show \
		    [dl_select stimdg:stimtype [dl_gt stimdg:remaining 0]]
		set cur_id [dl_pickone $left_to_show]
		set stimtype [dl_get stimdg:stimtype $cur_id]
		
		# set these touching_response knows where choices are
		set targ_x [dl_get stimdg:match_x $stimtype]
		set targ_y [dl_get stimdg:match_y $stimtype]
		set targ_r [dl_get stimdg:match_r $stimtype]
		set dist_x [dl_get stimdg:nonmatch_x $stimtype]
		set dist_y [dl_get stimdg:nonmatch_y $stimtype]
		set dist_r [dl_get stimdg:nonmatch_r $stimtype]

		::ess::touch_region_off 0
		::ess::touch_region_off 1
		::ess::touch_reset
		
		::ess::touch_win_set 0 $targ_x $targ_y $targ_r 0
		::ess::touch_win_set 1 $dist_x $dist_y $dist_r 0

		rmtSend "nexttrial $stimtype"

		set correct -1
	    }
	}

	$s add_method endobs {} {
	    if { $correct != -1 } {
		dl_put stimdg:remaining $cur_id 0
		incr obs_count
	    }
	}

	$s add_method finished {} {
	    return [expr [dl_sum stimdg:remaining]==0]
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
	    ::ess::touch_region_on 0
	    ::ess::touch_region_on 1
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

	}

	$s add_method finale {} {
	    soundPlay 6 60 400
	}

	$s add_method response_correct {} { return $correct }
	
	$s add_method responded {} {
	    if { $use_buttons && $buttons_changed } {
		return -1
	    }

	    if { [::ess::touch_in_win 0] } {
		set correct 1
		return 0
	    } elseif { [::ess::touch_in_win 1] } {
		set correct 0
		return 1
	    } else {
		return -1
	    }
	}
	
	return
    }
}


##
##  NAME
##    hapticvis.tcl
##
##  DECRIPTION
##    System for creating haptic visual shape paradigms
##

package require ess

namespace eval hapticvis {
    proc create {} {
	set sys [::ess::create_system [namespace tail [namespace current]]]
	
	$sys set_version 0.9

	######################################################################
	#                          System Parameters                         #
	######################################################################
	
	$sys add_param start_delay          0      time int
	$sys add_param interblock_time   1000      time int
	$sys add_param pre_stim_time      250      time int

	$sys add_param stim_duration    10000      time int

	$sys add_param have_cue             0      variable bool
	$sys add_param cue_delay            0      time int
	$sys add_param cue_duration         0      time int

	$sys add_param have_choices         1      variable bool
	$sys add_param choice_delay       500      time int
	$sys add_param choice_duration   9500      time int
	
	$sys add_param sample_delay         0      time int
	$sys add_param sample_duration   2000      time int

	$sys add_param post_response_time 1000     time int

	$sys add_param finale_delay       500      time int
	$sys add_param simulate_grasp       0      variable bool

	##
	## Local variables for the hapticvis base system
	##
	$sys add_variable n_obs              0
	$sys add_variable obs_count          0
	$sys add_variable cur_id             0
	$sys add_variable first_time         1
	$sys add_variable stimtype           0

	## track status of cue, sample, choices
	$sys add_variable sample_up          0
	$sys add_variable sample_presented   0
	$sys add_variable cue_up             0
	$sys add_variable choices_up         0
	$sys add_variable stim_update        0
	
	## timer ids
	$sys add_variable sample_timer       1
	$sys add_variable cue_timer          2
	$sys add_variable choice_timer       3

	## info for each trial
	$sys add_variable resp               0
	$sys add_variable correct           -1
	$sys add_variable rt
	$sys add_variable sample_on_time
	$sys add_variable choice_on_time  


	######################################################################
	#                            System States                           #
	######################################################################
	
	$sys set_start start

	#
	# start
	#
	$sys add_state start {} { return start_delay }
	
	#
	# start_delay
	#
	$sys add_action start_delay {
	    ::ess::evt_put SYSTEM_STATE RUNNING [now]
	    timerTick $start_delay
	    
	}
	$sys add_transition start_delay {
	    if { [timerExpired] } { return inter_obs }
	}
	
	#
	# inter_obs
	#
	$sys add_action inter_obs {
	    if { !$first_time } {
		timerTick $interblock_time
	    } else {
		set first_time 0
	    }
	    set rt -1
	    set correct -1
	    my nexttrial
	}
	
	
	$sys add_transition inter_obs {
	    if [my finished] { return pre_finale }
	    if { [timerExpired] } { return start_obs }
	}
	
	#
	# start_obs
	#
	$sys add_action start_obs {
	    ::ess::begin_obs $obs_count $n_obs
	    timerTick $pre_stim_time
	    my start_obs_reset
	}
	$sys add_transition start_obs {
	    if { [timerExpired] } {
		return stim_on
	    }
	}

	#
	# stim_on
	#
	$sys add_action stim_on {
	    set sample_up  0
	    set sample_presented 0
	    set cue_up     0
	    set choices_up 0
	    set sample_on_time -1
	    set choice_on_time -1
	    timerTick $stim_duration
	    timerTick $sample_timer $sample_delay
	    timerTick $choice_timer $choice_delay
	    timerTick $cue_timer $cue_delay
	    my stim_on
	    ::ess::evt_put STIMTYPE STIMID [now] $stimtype	
	    ::ess::evt_put STIMULUS ON [now] 
	}
	
	$sys add_transition stim_on {
	    return stim_wait
	}

	#
	# stim_wait: stimulus is "up"
	#
	$sys add_action stim_wait {
	}
	
	$sys add_transition stim_wait {
	    # haptic is waiting for available signal
	    if { $sample_up == -2 } { 
		if { [my sample_presented] } {
		    set sample_presented 1
		}
	    }
	    if { $sample_presented == 1 } {
		return sample_presented
	    }

	    if { $stim_update } {
		return stim_update
	    }
	    
	    if { [timerExpired $sample_timer] } {
		if { $sample_up == 0 } {
		    return sample_on
		} elseif { $sample_up == 1 } {
		    return sample_off
		}
	    }
	    if { [timerExpired $choice_timer] } {
		if { $choices_up == 0 } {
		    return choices_on
		} elseif { $choices_up == 1 } {
		    return choices_off
		}
	    }
	    if { [timerExpired $cue_timer] } {
		if { $cue_up == 0 } {
		    return cue_on
		} elseif { $cue_up == 1 } {
		    return cue_off
		}
	    }
	    if { [timerExpired] } {
		return no_response
	    }

	    # allow responses after sample has appeared
	    if { $sample_on_time >= 0 } {
		set resp [my responded]
		if { $resp >= 0 } { return response }
		if { $resp == -2 } { return highlight_response }
	    }
	}

	
	#
	# highlight_response - show current selection
	#
	$sys add_action highlight_response {
	    my highlight_response
	}
	
	$sys add_transition highlight_response {
	    return stim_wait
	}
	

	#
	# cue_on: show cue (if cue_time > 0)
	#
	$sys add_action cue_on {
	    my cue_on
	    set cue_up 1
	    timerTick $cue_timer $cue_duration
	    ::ess::evt_put CUE ON [now] 
	}
	
	$sys add_transition cue_on { return stim_wait }

	
	#
	# cue_off: show cue if desired
	#
	$sys add_action cue_off {
	    my cue_off
	    ::ess::evt_put CUE OFF [now]
	    set cue_up -1
	}
	
	$sys add_transition cue_off { return stim_wait }
	
	#
	# stim_update: request stimulus update
	#
	$sys add_action stim_update {
	    my stim_update
	    set stim_update 0;	# reset
	}
	
	$sys add_transition stim_update { return stim_wait }

	#
	# sample_on: request sample on
	#
	$sys add_action sample_on {
	    my sample_on
	    if { $trial_type == "visual" || $simulate_grasp } {
		set sample_presented 1
	    } else {
		# this is important, because it will signal
		# that we are not ready yet but also that
		# we don't want to call sample_on again!
		set sample_up -2
	    }
	}
	
	$sys add_transition sample_on { return stim_wait }
	
	#
	# sample_presented: now that sample is up, tick timer
	#
	$sys add_action sample_presented {
	    set sample_presented -1; # don't do this again
	    set sample_up 1;	     # sample is now up
	    timerTick $sample_timer $sample_duration
	    set sample_on_time [now]
	    ::ess::evt_put SAMPLE ON $sample_on_time
	}
	$sys add_transition sample_presented { return stim_wait }
	
	#
	# sample_off: turn sample off
	#
	$sys add_action sample_off {
	    my sample_off
	    ::ess::evt_put SAMPLE OFF [now]
	    set sample_up -1
	}
	
	$sys add_transition sample_off { return stim_wait }

	
	#
	# choices_on: show choices
	#
	$sys add_action choices_on {
	    my choices_on
	    set choices_up 1
	    timerTick $choice_timer $choice_duration
	    set choice_on_time [now]
	    ::ess::evt_put CHOICES ON $choice_on_time
	}
	
	$sys add_transition choices_on { return stim_wait }

	
	#
	# choices_off: turn off choices
	#
	$sys add_action choices_off {
	    my choices_off
	    ::ess::evt_put CHOICES OFF [now]
	    set choices_up -1
	}
	
	$sys add_transition choices_off { return stim_wait }
	
	#
	# response
	#
	$sys add_action response {
	    set response_time [now]
	    set rt [expr {($response_time-$sample_on_time)/1000}]
	    ::ess::evt_put RESP $resp [now]
	    if { $sample_up == 1 } {
		my sample_off
		::ess::evt_put SAMPLE OFF [now]
		set sample_up -1
	    }
	}
	
	$sys add_transition response {
	    if { [my response_correct] } {
		return correct
	    } else {
		return incorrect
	    }
	}
	
	#
	# no_response
	#
	$sys add_action no_response {
	    ::ess::evt_put STIMULUS OFF [now] 
	    ::ess::evt_put ABORT NORESPONSE [now]
	    ::ess::evt_put ENDTRIAL ABORT [now]
	}
	
	$sys add_transition no_response {
	    return post_trial
	}
	
	#
	# correct
	#
	$sys add_action correct {
	    set correct 1
	    ::ess::evt_put ENDTRIAL CORRECT [now]
	    my reward
	}
	
	$sys add_transition correct { return post_trial }
	
	#
	# incorrect
	#
	$sys add_action incorrect {
	    set correct 0
	    ::ess::evt_put ENDTRIAL INCORRECT [now]
	    my noreward
	}
	
	$sys add_transition incorrect { return post_trial }
	
	#
	# post_trial
	#
	$sys add_action post_trial {
	    timerTick $post_response_time
	}
	
	$sys add_transition post_trial {
	    if { [timerExpired] } { return stim_off }
	}


	#
	# stim_off
	#
	$sys add_action stim_off {
	    my stim_off
	    ::ess::evt_put STIMULUS OFF [now] 
	    ::ess::save_trial_info $correct $rt $stimtype
	}

	$sys add_transition stim_off {
	    return finish
	}
	
	#
	# finish
	#
	$sys add_action finish {
	    ::ess::end_obs COMPLETE
	    my endobs
	}
	
	$sys add_transition finish { return inter_obs }
	
	#
	# finale
	#
	$sys add_action pre_finale {
	    timerTick $finale_delay
	}
	
	$sys add_transition pre_finale {
	    if { [timerExpired] } {
		return finale
	    }
	}
	
	$sys add_action finale { my finale }
	$sys add_transition finale { return end }
	
	#
	# end
	# 
	$sys set_end {}
	
	######################################################################
	#                         System Callbacks                           #
	######################################################################
	
	$sys set_init_callback {
	    ::ess::init
	}
	
	$sys set_deinit_callback {}
	
	$sys set_reset_callback {
	    set n_obs [my n_obs]
	    set obs_count 0	    
	}
	
	$sys set_start_callback {
	    set first_time 1
	}
	
	$sys set_quit_callback {
	    ::ess::end_obs QUIT
	}
	
	$sys set_end_callback {}
	
	$sys set_file_open_callback {}
	
	$sys set_file_close_callback {}
	
	$sys set_subject_callback {}
	
	
	######################################################################
	#                          System Methods                            #
	######################################################################

	$sys add_method start_obs_reset {} {}
	
	$sys add_method n_obs { } { return 10 }
	$sys add_method nexttrial { } {
	    set cur_id $obs_count
	    set stimtype $obs_count
	}
	
	$sys add_method finished { } {
	    if { $obs_count == $n_obs } { return 1 } { return 0 }
	}
	
	$sys add_method endobs {} { incr obs_count }

        $sys add_method stim_on {} { print stim_on }
	$sys add_method stim_off {} { print stim_off }

	$sys add_method highlight_response {} { print highlight_response }

	$sys add_method cue_on {} { print cue_on }
	$sys add_method cue_off {} { print cue_off }
        $sys add_method sample_presented {} { return 1 }
        $sys add_method sample_on {} { print sample_on }
	$sys add_method sample_off {} { print sample_off }
	$sys add_method choices_on {} { print choices_on }
	$sys add_method choices_off {} { print choices_off }
	$sys add_method stim_update {} { print stim_update }
	
	$sys add_method reward {} { print reward }
	$sys add_method noreward {} { print noreward }
	$sys add_method finale {} { print finale }
	
	$sys add_method responded {} { return 0 }
	$sys add_method response_correct {} { return 1 }
	
	
	return $sys
    }
}

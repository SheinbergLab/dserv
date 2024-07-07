##
##  NAME
##    match_to_sample.tcl
##
##  DECRIPTION
##    System for creating match to sample paradigms
##

package require ess

proc match_to_sample_system { system_name } {
    set sys [ess::create_system $system_name]

    ######################################################################
    #                          System Parameters                         #
    ######################################################################
    
    $sys add_param n_rep             100      variable int
    $sys add_param start_delay      1000      time int
    $sys add_param interblock_time  1000      time int
    $sys add_param sample_pre_time   250      time int
    $sys add_param sample_time       250      time int
    $sys add_param delay_time       1000      time int
    $sys add_param match_time        250      time int
    $sys add_param finale_delay      500      time int

    $sys add_param response_timeout 2000      time int
    
    ##
    ## Local variables for the match_to_sample base system
    ##
    $sys add_variable n_obs              0
    $sys add_variable obs_count          0
    $sys add_variable cur_id             0
    $sys add_variable first_time         1
    $sys add_variable stimtype           0
    
    
    ######################################################################
    #                            System States                           #
    ######################################################################

    $sys set_start start
	
    #
    # start
    #
    $sys add_action start {
	ess::evt_put SYSTEM_STATE RUNNING [now]	
	timerTick $start_delay
	
    }
    $sys add_transition start {
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
	ess::begin_obs $n_obs $obs_count
    }
    $sys add_transition start_obs {
	return pre_sample
    }

    #
    # pre_sample
    #
    $sys add_action pre_sample {
	timerTick $sample_pre_time
    }
    
    $sys add_transition pre_sample {
	if { [timerExpired] } { return sample_on }
    }
    
    #
    # sample_on
    #
    $sys add_action sample_on {
	ess::evt_put SAMPLE ON [now] 
	ess::evt_put STIMTYPE STIMID [now] $stimtype	
	my sample_on
	timerTick $sample_time
    }

    $sys add_transition sample_on {
	if { [timerExpired] } { return sample_off }
    }
    
    #
    # sample_off
    #
    $sys add_action sample_off {
	ess::evt_put SAMPLE OFF [now] 
	my sample_off
	timerTick $delay_time
    }

    $sys add_transition sample_on {
	if { [timerExpired] } { return choices_on }
    }
    
    #
    # choices_on
    #
    $sys add_action choices_on {
	my choices_on
	ess::evt_put CHOICES ON [now] 
    }

    $sys add_transition choices_on {
	return wait_for_response
    }
    
    #
    # wait_for_response
    #
    $sys add_action wait_for_response {
	timerTick $response_timeout
    }
    $sys add_transition wait_for_response {
	if [timerExpired] { return no_response }
	if [my responded] { return response }
    }
    
    #
    # response
    #
    $sys add_action response {
	ess::evt_put RESP 1 [now]
	my choices_off
	ess::evt_put CHOICES OFF [now] 
    }

    $sys add_transition response {
	if [my response_correct] {
	    return correct
	} else {
	    return incorrect
	}
    }

    #
    # no_response
    #
    $sys add_action no_response {
	ess::evt_put RESP NONE [now]
    }

    $sys add_transition no_response {
	return post_trial
    }
    
    #
    # correct
    #
    $sys add_action correct {
	my reward
    }

    $sys add_transition correct { return post_trial }
    
    #
    # incorrect
    #
    $sys add_action incorrect {
	my noreward
    }
    
    $sys add_transition noreward { return post_trial }

    #
    # post_trial
    #
    $sys add_action post_trial {}
    
    $sys add_transition post_trial {
	return finish
    }
    
    #
    # finish
    #
    $sys add_action finish {
	ess::end_obs COMPLETE
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
	ess::init
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
	ess::end_obs QUIT
    }
    
    $sys set_end_callback {}

    $sys set_file_open_callback {}
    
    $sys set_file_close_callback {}

    $sys set_subject_callback {}


    ######################################################################
    #                          System Methods                            #
    ######################################################################

    $sys add_method n_obs { } { return 10 }
    $sys add_method nexttrial { } {
	set cur_id $obs_count
	set stimtype $obs_count
    }

    $sys add_method finished { } {
	if { $obs_count == $n_obs } { return 1 } { return 0 }
    }

    $sys add_method endobs {} { incr obs_count }
    
    $sys add_method sample_on {} { print sample_on }
    $sys add_method sample_off {} { print sample_off }
    $sys add_method choices_on {} { print choices_on }
    $sys add_method choices_off {} { print choices_off }
    $sys add_method reward {} { print reward }
    $sys add_method noreward {} { print noreward }
    $sys add_method finale {} { print finale }

    $sys add_method responded {} { return 0 }
    $sys add_method response_correct {} { return 1 }
    

    return $sys
}



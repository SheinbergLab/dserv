##
##  NAME
##    search.tcl
##
##  DECRIPTION
##    System for touch screen training
##

package require ess
package require points


proc search_system { system_name } {
    set sys [ess::create_system $system_name]
    
    ######################################################################
    #                          System Parameters                         #
    ######################################################################
    
    $sys add_param n_rep             100      variable int
    $sys add_param interblock_time  1000      time int
    $sys add_param prestim_time      250      time int

    $sys add_param response_timeout 2000      time int
    
    ##
    ## Local variables for this system
    ##
    $sys add_variable n_obs              100
    $sys add_variable obs_count          0
    $sys add_variable cur_id             0
    
    $sys add_variable start_delay        2000
    $sys add_variable stimtype           0
    
    $sys add_variable first_time         1
    
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
	
	set n_obs [my n_obs]
	
	if { !$first_time } {
	    timerTick $interblock_time
	} else {
	    set first_time 0
	}
	
	my nexttrial
    }
    
    $sys add_transition inter_obs {
	if [my finished] { return finale }
	if { [timerExpired] } { return start_obs }
    }
    
    #
    # start_obs
    #
    $sys add_action start_obs {
	ess::begin_obs $n_obs $obs_count
    }	
    $sys add_transition start_obs {
	return pre_stim
    }
    
    #
    # pre_stim
    #
    $sys add_action pre_stim {
	timerTick $prestim_time
    }
    $sys add_transition pre_stim {
	if { [timerExpired] } { return stim_on }
    }
    
    #
    # stim_on
    #
    $sys add_action stim_on {
	my stim_on
	ess::evt_put PATTERN ON [now] 
	ess::evt_put STIMTYPE STIMID [now] $stimtype
    }
    
    $sys add_transition stim_on {
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
    }

    $sys add_transition response {
	if [my hit_target] {
	    return correct
	} else {
	    return incorrect
	}
    }

    #
    # no_response
    #
    $sys add_action no_response {
	my stim_off
	ess::evt_put PATTERN ON [now] 
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
    
    $sys add_method stim_on {} { print search_on }
    $sys add_method stim_off {} { print search_off }
    $sys add_method reward {} { print reward }
    $sys add_method noreward {} { print noreward }
    $sys add_method finale {} { print finale }

    $sys add_method responded {} { return 0 }
    $sys add_method response_correct {} { return 1 }


    return $sys
}


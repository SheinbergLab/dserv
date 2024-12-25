##
##  NAME
##    emcalib.tcl
##
##  DECRIPTION
##    System for calibrating eye position
##

package require ess
package require points

namespace eval emcalib {
    proc create {} {
	set sys [::ess::create_system [namespace tail [namespace current]]]

	$sys set_version 1.0
	
	######################################################################
	#                          System Parameters                         #
	######################################################################
	
	$sys add_param interblock_time  1000      time int
	$sys add_param acquire_time     3000      time int
	$sys add_param reacquire_time   3000      time int
	$sys add_param fixhold_time      400      time int
	$sys add_param pre_sample_time   500      time int
	
	$sys add_param sample_count      500      variable int

	##
	## Local variables for this system
	##
	$sys add_variable n_obs              100
	$sys add_variable obs_count          0
	$sys add_variable cur_id             0
	
	$sys add_variable start_delay        0
	$sys add_variable stimtype           0

	$sys add_variable complete           0
	
	$sys add_variable first_time         1

	$sys add_variable jump_time
	$sys add_variable rt
	
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
	    
	    set n_obs [my n_obs]
	    
	    if { !$first_time } {
		set delay $interblock_time
	    } else {
		set first_time 0
		set delay 0
	    }
	    set rt -1
	    set complete 0	    
	    timerTick $delay
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
	    ::ess::begin_obs $obs_count $n_obs
	    ::ess::evt_put STIMTYPE STIMID [now] $stimtype
	}	
	$sys add_transition start_obs {
	    return fixon
	}
	
	#
	# fixon
	#
	$sys add_action fixon {
	    my fixon
	    ::ess::evt_put FIXSPOT ON [now] 
	    timerTick $acquire_time
	}
	$sys add_transition fixon {
	    if { [timerExpired] } { return abort }
	    if { [my acquired_fixspot] } { return fixhold }
	}
	
	#
	# fixhold
	#
	$sys add_action fixhold {
	    ::ess::evt_put FIXATE IN [now]
	    timerTick $fixhold_time
	}
	
	$sys add_transition fixhold {
	    if { [timerExpired] } { return fixjump }

	}
	
	#
	# fixjump
	#
	$sys add_action fixjump {
	    set jump_time [now]
	    my fixjump
	    timerTick $reacquire_time
	}
	
	$sys add_transition fixjump {
	    if [timerExpired] { return abort }
	    if { [my acquired_fixjump] } { return pre_sample }
	}
	
	#
	# pre_sample
	#
	$sys add_action pre_sample {
	    set jump_acquired [now]
	    set rt [expr ($jump_acquired-$jump_time)/1000]
	    ::ess::evt_put FIXATE REFIXATE $jump_acquired
	    timerTick $pre_sample_time
	}

	$sys add_transition pre_sample {
	    if [timerExpired] { return sample_position }
	    if [my out_of_sample_win] { return abort }
	}
	
	#
	# sample_position
	#
	$sys add_action sample_position {
	    my sample_position
	    timerTick [expr $sample_count+100]
	}
	
	$sys add_transition sample_position {
	    if { [my sample_position_complete] } {
		return store_calibration
	    }
	    if [timerExpired] { return reward }
	    if [my out_of_sample_win] { return abort }
	}
	

	#
	# store_calibration
	#
	$sys add_action store_calibration {
	    my store_calibration
	}
	
	$sys add_transition store_calibration {
	    return reward
	}
	
	#
	# reward
	#
	$sys add_action reward {
	    set complete 1
	    my reward
	    ::ess::evt_put ENDTRIAL CORRECT [now]
	    ::ess::save_trial_info 1 $rt $stimtype
	}
	
	$sys add_transition reward { return post_trial }
	

	#
	# abort
	#
	$sys add_action abort {
	    ::ess::evt_put ENDTRIAL INCORRECT [now]
	}
	$sys add_transition abort {
	    return post_trial
	}

	#
	# post_trial
	#
	$sys add_action post_trial {
	    my fixation_off
	}
	
	$sys add_transition post_trial {
	    return finish
	}
	
	#
	# finish
	#
	$sys add_action finish {
	    if { $complete } {
         ::ess::end_obs COMPLETE
      } else {
         ::ess::end_obs INCOMPLETE
      }
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
	
	$sys add_method n_obs { } { return 18 }
	
	$sys add_method nexttrial { } {
	    set cur_id $obs_count
	    set stimtype $obs_count
	}
	
	$sys add_method finished { } {
	    if { $obs_count == $n_obs } { return 1 } { return 0 }
	}

	$sys add_method fixon {} {}
	$sys add_method acquired_fixspot {} { return 0 }
	$sys add_method acquired_fixjump {} { return 0 }
	$sys add_method fixjump {} {}

	$sys add_method sample_position {} {}
	$sys add_method sample_position_complete {} { return 0 }
	$sys add_method out_of_sample_win {} { return 0 }
	$sys add_method store_calibration {} {}
	$sys add_method fixation_off {} {}
	$sys add_method reward {} {}
	$sys add_method endobs {} { incr obs_count }
	$sys add_method finale {} {}
	
	return $sys
    }
}

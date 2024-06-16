##
##  NAME
##    choice.tcl
##
##  DECRIPTION
##    System for touch screen choice paradigms
##

package require ess
package require points

set choice [ess::create_system choice]

######################################################################
#                          System Parameters                         #
######################################################################

$choice add_param n_rep             100      variable int
$choice add_param interblock_time  1000      time int
$choice add_param prestim_time      250      time int

$choice add_param targ_radius         1.5    variable stim
$choice add_param targ_range          4      variable stim
$choice add_param targ_color          ".2 .9 .9" variable stim

$choice add_param dist_prop          0.67    variable stim
$choice add_param ndists              4      variable stim

$choice add_param rmt_host          $ess::rmt_host   stim ipaddr

$choice add_param juice_pin         27       variable int
$choice add_param juice_time      1000       time int

$choice add_param use_buttons        1       variable int
$choice add_param left_button       24       variable int
$choice add_param right_button      25       variable int

	
##
## Local variables for the choice system
##
$choice add_variable n_obs              0
$choice add_variable obs_count          0
$choice add_variable cur_id             0

$choice add_variable start_delay        2000
$choice add_variable timerID            0
$choice add_variable stimtype           0

$choice add_variable screen_w           
$choice add_variable screen_h           
$choice add_variable screen_halfx       
$choice add_variable screen_halfy       

$choice add_variable targ_x             
$choice add_variable targ_y             
$choice add_variable targ_r             

$choice add_variable touch_count        0
$choice add_variable touch_last         0
$choice add_variable touch_x            
$choice add_variable touch_y            

$choice add_variable buttons_changed    0
	
######################################################################
#                            System States                           #
######################################################################

$choice set_start start
	
#
# start
#
$choice add_action start {
    variable timerID
    variable start_delay
    
    ess::evt_put SYSTEM_STATE RUNNING [now]	
    timerTick $timerID $start_delay
    
}
$choice add_transition start {
    variable timerID
    if { [timerExpired $timerID] } { return inter_obs }
}
	
#
# inter_obs
#
$choice add_action inter_obs {
    variable first_time
    variable interblock_time
    variable timerID
    variable stimtype
    variable targ_x
    variable targ_y
    variable targ_r
    
    variable n_obs
    set n_obs [dl_length stimdg:stimtype]

    if { !$first_time } {
	timerTick $timerID $interblock_time
    } else {
	set first_time 0
    }
    if { [dl_sum stimdg:remaining] } {
	dl_local left_to_show \
	    [dl_select stimdg:stimtype [dl_gt stimdg:remaining 0]]
	set cur_id [dl_pickone $left_to_show]
	set stimtype [dl_get stimdg:stimtype $cur_id]
	
	# set these touching_spot knows where target is
	set targ_x [dl_get stimdg:match_x $stimtype]
	set targ_y [dl_get stimdg:match_y $stimtype]
	set targ_r [dl_get stimdg:match_r $stimtype]
	
	rmtSend "nexttrial $stimtype"
    }
}

$choice add_transition inter_obs {
    variable cur_id
    variable timerID
    variable use_buttons
    if { $use_buttons && [my button_pressed] } {
	print "button pressed"
    }
    
    if { ![dl_sum stimdg:remaining] } {
	return finale
    }
    if { [timerExpired $timerID] } { return start_obs }
}

#
# start_obs
#
$choice add_action start_obs {
    variable obs_count
    variable n_obs
    variable buttons_changed
    set buttons_changes 0
    ess::begin_obs $n_obs $obs_count
}	
$choice add_transition start_obs {
    return pre_stim
}

#
# pre_stim
#
$choice add_action pre_stim {
    variable prestim_time
    variable timerID
    soundPlay 1 70 200
    timerTick $timerID $prestim_time
}

$choice add_transition pre_stim {
    variable timerID
    if { [timerExpired $timerID] } { return stim_on }
}

#
# stim_on
#
$choice add_action stim_on {
    variable stimtype
    variable timerID
    variable touch_count
    variable touch_last
    
    rmtSend "!stimon"
    ess::evt_put PATTERN ON [now] 
    ess::evt_put STIMTYPE STIMID [now] $stimtype
    
    set touch_count 0
    set touch_last 0
    
}

$choice add_transition stim_on {
    return wait_for_response
}

#
# wait_for_response
#
$choice add_action wait_for_response {}
$choice add_transition wait_for_response {
    variable touch_last
    variable touch_count
    variable touch_x
    variable touch_y
    variable targ_x
    variable targ_y
    variable targ_r
    variable use_buttons
    variable left_button
    variable right_button
    variable buttons_changed
    
    if { $buttons_changed && $use_buttons } {
	set buttons_changed 0
	if [dpointGet rpio/levels/${left_button}] {
	    if { $targ_x < 0 } {
		return touched_target
	    } else {
		return missed_target
	    }
	} elseif [dpointGet rpio/levels/${right_button}] {
	    if { $targ_x > 0 } {
		return touched_target
	    } else {
		return missed_target
	    }
	} else {
	    return
	}
    }
    
    if { $touch_count > $touch_last } {
	set touch_last $touch_count
	if [my touching_spot $touch_x $touch_y $targ_x $targ_y $targ_r] {
	    return touched_target
	} else {
	    return missed_target
	}
    }
}


#
# touched_target
#
$choice add_action touched_target {
    variable touch_x
    variable touch_y
    variable use_buttons
    variable left_button
    variable right_button
    
    if { $use_buttons } {
	ess::evt_put RESP 1 [now] [dpointGet rpio/levels/$left_button] [dpointGet rpio/levels/$right_button]
    } else {
	ess::evt_put RESP 1 [now] $touch_x $touch_y
    }
    rmtSend "!stimoff"
    ess::evt_put PATTERN OFF [now] 
}

$choice add_transition touched_target { return reward }

#
# missed_target
#
$choice add_action missed_target {
    variable touch_x
    variable touch_y
    variable use_buttons
    variable left_button
    variable right_button
    
    if { $use_buttons } {
	ess::evt_put RESP 0 [now] [dpointGet rpio/levels/$left_button] [dpointGet rpio/levels/$right_button]
    } else {
	ess::evt_put RESP 0 [now] $touch_x $touch_y
    }
}

$choice add_transition missed_target { return wait_for_response }

#
# reward
#
$choice add_action reward {
    variable juice_time
    soundPlay 3 70 70
    juicerJuice 0 $juice_time
    ess::evt_put REWARD DURATION [now] $juice_time
}

$choice add_transition reward { return post_trial }

#
# post_trial
#
$choice add_action post_trial {}

$choice add_transition post_trial {
    variable use_buttons
    if { $use_buttons && [my button_pressed] } {
	return
    }
    return finish
}

#
# abort
#
$choice add_action abort {
    rmtSend stimoff
    ess::end_obs ABORT
    
}
$choice add_transition abort { return inter_obs }
	
#
# finish
#
$choice add_action finish {
    variable obs_count
    variable stimtype
    
    ess::end_obs COMPLETE
    # decrement the counter tracking items left to show
    dl_put stimdg:remaining $stimtype \
	[expr [dl_get stimdg:remaining $stimtype]-1]	    
    
    incr obs_count
}

$choice add_transition finish { return inter_obs }

#
# finale
#
$choice add_action finale {
    variable timerID
    timerTick $timerID 500
}

$choice add_transition finale {
    variable timerID
    if { [timerExpired $timerID] } {
	return finale_sound
    }
}

$choice add_action finale_sound { soundPlay 6 60 400 }
$choice add_transition finale_sound { return end }

#
# end
# 
$choice set_end {}


	
######################################################################
#                         System Callbacks                           #
######################################################################

$choice set_init_callback {
    variable juice_pin
    variable rmt_host
    
    variable screen_w
    variable screen_h
    variable screen_halfx
    variable screen_halfy
    
    variable use_buttons
    variable left_button
    variable right_button
    ess::init

    if { $use_buttons } {
	foreach b "$left_button $right_button" {
	    dservAddExactMatch rpio/levels/$b
	    dservTouch rpio/levels/$b
	    dpointSetScript rpio/levels/$b "[namespace current] update_button $b"
	}
    }

    dservAddExactMatch mtouch/touch
    dpointSetScript mtouch/touch "[namespace current] update_touch"
    
    # configure juice channel pin
    juicerSetPin 0 $juice_pin
    
    rmtOpen $rmt_host
    set screen_halfx [rmtSend "screen_set HalfScreenDegreeX"]
    set screen_halfy [rmtSend "screen_set HalfScreenDegreeY"]
    set scale_x [rmtSend "screen_set ScaleX"]
    set scale_y [rmtSend "screen_set ScaleY"]
    set screen_w [expr [rmtSend "screen_set WinWidth"]/$scale_x]
    set screen_h [expr [rmtSend "screen_set WinHeight"]/$scale_y]
    if { $screen_halfx == "" } {
	set screen_halfx 16.0
	set screen_halfy 9.0
	set screen_w 1024
	set screen_h 600
    }

    soundReset
    soundSetVoice 81 0    0
    soundSetVoice 57 17   1
    soundSetVoice 60 0    2
    soundSetVoice 42 0    3
    soundSetVoice 21 0    4
    soundSetVoice 8  0    5
    soundSetVoice 113 100 6
    foreach i "0 1 2 3 4 5 6" { soundVolume 127 $i }

    return
}

$choice set_deinit_callback {
    rmtClose
}
	
$choice set_reset_callback {
    dl_set stimdg:remaining [dl_ones [dl_length stimdg:stimtype]]
    variable obs_count
    set obs_count 0	    
    rmtSend reset
}

$choice set_start_callback {
    variable first_time
    set first_time 1
}

$choice set_quit_callback {
    rmtSend clearscreen
    ess::end_obs QUIT
}

$choice set_end_callback {
    ess::evt_put SYSTEM_STATE STOPPED [now]
    if { $ess::open_datafile != "" } {
	ess::file_close
    }
}

$choice set_file_open_callback {
    print "opened datafile $filename"
}

$choice set_file_close_callback {
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

$choice add_method touching_spot { xpix ypix targ_x targ_y targ_r } {
    variable screen_w
    variable screen_h
    variable screen_halfx
    variable screen_halfy

    set halfx $screen_halfx
    set halfy $screen_halfy
    set halfw [expr {$screen_w/2}]
    set h $screen_h
    set halfh [expr {$h/2}]
    set x [expr {($xpix-$halfw)*($halfx/$halfw)}]
    set y [expr {(($h-$ypix)-$halfh)*($halfy/$halfh)}]
    
    set x0 [expr {$targ_x-$x}]
    set y0 [expr {$targ_y-$y}]

    if { [expr {($x0*$x0+$y0*$y0)<($targ_r*$targ_r)}] } {
	return 1
    } else {
	return 0
    }
}

$choice add_method button_pressed {} {
    variable use_buttons
    variable left_button
    variable right_button
    if { $use_buttons } {
	if { [dpointGet rpio/levels/$left_button] ||
	     [dpointGet rpio/levels/$right_button] } {
	    return 1
	}
    }
    return 0
}

$choice add_method update_button { b } {
    variable buttons_changed
    set buttons_changed 1
    ess::do_update
}

$choice add_method update_touch { } {
    variable touch_count
    variable touch_x
    variable touch_y
    
    lassign [dservGet mtouch/touch] id c x y
    set touch_x $x
    set touch_y $y
    incr touch_count

    ess::do_update
}





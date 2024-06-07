##
##  NAME
##    search.tcl
##
##  DECRIPTION
##    System for touch screen training
##

package require ess
package require points

set search [ess::create_system search]


######################################################################
#                          System Parameters                         #
######################################################################

$search add_param n_rep             100      variable int
$search add_param interblock_time  1000      time int
$search add_param prestim_time      250      time int

$search add_param targ_radius        1.5     variable stim
$search add_param targ_range          4      variable stim
$search add_param targ_color          ".2 .9 .9" variable stim

$search add_param dist_prop          0.67    variable stim
$search add_param ndists              4      variable stim

$search add_param rmt_host          $ess::rmt_host   stim ipaddr

$search add_param juice_pin         27       variable int
$search add_param juice_time      1000       time int

##
## Local variables for this system
##
$search add_variable n_obs              100
$search add_variable obs_count           0
$search add_variable cur_id             0

$search add_variable start_delay        2000
$search add_variable timerID            0
$search add_variable stimtype           0

$search add_variable screen_w
$search add_variable screen_h
$search add_variable screen_halfx
$search add_variable screen_halfy

$search add_variable targ_x
$search add_variable targ_y
$search add_variable targ_r

$search add_variable touch_count        0
$search add_variable touch_last         0
$search add_variable touch_x
$search add_variable touch_y

######################################################################
#                            System States                           #
######################################################################
	
$search set_start start	

#
# start
#
$search add_action start {
    variable timerID
    variable start_delay
    variable n_obs

    set n_obs [dl_length stimdg:stimtype]

    ess::evt_put SYSTEM_STATE RUNNING [now]	
    timerTick $timerID $start_delay
}

$search add_transition start {
    variable timerID
    if { [timerExpired $timerID] } { return inter_obs }
}
	
#
# inter_obs
#
$search add_action inter_obs {
    variable first_time
    variable interblock_time
    variable timerID
    variable stimtype
    variable targ_x
    variable targ_y
    variable targ_r
    
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
	foreach p "targ_x targ_y targ_r" {
	    set $p [dl_get stimdg:$p $stimtype]
	}
	
	rmtSend "nexttrial $stimtype"
    }
}

$search add_transition inter_obs {
    variable cur_id
    variable obs_count
    variable timerID
    
    if { ![dl_sum stimdg:remaining] } {
	return finale
    }
    if { [timerExpired $timerID] } { return start_obs }
}

#
# start_obs
#
$search add_action start_obs {
    variable obs_count
    variable n_obs
    ess::begin_obs $n_obs $obs_count
}	
$search add_transition start_obs {
    return pre_stim
}

#
# pre_stim
#
$search add_action pre_stim {
    variable prestim_time
    variable timerID
    timerTick $timerID $prestim_time
}
$search add_transition pre_stim {
    variable timerID
    if { [timerExpired $timerID] } { return stim_on }
}

#
# stim_on
#
$search add_action stim_on {
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
$search add_transition stim_on {
    return wait_for_response
}

#
# wait_for_response
#
$search add_action wait_for_response {}

$search add_transition wait_for_response {
    variable touch_last
    variable touch_count
    variable touch_x
    variable touch_y
    variable targ_x
    variable targ_y
    variable targ_r
    
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
$search add_action touched_target {
    variable touch_x
    variable touch_y
    ess::evt_put RESP 1 [now] $touch_x $touch_y
    rmtSend "!stimoff"
    ess::evt_put PATTERN OFF [now] 
}

$search add_transition touched_target {
    return reward
}

#
# missed_target
#
$search add_action missed_target {
    variable touch_x
    variable touch_y
    ess::evt_put RESP 0 [now] $touch_x $touch_y
}

$search add_transition missed_target {
    return wait_for_response
}

#
# reward
#
$search add_action reward {
    variable juice_time
    juicerJuice 0 $juice_time
    ess::evt_put REWARD DURATION [now] $juice_time
}

$search add_transition reward {
    return finish
}


#
# abort
#
$search add_action abort {
    rmtSend stimoff
    ess::end_obs ABORT
}

$search add_transition abort {
    return inter_obs
}

#
# finish
#
$search add_action finish {
    variable obs_count
    variable stimtype
    
    ess::end_obs COMPLETE
    # decrement the counter tracking items left to show
    dl_put stimdg:remaining $stimtype \
	[expr [dl_get stimdg:remaining $stimtype]-1]	    
    
    incr obs_count
}
$search add_transition finish {
    return inter_obs
}

#
# finale
#
$search add_action finale {
    variable timerID
    timerTick $timerID 250
}

$search add_transition finale {
    variable timerID
    if { [timerExpired $timerID] } {
	return end
    }
}

#
# end
# 
$search set_end {}
	
	
######################################################################
#                         System Callbacks                           #
######################################################################
	
$search set_init_callback {
    variable juice_pin
    variable rmt_host
    
    variable screen_w
    variable screen_h
    variable screen_halfx
    variable screen_halfy
    ess::init
    
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
}

$search set_deinit_callback {
    rmtClose
}

$search set_reset_callback {
    dl_set stimdg:remaining [dl_ones [dl_length stimdg:stimtype]]
    variable obs_count
    set obs_count 0	    
    rmtSend reset
}

$search set_start_callback {
    variable first_time
    set first_time 1
}

$search set_quit_callback {
    rmtSend clearscreen
    ess::end_obs QUIT
}

$search set_end_callback {
    ess::evt_put SYSTEM_STATE STOPPED [now]
    if { $ess::open_datafile != "" } {
	ess::file_close
    }
}

$search set_file_open_callback {
    print "opened datafile $filename"
}
	
$search set_file_close_callback {
    set name [file tail [file root $filename]]
    #	    set path [string map {-rpi4- {}} [info hostname]]
    set path {}
    set output_name [file join /tmp $path $name.csv]
    #	    set converted [save_data_as_csv $f $output_name]
    #	    print "saved data to $output_name"
    print "closed $name"
}

######################################################################
#                         Utility Methods                            #
######################################################################

$search add_method touching_spot { xpix ypix targ_x targ_y targ_r } {
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

$search add_method update_touch {} {
    variable touch_count
    variable touch_x
    variable touch_y
    
    lassign [dservGet mtouch/touch] id c x y
    set touch_x $x
    set touch_y $y
    incr touch_count
    ess::do_update
}

	



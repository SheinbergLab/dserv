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
	
	$s add_param targ_radius         1.5    variable stim
	$s add_param targ_range          4      variable stim
	$s add_param targ_color          ".2 .9 .9" variable stim
	
	$s add_param rmt_host          $ess::rmt_host   stim ipaddr
	
	$s add_param juice_pin         27       variable int
	$s add_param juice_time      1000       time int
	
	$s add_param use_buttons        1       variable int
	$s add_param left_button       24       variable int
	$s add_param right_button      25       variable int
	
	$s add_variable targ_x             
	$s add_variable targ_y             
	$s add_variable targ_r             
	
	$s add_variable dist_x             
	$s add_variable dist_y             
	$s add_variable dist_r             
	
	$s add_variable touch_count        0
	$s add_variable touch_last         0
	$s add_variable touch_x            
	$s add_variable touch_y            
	
	$s add_variable buttons_changed    0

	$s set_protocol_init_callback {
	    ess::init

	    if { $use_buttons } {
		foreach b "$left_button $right_button" {
		    dservAddExactMatch gpio/input/$b
		    dservTouch gpio/input/$b
		    dpointSetScript gpio/input/$b "[namespace current] update_button $b"
		}
	    }
	    
	    dservAddExactMatch mtouch/touch
	    dpointSetScript mtouch/touch "[namespace current] update_touch"


	    # open connection to rmt and upload ${protocol}_stim.tcl
	    my configure_stim $rmt_host
	    
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
	    dl_set stimdg:remaining [dl_ones [dl_length stimdg:stimtype]]
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
	
	$s add_method touching_response {
	    xpix ypix targ_x targ_y targ_r \
		dist_x dist_y dist_r } {
		    set halfx $screen_halfx
		    set halfy $screen_halfy
		    set halfw [expr {$screen_w/2}]
		    set h $screen_h
		    set halfh [expr {$h/2}]
		    set x [expr {($xpix-$halfw)*($halfx/$halfw)}]
		    set y [expr {(($h-$ypix)-$halfh)*($halfy/$halfh)}]
		    
		    set x_t [expr {$targ_x-$x}]
		    set y_t [expr {$targ_y-$y}]
		    set x_d [expr {$dist_x-$x}]
		    set y_d [expr {$dist_y-$y}]
		    
		    if { [expr {($x_t*$x_t+$y_t*$y_t)<($targ_r*$targ_r)}] } {
			return 1
		    } elseif {[expr {($x_d*$x_d+$y_d*$y_d)<($dist_r*$dist_r)}] } {
			return -1
		    } else {
			return 0
		    }
		}
	
	$s add_method button_pressed {} {
	    if { $use_buttons } {
		if { [dpointGet gpio/input/$left_button] ||
		     [dpointGet gpio/input/$right_button] } {
		    return 1
		}
	    }
	    return 0
	}
	
	$s add_method update_button { b } {
	    set buttons_changed 1
	    ess::do_update
	}
	
	$s add_method update_touch { } {
	    lassign [dservGet mtouch/touch] id c x y
	    set touch_x $x
	    set touch_y $y
	    incr touch_count
	    ess::do_update
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
		
		rmtSend "nexttrial $stimtype"
	    }
	}
	$s add_method finished {} {
	    return [expr [dl_sum stimdg:remaining]==0]
	}

	$s add_method presample {} {
	    soundPlay 1 70 200
	}

	$s add_method sample_on {} {
	    set touch_count 0
	    set touch_last 0
	    rmtSend "!sample_on"
	}

	$s add_method sample_off {} {
	    rmtSend "!sample_off"
	}

	$s add_method choices_on {} {
	    rmtSend "!choices_on"
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
	    if { $use_buttons && $buttons_changed } {
		return 1
	    }

	    if { $touch_count > $touch_last } {
		set touch_last $touch_count
		set resp [my touching_response $touch_x $touch_y \
			      $targ_x $targ_y $targ_r $dist_x $dist_y $dist_r]
		return $resp
	    }
	}
	
	$s add_method setup_trials { color_choices } {

	    # build our stimdg
	    if { [dg_exists stimdg] } { dg_delete stimdg }
	    set g [dg_create stimdg]
	    dg_rename $g stimdg
	    
	    set xoff 3.0
	    set yoff 2.0
	    
	    set n_obs [expr $n_rep*2]
	    
	    set scale $targ_radius
	    set maxx [expr $screen_halfx]
	    set maxy [expr $screen_halfy]
	    
	    dl_set $g:stimtype [dl_fromto 0 $n_obs]
	    dl_set $g:side [dl_repeat "0 1" $n_rep]

	    if { $color_choices == "redgreen" } {
		dl_local red [dl_flist 1 0 0]
		dl_local green [dl_flist 0 1 0]
		dl_local sample_colors [dl_repeat [dl_llist $red $green] $n_rep]
		dl_local nonmatch_colors [dl_repeat [dl_llist $green $red] $n_rep]
	    } elseif { $color_choices == "random" } {
		dl_local sample_colors [dl_urand [dl_repeat 3 $n_obs]]
		dl_local nonmatch_colors [dl_urand [dl_repeat 3 $n_obs]]
	    } elseif { $color_choices == "easy" } {
		dl_local sample_hues [dl_irand $n_obs 360]
		dl_local nonmatch_hues [dl_mod [dl_add 180 $sample_hues] 360]
		dl_local l [dl_repeat 85. $n_obs]
		dl_local c [dl_repeat 95. $n_obs]
		dl_local sample_colors \
		    [dl_div [dl_transpose \
				 [dlg_polarlabcolors $l $c [dl_float $sample_hues]]] \
			 255.]
		dl_local nonmatch_colors \
		    [dl_div [dl_transpose \
				 [dlg_polarlabcolors $l $c [dl_float $nonmatch_hues]]] \
			 255.]
	    }
	    
	    dl_set $g:sample_x [dl_repeat 0. $n_obs]
	    dl_set $g:sample_y [dl_repeat $yoff $n_obs]
	    dl_set $g:sample_r [dl_repeat $targ_radius $n_obs]
	    dl_set $g:sample_color $sample_colors
	    dl_set $g:sample_pos \
		[dl_reshape [dl_interleave $g:sample_x $g:sample_y] - 2]
	    
	    
	    dl_set $g:match_x [dl_mult 2 [dl_sub $g:side .5] $xoff]
	    dl_set $g:match_y [dl_repeat [expr -1*$yoff] $n_obs]
	    dl_set $g:match_r [dl_repeat $targ_radius $n_obs]
	    dl_set $g:match_color $sample_colors
	    dl_set $g:match_pos [dl_reshape [dl_interleave $g:match_x $g:match_y] - 2]
	    
	    dl_set $g:nonmatch_x [dl_mult 2 [dl_sub [dl_sub 1 $g:side] .5] $xoff]
	    dl_set $g:nonmatch_y [dl_repeat [expr -1*$yoff] $n_obs]
	    dl_set $g:nonmatch_r [dl_repeat $targ_radius $n_obs]
	    dl_set $g:nonmatch_color $nonmatch_colors
	    dl_set $g:nonmatch_pos \
		[dl_reshape [dl_interleave $g:nonmatch_x $g:nonmatch_y] - 2]
	    
	    dl_set $g:remaining [dl_ones $n_obs]
	    
	    return $g
	}

	return
    }
}


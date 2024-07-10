#
# PROTOCOL
#   search circles
#
# DESCRIPTION
#   Present a large circle with possible distractors
#

namespace eval search::circles {
    proc protocol_init { s } {
	$s set_protocol [namespace tail [namespace current]]
	
	$s add_param targ_radius         1.5    variable stim
	$s add_param targ_range          4      variable stim
	$s add_param targ_color          ".2 .9 .9" variable stim
	
	$s add_param dist_prop          0.67    variable stim
	$s add_param ndists              4      variable stim
	
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

	$s set_protocol_init_callback {
	    ess::init

	    dservAddExactMatch mtouch/touch
	    dpointSetScript mtouch/touch "[namespace current] update_touch"
	    
	    # configure juice channel pin
	    juicerSetPin 0 $juice_pin
	    
	    # open connection to rmt and upload ${protocol}_stim.tcl
	    my configure_stim $rmt_host
	    
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
	

	$s add_method touching_spot { xpix ypix targ_x targ_y targ_r } {
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
		
		# set these touching_spot knows where target is
		foreach p "targ_x targ_y targ_r" {
		    set $p [dl_get stimdg:$p $stimtype]
		}
		
		rmtSend "nexttrial $stimtype"
	    }
	}
	$s add_method finished {} {
	    return [expr [dl_sum stimdg:remaining]==0]
	}

	$s add_method prestim {} {
	    soundPlay 1 70 200
	}
	
	$s add_method stim_on {} {
	    set touch_count 0
	    set touch_last 0
	    rmtSend "!stimon"
	}

	$s add_method stim_off {} {
	    rmtSend "!stimoff"
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

	    if { $touch_count > $touch_last } {
		set touch_last $touch_count	
		if [my touching_spot $touch_x $touch_y $targ_x $targ_y $targ_r] {
		    return 1
		} else {
		    return -1
		}
	    }
	    return 0
	}

	$s add_method basic_search { nr nd mindist } {
	    set n_rep $nr
	    set ndists $nd
	    
	    if { [dg_exists stimdg] } { dg_delete stimdg }
	    set g [dg_create stimdg]
	    dg_rename $g stimdg
	    
	    set n_obs [expr [llength $ndists]*$n_rep]
	    
	    set scale $targ_radius
	    set maxx [expr $screen_halfx]
	    set maxy [expr $screen_halfy]
	    
	    dl_set $g:stimtype [dl_fromto 0 $n_obs]
	    dl_set $g:targ_x [dl_mult 2 [dl_sub [dl_urand $n_obs] 0.5] $targ_range]
	    dl_set $g:targ_y [dl_mult 2 [dl_sub [dl_urand $n_obs] 0.5] $targ_range]
	    dl_set $g:targ_r [dl_repeat $targ_radius $n_obs]
	    dl_set $g:targ_color [dl_repeat [dl_slist $targ_color] $n_obs]
	    dl_set $g:targ_pos [dl_reshape [dl_interleave $g:targ_x $g:targ_y] - 2]
	    
	    # add distractors
	    # maxy is typically less than maxx
	    #	dl_local max_x [dl_repeat $maxx $n_obs]
	    dl_local max_y [dl_repeat $maxy $n_obs]
	    
	    dl_local min_dist [dl_repeat $mindist $n_obs]
	    dl_set $g:dists_n [dl_replicate [dl_ilist {*}$ndists] $n_rep]
	    
	    dl_set $g:dists_pos \
		[::points::pickpoints $g:dists_n [dl_pack stimdg:targ_pos] \
		     $min_dist $max_y $max_y]
	    
	    # pull out the xs and ys from the packed dists_pos list
	    dl_set $g:dist_xs [dl_unpack [dl_choose $g:dists_pos [dl_llist [dl_llist 0]]]]
	    dl_set $g:dist_ys [dl_unpack [dl_choose $g:dists_pos [dl_llist [dl_llist 1]]]]
	    
	    set dist_r [expr $scale*$dist_prop]
	    set dist_color $targ_color
	    
	    dl_set $g:dist_rs [dl_repeat $dist_r [dl_llength $g:dist_xs]]
	    dl_set $g:dist_colors [dl_repeat [dl_slist $dist_color] [dl_llength $g:dist_xs]]
	    
	    dl_set $g:remaining [dl_ones $n_obs]
	    
	    return $g
	}
	return
    }
}


# Some basic exploration of the simulation data
#  To load data:
#   load_data human_sim_022217004
#

#package require mupdf-notk
#package require newton
package require impro
package require dlsh

proc show_trials { g } {

    #dl_local valid_sacs [dl_between $g:sactimes $g:stimon $g:response]

   # for { set i 0 } { $i < [dl_length $valid_sacs] } { incr i } {
#	set replacement_sac [expr [dl_find $valid_sacs:$i 1] -1]
#	dl_set $valid_sacs:$i [dl_replaceByIndex $valid_sacs:$i $replacement_sac 1]
#    }

    #dl_local xysacs [smoothsacs_new $g 0]
    #dl_local xysacs [smoothems $g]
    #dl_local goodems [smoothems $g]
    
    #foreach c "valid_sacs xysacs goodems" {
#	dl_set $g:st_$c [set $c]
    #}
    set ::activator::execcmd "show_trial $g %n"
    activator::setup
}


proc show_trial { g trial { show_saccades 0 } { show_post 0 } } {
    set w [get_world $g $trial]

    # Should have been computed in show_trials
    #set valid_sacs $g:st_valid_sacs
    #set xysacs $g:st_xysacs
    #set goodems $g:st_goodems
    #dl_local translationx [dl_series -20 20 2]
    #dl_local translationy [dl_series -10 10 2]

    clearwin

    # Setup the viewport to be the middle of the original display
    setwindow -16 -12 16 12
 
    #set stimon [dl_get $g:stimon $trial]
    #set response [dl_get $g:response $trial]

    #dl_local valid_sacs [dl_between $g:sactimes:$trial $stimon $response]
    #dl_local sactos [dl_select $g:sactos:$trial $valid_sacs]
    #dl_local sac_x1 [dl_unpack [dl_choose $sactos [dl_llist 0]]]
    #dl_local sac_y1 [dl_unpack [dl_choose $sactos [dl_llist 1]]]
    #dl_local sac_1 [dl_unpack [dl_choose $sactos [dl_llist 0]]]
    #dl_local sac_2 [dl_unpack [dl_choose $sactos [dl_llist 1]]]
    #dl_local sac_x1 [dl_select $sac_1 [dl_between $sac_2 -12 12]]
    #dl_local sac_y1 [dl_select $sac_2 [dl_between $sac_2 -12 12]]

    #dl_local xysacs [smoothsacs_pre_resp $g 0]
    #dl_local goodems [smoothems $g]
    #dl_local RNN_decoding [smoothsacs_decoding $g RNN]
    #dl_local CNN_decoding [smoothsacs_decoding $g CNN]
    #dl_local smoothed_trajectory [smoothsacs_decoding $g irrelevant]

   # do_simulation $w center
   # dlg_markers ball_pos:x ball_pos:y fcircle -color [dlg_rgbcolor 120 120 120] -size 1x
   		       
    set w [dg_copySelected $w [dl_not [dl_oneof $w:name [dl_slist gate_center b_wall l_wall r_wall]]]]
    show_world $w
    dlg_markers [dl_get $g:trajectory:${trial} 0] [dl_get $g:trajectory:${trial} 1] -marker fcircle -size 1x -color [dlg_rgbcolor 120 120 120]

    if { $show_saccades } {
	#dl_local ems [get_ems_pre_response $g]
	#dlg_markers $ems:0:$trial $ems:1:$trial -marker fcircle -color $::colors(cyan)
	
	#set sac_x1 $xysacs:0:$trial
	#set sac_y1 $xysacs:1:$trial

	#set smooth_RNN_x1 $RNN_decoding:0:$trial
	#set smooth_RNN_y1 $RNN_decoding:1:$trial

	#set smooth_CNN_x1 $CNN_decoding:0:$trial
	#set smooth_CNN_y1 $CNN_decoding:1:$trial

	#set sac_x1 $valid_sacs:0:$trial
	#set sac_y1 $valid_sacs:1:$trial

	#dlg_lines $sac_x1 $sac_y1 -lwidth 100
	#dlg_markers $sac_x1 $sac_y1 -marker fcircle -size 0.5x -color $::colors(cyan)
	#dlg_markers $smooth_RNN_x1 $smooth_RNN_y1 -marker fcircle -size 1x -color $::colors(cyan)
	#dlg_markers $smooth_CNN_x1 $smooth_CNN_y1 -marker fcircle -size -1x -color $::colors(yellow)

	#dlg_markers $g:true_positions:${trial}:0  $g:true_positions:${trial}:1 -marker fcircle -size 1x 
	dl_local trajectory [dl_get $g:trajectory $trial]
	
	dlg_markers [dl_get $g:trajectory:${trial} 0] [dl_get $g:trajectory:${trial} 1] -marker fcircle -size 1x -color [dlg_rgbcolor 120 120 120]
	

        dl_local trajectory_points_x [dl_select $trajectory:0 [dl_fromto 0 [dl_length $trajectory:0] [expr [dl_length $trajectory:0] / 16]]]
	dl_local trajectory_points_y [dl_select $trajectory:1 [dl_fromto 0 [dl_length $trajectory:0] [expr [dl_length $trajectory:0] / 16]]]

	set equidistant [equidistant_points $g $trial]
	dlg_markers $equidistant:x  $equidistant:y  -marker fcircle -size 1x -color $::colors(yellow)

	#


	#dlg_text $sac_x1 $sac_y1 [dl_series 1 [dl_length $sac_x1]] \
	#    -color $::colors(black)
	
	if { $show_post } {
	    #dl_local ems [get_ems_post_response $g]
	    #dlg_markers $ems:0:$trial $ems:1:$trial -marker fcircle -color $::colors(yellow) -size 1x
	    dlg_markers $goodems:0:$trial $goodems:1:$trial -marker fcircle -color $::colors(yellow) -size 0.5x
	}

    } else {
	#dl_local ems [get_ems_pre_response $g]
	#dlg_markers $ems:0:$trial $ems:1:$trial -marker fcircle -color $::colors(cyan)
	#set blinks [ems_minus_blinks $g]
	#dlg_markers $blinks:sorted_ems_x:$trial $blinks:sorted_ems_y:$trial -marker fcircle -color $::colors(yellow)
	
	if { $show_post } {
	    dl_local ems [get_ems_post_response $g]
	    dlg_markers $ems:0:$trial $ems:1:$trial -marker fcircle -color $::colors(yellow)
	}
    }
	
	
	
    #set status [dl_get $g:status $trial]
    #set rts [dl_get $g:rts $trial]
    #set side [dl_get $g:side $trial]
    if { [dl_exists $g:show_shadow] } {
	set show_shadow [dl_get $g:show_shadow $trial]
    }
    #set intersection [dl_get $g:intersection $trial]
    set discrete_uncertainty [dl_get $g:discrete_uncertainty $trial]
    set uncertainty [dl_get $g:uncertainty $trial]
    
    #if { $side == 0 } {
#	set answer "Correct Answer: Left"
#    } else {
#	set answer "Correct Answer: Right"
#    }
  
    setwindow 0 0 1 1
#    set text [format "Trial %d" $trial]
#    dlg_text  0.98 0.95 $text -just 1 -size 14
#    set text [format "Status %.2f" $status]
#    dlg_text  0.98 0.9 $text -just 1
#    set text [format "RT: %.2f" $rts]
#    dlg_text  0.98 0.85 $text -just 1
    #set text [format "Intersection: %.2f" $intersection]
#    #dlg_text  0.98 0.8 $text -just 1
#    set text [format "Uncertainty: %.2f" $uncertainty]
#    dlg_text 0.98 0.8 $text -just 1 
#    set text [format "Discrete uncertainty: %.2f" $discrete_uncertainty]
#    dlg_text  0.98 0.75 $text -just 1
    
}

proc derive_individual_means { subjectlist maingroup } {
    set meangroup [dg_create]
    for { set subj 0 } { $subj < [dl_length $subjectlist] } { incr subj } {
	dl_set $meangroup:$subj [dl_flist]
	set currsubj [dg_copySelected $maingroup [dl_eq $maingroup:subj "h[dl_get $subjectlist $subj]"]]
	#set meaneds [dl_mean $currsubj:eds]
	#set meanshuffled [dl_mean $currsubj:shuffled_eds]
	set meanstatus [dl_mean $currsubj:status]
	#set meanintersection [dl_mean $currsubj:intersection]
	#set meanrts [dl_mean $currsubj:rts]
	#set meanvalidsacs [dl_mean $currsubj:validsacs]
	set meancorrect [dl_mean $currsubj:correct]

	#dl_append $meangroup:$subj $meaneds
	#dl_append $meangroup:$subj $meanshuffled
	dl_append $meangroup:$subj $meanstatus
	#dl_append $meangroup:$subj $meanintersection
	#dl_append $meangroup:$subj $meanrts
	#dl_append $meangroup:$subj $meanvalidsacs
	dl_append $meangroup:$subj $meancorrect

    }
    return $meangroup
}


proc compute_overlap { img1 img2 img3 translationx translationy } {
    dl_local i1 [img_img2list $img1]
    dl_local i2 [img_img2list $img2]
    set intersection -1.0

    scan [img_alphaBB $img2] "%d %d %d %d" topx topy botx boty
    set xrange [expr $botx-$topx]
    set yrange [expr $boty-$topy]

    foreach tx [dl_tcllist $translationx] {
	foreach ty [dl_tcllist $translationy] {
	    dl_pushTemps;		# keep track of temporary variables
	    img_bkgfill $img3 0;
	    img_copyarea $img2 $img3 [expr $topx + $tx] [expr $topy + $ty] $topx $topy $xrange $yrange
    	    dl_local i3  [img_img2list $img3]
	    dl_local i4 [dl_mult [dl_int $i1] [dl_int $i3]]
	    dl_local i5 [dl_add [dl_int $i1] [dl_int $i3]]
	    set olap [expr [dl_get [dl_float [dl_sum [dl_eq $i4 5000]]] 0] / [dl_sum [dl_gte [dl_int $i5] 50]]]
	    # Keep if bigger than current overlap
	    if { [expr $olap > $intersection] } {
		set max_x_index $tx
		set max_y_index $ty
		set intersection $olap
	    }
	    dl_popTemps;		# free unused variables
	}
    }
    return "$intersection $max_x_index $max_y_index"
}

proc overlap_per_trial { trial img1 img2 sig1 sig2 translate translationx translationy { w 256 } { h 256} } {
    img_bkgfill $img1 0;	# clear to black
    img_bkgfill $img2 0;	# clear to black

    setwindow -12 -12 12 12
    set circle_pix [expr int($w/21)]

    set sig1_x1 $sig1:0:$trial
    set sig1_y1 $sig1:1:$trial
    dl_local sig1pixx [deg2pix_horiz $sig1_x1 $w 24]
    dl_local sig1pixy [deg2pix_vert $sig1_y1 $h 24]

    set sig2_x1 $sig2:0:$trial
    set sig2_y1 $sig2:1:$trial
    dl_local sig2pixx [deg2pix_horiz $sig2_x1 $w 24]
    dl_local sig2pixy [deg2pix_vert $sig2_y1 $h 24]

    img_circles $img1 $sig1pixx $sig1pixy $circle_pix 50
    img_circles $img2 $sig2pixx $sig2pixy $circle_pix 100

    if { $translate == 1 } {
	set img3 [img_create -width $w -height $h]
	scan [compute_overlap $img1 $img2 $img3 $translationx $translationy] "%f %d %d" overlap xtrans ytrans
	img_delete $img3
    } elseif { $translate == 0 } {
	dl_local i1 [img_img2list $img1]
	dl_local i2 [img_img2list $img2]
	dl_local i3 [dl_mult [dl_int $i1] [dl_int $i2]]
	dl_local i4 [dl_add [dl_int $i1] [dl_int $i2]]
	set overlap [expr [dl_get [dl_float [dl_sum [dl_eq $i3 5000]]] 0] / [dl_sum [dl_gte [dl_int $i4] 50]]]
	set xtrans 0
	set ytrans 0
    }
    dl_local outlist [dl_flist $overlap $xtrans $ytrans]


    dl_return $outlist
}

proc overlap_shuffled_multiple { subjectlist signal1 signal2 repetitions { control 1 } } {
    for { set i 0 } { $i < [dl_length $subjectlist] } { incr i } {
	set currentgroup [make_group [dl_get $subjectlist 0]]
	overlap_shuffled_repeat $currentgroup $signal1 $signal2 $repetitions $control
    }
}

proc overlap_shuffled_repeat { group signal1 signal2 repetitions { within 1 } { control 1 } } {
    dl_local means [dl_flist]
    for { set i 0 } { $i < $repetitions } { incr i } {
	set currentgroup [degreeoverlap_shuffled $group $signal1 $signal2 $within $control]
	dl_append $means [dl_mean $currentgroup:intersection]
    }
    set sorted [dl_reshape [dl_sort $means] 1 $repetitions]
    dl_set $group:shuffled_intersections $sorted
    #dg_delete $currentgroup
    return $group
}

proc degreeoverlap_shuffled { g signal1 signal2 { within 1 } { control 1 } } {
    if { $within == 1 } {
	set groupleft [dg_copySelected $g [dl_eq $g:side 0]]
	set intersection_left [degreeoverlap $groupleft $signal1 $signal2 $control]
	set groupright [dg_copySelected $g [dl_eq $g:side 1]]
	set intersection_right [degreeoverlap $groupright $signal1 $signal2 $control]
	dg_append $intersection_left $intersection_right
	set outgroup $intersection_left

    } else {
	set outgroup [degreeoverlap $g $signal1 $signal2 $control]
    }

    #dg_delete $intersection_left
    #dg_delete $intersection_right
    return $outgroup
}

proc degreeoverlap { g signal1 signal2 { control 0 } { translate 1 } { w 256 } { h 256 } } {
    dl_set $g:intersection [dl_flist]
    dl_set $g:translation_x [dl_flist]
    dl_set $g:translation_y [dl_flist]

    dl_set $g:trial [dl_ilist]
    dl_local translationx [dl_series -3 3 1]
    dl_local translationy [dl_series -3 3 1]

    # Create two images to use for comparing trajectories
    set img1 [img_create -width $w -height $h]
    set img2 [img_create -width $w -height $h]

    if { $signal1 == "ems" || $signal2 == "ems"} {
	dl_local ems [smoothems $g $control]
    }

    if { $signal1 == "sacs" || $signal2 == "sacs" } {
	dl_local sacs [smoothsacs_pre_resp $g $control]
    }

    if { $signal1 == "ballpos" || $signal2 == "ballpos"} {
	dl_local ballpos [dl_transpose $g:trajectory]

    }

    dl_local decoding_result [smoothsacs_decoding $g CNN]

    for { set trial 0 } { $trial < [dl_length $g:rts] } { incr trial } {
	set trialoverlap [overlap_per_trial $trial $img1 $img2 $sacs $decoding_result $translate $translationx $translationy]
	dl_append $g:trial $trial
	dl_append $g:intersection [dl_get $trialoverlap 0]
	dl_append $g:translation_x [dl_get $trialoverlap 1]
	dl_append $g:translation_y [dl_get $trialoverlap 2]
    }


    dl_int $g:translation_x
    dl_int $g:translation_y
    img_delete $img1 $img2
    return $g
}



proc degreeoverlap_decoding { g decoding_values { translate 0 } { w 256 } { h 256 } } {
    dl_set $g:${decoding_values}_intersection [dl_flist]
    dl_set $g:${decoding_values}_smoothed [dl_llist]
    #dl_set $g:translation_x [dl_flist]
    #dl_set $g:translation_y [dl_flist]

    dl_set $g:trial [dl_ilist]
    dl_local translationx [dl_series -1 1 1]
    dl_local translationy [dl_series -1 1 1]

    # Create two images to use for comparing trajectories
    set img1 [img_create -width $w -height $h]
    set img2 [img_create -width $w -height $h]

    dl_local ballpos [dl_transpose $g:trajectory]
    dl_local decoding_result [smoothsacs_decoding $g $decoding_values]
    dl_append $g:${decoding_values}_smoothed $decoding_result
    
    for { set trial 0 } { $trial < [dl_length $g:rts] } { incr trial } {
	set trialoverlap [overlap_per_trial $trial $img1 $img2 $ballpos $decoding_result $translate $translationx $translationy]
	dl_append $g:trial $trial
	dl_append $g:${decoding_values}_intersection [dl_get $trialoverlap 0]
	#dl_append $g:translation_x [dl_get $trialoverlap 1]
	#dl_append $g:translation_y [dl_get $trialoverlap 2]
    }


    #dl_int $g:translation_x
    #dl_int $g:translation_y
    img_delete $img1 $img2
    return $g
}

set mysubjs "20 21 23 24 25 26 27 29 30 31 32 33 34 35 36 38"

proc one_subject { s { translate 1 } { control 0 } } {
    set subjectgroup [make_group [dl_get $subjectlist 0]]
    set returngroup [degreeoverlap $group ballpos ems $translate $control]
}


proc one_by_one { subjectlist signal1 signal2 { control 0 } { translate 1 } } {
	set datadir savedg
	set returngroup [make_group [dl_get $subjectlist 0]]
	set returngroup [degreeoverlap $returngroup $signal1 $signal2 $translate $control]
	set outname [format "s%02d_intersection.dgz" [dl_get $subjectlist 0]]
	dg_write $returngroup $outname
	for { set subj 1 } { $subj < [dl_length $subjectlist] } { incr subj } {
	    set group [make_group [dl_get $subjectlist $subj]]
	    if { $control == 0 } {
		set currentgroup [degreeoverlap $group $signal1 $signal2]
	    } elseif { $control == 1 } {
		set currentgroup [degreeoverlap_shuffled $group $signal1 $signal2 $control]
	    }
	    dg_append $returngroup $currentgroup
	    set outname [format "s%02d_intersection.dgz" [dl_get $subjectlist $subj]]
	    dg_write $currentgroup $outname
	    dg_delete $currentgroup
	}
    return $returngroup
}

proc combine_subjects { repetitions } {
    set returngroup [dg_read h210.dgz]
    dl_local mean [dl_mean $returngroup:intersection]
    for { set i 1 } { $i < $repetitions } { incr i } {
	set group [dg_read h21$i.dgz]
	dl_append $mean [dl_mean $group:intersection]
	dg_delete $group
    }
    dl_return $mean
}

proc all_conditions { subject translate { control 0 } } {
    set sacemsgroup [make_group $subject]
    degreeoverlap $sacemsgroup ems sacs $translate $control
    degreeoverlap $sacsimsgroup ballpos sacs $translate $control
    degreeoverlap
    for { set subj 1 } { $subj < [dl_length $subjectlist] } { incr subj } {
	set group [make_group [dl_get $subjectlist $subj]]
	set currentgroup [degreeoverlap $group $control]
	dg_append $returngroup $currentgroup
    }
    return $returngroup
}

proc save_images { g { w 512 } { h 512 } } {
    # Create two images to use for comparing trajectories
    set img1 [img_create -width $w -height $h]
    set img2 [img_create -width $w -height $h]

    for { set a 0 } { $a < [dl_length $g:rts] } { incr a } {
	img_bkgfill $img1 0;	# clear to black
	img_bkgfill $img2 0;	# clear to black

	dl_pushTemps;		# keep track of temporary variables
	set trial $a
	setwindow -12 -12 12 12
	set stimon [dl_get $g:stimon $trial]
	set response [dl_get $g:response $trial]

	dl_local valid_sacs [dl_between $g:sactimes:$trial $stimon $response]
	if { [dl_sum $valid_sacs] } {
	    dl_local xysacs [smoothsacs $g $valid_sacs $trial]
	    dl_local sac_x1 $xysacs:0
	    dl_local sac_y1 $xysacs:1

	    dl_local goodems [smoothems $g $trial]
	    dl_local goodemsx1 $goodems:0
	    dl_local goodemsy1 $goodems:1

	    if { [dl_length $sac_x1] > 0 } {
		set circle_pix [expr int($w/40)]

		dl_local sacpixx [deg2pix_horiz $sac_x1 $w 24]
		dl_local sacpixy [deg2pix_vert $sac_y1 $h 24]
		img_circles $img1 $sacpixx $sacpixy $circle_pix 50
		img_writePNG $img1 c:/Users/lab/Desktop/TrajectoryImages/saccade$a.png

		dl_local empixx [deg2pix_horiz $goodemsx1 $w 24]
		dl_local empixy [deg2pix_vert $goodemsy1 $h 24]
		img_circles $img2 $empixx $empixy $circle_pix 50
		img_writePNG $img2 c:/Users/lab/Desktop/TrajectoryImages/pursuit$a.png
	    }
	}
	dl_popTemps;
    }
    img_delete $img1 $img2
}
proc sample_plank_per_trial { group trial } {
    dl_local ems [get_ems_pre_response $group]
    dl_local current_trial_h $ems:0:$trial
    dl_local current_trial_v $ems:1:$trial
    #set stimon [dl_get $group:stimon $trial]
    #set response [dl_get $group:response $trial]
    #dl_local valid_sacs [dl_between $group:sactimes:$trial $stimon $response]
    #dl_local sactos [dl_select $group:sactos:$trial $valid_sacs]
    #dl_local sac_1 [dl_unpack [dl_choose $sactos [dl_llist 0]]]
    #dl_local sac_2 [dl_unpack [dl_choose $sactos [dl_llist 1]]]
    #dl_local sac_x [dl_select $sac_1 [dl_gte $sac_2 -6]]
    #dl_local sac_y [dl_select $sac_2 [dl_gte $sac_2 -6]]

    dl_local eachsac [dl_flist]

    for { set i 0 } { $i < [dl_length $current_trial_h] } { incr i } {
	dl_local distlist [dl_flist]

	for { set planks 0 } { $planks < 10 } { incr planks } {
	    set sx [dl_get $group:world#sx:$trial $planks]
	    set sy [dl_get $group:world#sy:$trial $planks]
	    set spin [dl_get $group:world#spin:$trial $planks]
	    set tx [dl_get $group:world#tx:$trial $planks]
	    set ty [dl_get $group:world#ty:$trial $planks]

	    dl_local x [dl_mult $sx [dl_flist -.5 .5 .5 -.5  ]]
	    dl_local y [dl_mult $sy [dl_flist -.5  -.5 .5 .5  ]]

	    set cos_theta [expr cos(-1*$spin*($::pi/180.))]
	    set sin_theta [expr sin(-1*$spin*($::pi/180.))]

	    dl_local rotated_x [dl_sub [dl_mult $x $cos_theta] [dl_mult $y $sin_theta]]
	    dl_local rotated_y [dl_add [dl_mult $y $cos_theta] [dl_mult $x $sin_theta]]

	    dl_local x [dl_add $tx $rotated_x]
	    dl_local y [dl_add $ty $rotated_y]

	    dl_append $x $tx
	    dl_append $y $ty

	    for { set a 0 } { $a < [dl_length $x] } { incr a } {
		set xval [expr [dl_get $x $a] - [dl_get $current_trial_h $i]]
		set xsqr [expr $xval * $xval]
		set yval [expr [dl_get $y $a] - [dl_get $current_trial_v $i]]
		set ysqr [expr $yval * $yval]
		set distance [dl_sqrt [expr $xsqr + $ysqr ]]
		dl_append $distlist [dl_get $distance 0]
	    }
	}
	
	set shortestdistance [dl_min $distlist]
	if { $shortestdistance < 1.25 } {
	    set lowestdist [dl_floor [expr [dl_find $distlist $shortestdistance] / 5]]
	    set exists [dl_eq $eachsac [dl_get $lowestdist 0]]
	    if { [dl_sum $exists] < 1 } {
		dl_append $eachsac [dl_get $lowestdist 0]
	    }
	    dl_delete $distlist
	}
    }
    set eachsac [dl_int $eachsac]

    #dl_local current_chunk [dl_ilist]
    #dl_local frequency [dl_ilist]

    #for { set i 0 } { $i < [dl_length $eachsac] } { incr i } {
	#if {$i == [expr [dl_length $eachsac] - 1] } {
	 #   set next_plank -1
	#} else {
	#    set next_plank [dl_get $eachsac [expr $i + 1]]
	#}
	#set current_plank [dl_get $eachsac $i]
	#dl_append $current_chunk $current_plank
	#if { $current_plank != $next_plank } {
	#    set f [dl_length $current_chunk]
	#    dl_append $frequency $f
	#    dl_reset $current_chunk
	#}
    #}

    #dl_local unique_viewed [dl_uniqueNoSort $eachsac]
    #dl_local final_list [dl_select $unique_viewed [dl_gte $frequency 10]]
    #dl_local final_list [dl_uniqueNoSort $final_list]
    
    #dl_local unique [dl_uniqueNoSort $eachsac]
    #dl_local frequency [dl_countOccurences $eachsac $unique]
    #dl_local frequency [dl_countOccurences $eachsac $eachsac]
    #dl_local fixated [dl_select $eachsac [dl_gte $frequency 20]]
    #dl_local final_list [dl_uniqueNoSort $fixated]
    #dl_local final_list [dl_select $unique [dl_gt $frequency 20]]
    dl_return $eachsac
    #dl_return $final_list
}

proc saccade_plank_per_trial { group trial } {
    set stimon [dl_get $group:stimon $trial]
    set response [dl_get $group:response $trial]
    dl_local valid_sacs [dl_between $group:sactimes:$trial $stimon $response]
    dl_local sactos [dl_select $group:sactos:$trial $valid_sacs]
    dl_local sac_1 [dl_unpack [dl_choose $sactos [dl_llist 0]]]
    dl_local sac_2 [dl_unpack [dl_choose $sactos [dl_llist 1]]]
    dl_local sac_x [dl_select $sac_1 [dl_gte $sac_2 -6]]
    dl_local sac_y [dl_select $sac_2 [dl_gte $sac_2 -6]]

    dl_local eachsac [dl_flist]

    for { set i 0 } { $i < [dl_length $sac_x] } { incr i } {
	dl_local distlist [dl_flist]

	for { set planks 0 } { $planks < 10 } { incr planks } {
	    set sx [dl_get $group:world#sx:$trial $planks]
	    set sy [dl_get $group:world#sy:$trial $planks]
	    set spin [dl_get $group:world#spin:$trial $planks]
	    set tx [dl_get $group:world#tx:$trial $planks]
	    set ty [dl_get $group:world#ty:$trial $planks]

	    dl_local x [dl_mult $sx [dl_flist -.5 .5 .5 -.5  ]]
	    dl_local y [dl_mult $sy [dl_flist -.5  -.5 .5 .5  ]]

	    set cos_theta [expr cos(-1*$spin*($::pi/180.))]
	    set sin_theta [expr sin(-1*$spin*($::pi/180.))]

	    dl_local rotated_x [dl_sub [dl_mult $x $cos_theta] [dl_mult $y $sin_theta]]
	    dl_local rotated_y [dl_add [dl_mult $y $cos_theta] [dl_mult $x $sin_theta]]

	    dl_local x [dl_add $tx $rotated_x]
	    dl_local y [dl_add $ty $rotated_y]

	    dl_append $x $tx
	    dl_append $y $ty

	    for { set a 0 } { $a < [dl_length $x] } { incr a } {
		set xval [expr [dl_get $x $a] - [dl_get $sac_x $i]]
		set xsqr [expr $xval * $xval]
		set yval [expr [dl_get $y $a] - [dl_get $sac_y $i]]
		set ysqr [expr $yval * $yval]
		set distance [dl_sqrt [expr $xsqr + $ysqr ]]
		dl_append $distlist [dl_get $distance 0]
	    }
	}
	set shortestdistance [dl_min $distlist]
	if { $shortestdistance < 1.25 } {
	    set lowestdist [dl_floor [expr [dl_find $distlist $shortestdistance] / 5]]
	    set exists [dl_eq $eachsac [dl_get $lowestdist 0]]
	    if { [dl_sum $exists] < 1 } {
		dl_append $eachsac [dl_get $lowestdist 0]
	    }
	    dl_delete $distlist
	}
    }
    set eachsac [dl_int $eachsac]
    dl_return $eachsac
}

proc saccade_planks { g } {
    dl_set $g:em_plank_assignment [dl_llist]
    for { set i 0 } { $i < [dl_length $g:rts] } { incr i } {
	#dl_append $g:saccade_plank_assignment [saccade_plank_per_trial $g $i]
	dl_append $g:em_plank_assignment [sample_plank_per_trial $g $i]
	
    }
    #dl_set $g:saccade_plank_assignment [dl_uniqueNoSort $g:saccade_plank_assignment]
}

proc smoothsacs { g valid_sacs control } {
    set nsteps 25

    if { $control == 1 } {
	dl_set $g:sactos [dl_permute $g:sactos [dl_randfill [dl_length $g:sactos]]]
	#dl_set $g:sactos [dl_shuffleList $g:sactos]

    }
    dl_local sactos [dl_select $g:sactos $valid_sacs]

    dl_local sac_x [dl_unpack [dl_choose $sactos [dl_llist [dl_llist 0]]]]
    dl_local sac_y [dl_unpack [dl_choose $sactos [dl_llist [dl_llist 1]]]]

    dl_local sac_x1 [dl_select $sac_x [dl_between $sac_y -10 10]]
    dl_local sac_y1 [dl_select $sac_y [dl_between $sac_y -10 10]]


    # Get deltas to step from one x,y pos to next
    dl_local dx [dl_div [dl_diff $sac_x] $nsteps]
    dl_local dy [dl_div [dl_diff $sac_y] $nsteps]

    # Replicate each dx for nsteps
    dl_local dx [dl_choose [dl_deepPack $dx] [dl_llist [dl_llist [dl_zeros $nsteps]]]]
    dl_local dy [dl_choose [dl_deepPack $dy] [dl_llist [dl_llist [dl_zeros $nsteps]]]]

    # And convert to cumulative sums
    dl_local cumdx [dl_cumsum $dx]
    dl_local cumdy [dl_cumsum $dy]

    # Select all but last position
    dl_local all_but_last_x [dl_select $sac_x [dl_not [dl_lastPos $sac_x]]]
    dl_local all_but_last_y [dl_select $sac_y [dl_not [dl_lastPos $sac_y]]]

    # And add the cumulative sums to interpolate from point to point
    dl_local interpx [dl_add $cumdx $all_but_last_x]
    dl_local interpy [dl_add $cumdy $all_but_last_y]

    #Check if there happen to be any trials with no valid saccades - this should be rare but can happen
    if { [dl_length [dl_findAll [dl_lengths $interpx] 0]] > 0 } {
	dl_local no_sac_idx [dl_findAll [dl_lengths $interpx] 0]
	for { set id 0 } { $id < [dl_length $no_sac_idx] } { incr id } {
	    set to_replace [dl_get $no_sac_idx $id]
	    dl_set $interpx:$to_replace [dl_llist [dl_repeat 0.00 25] [dl_repeat 0.00 25]]
	    dl_set $interpy:$to_replace [dl_llist [dl_repeat 0.00 25] [dl_repeat 0.00 25]]
	}
    }


    dl_local sac_x1 [dl_unpack $interpx]
    dl_local sac_y1 [dl_unpack $interpy]

    dl_return [dl_llist $sac_x1 $sac_y1]
}


proc smoothsacs_decoding { g network_type } {
    set nsteps 25
    #dl_local ems [dl_transpose $g:${network_type}_decoding]
    dl_local ems [dl_transpose $g:trajectory]


    dl_local good_emsx1 $ems:0
    dl_local good_emsy1 $ems:1

    # Get deltas to step from one x,y pos to next
    dl_local dx [dl_div [dl_diff $good_emsx1] $nsteps]
    dl_local dy [dl_div [dl_diff $good_emsy1] $nsteps]

    # Replicate each dx for nsteps
    dl_local dx [dl_choose [dl_deepPack $dx] [dl_llist [dl_llist [dl_zeros $nsteps]]]]
    dl_local dy [dl_choose [dl_deepPack $dy] [dl_llist [dl_llist [dl_zeros $nsteps]]]]

    # And convert to cumulative sums
    dl_local cumdx [dl_cumsum $dx]
    dl_local cumdy [dl_cumsum $dy]

    # Select all but last position
    dl_local all_but_last_x [dl_select $good_emsx1 [dl_not [dl_lastPos $good_emsx1]]]
    dl_local all_but_last_y [dl_select $good_emsy1 [dl_not [dl_lastPos $good_emsy1]]]

    # And add the cumulative sums to interpolate from point to point
    dl_local interpx [dl_add $cumdx $all_but_last_x]
    dl_local interpy [dl_add $cumdy $all_but_last_y]

    dl_local goodemsx1 [dl_unpack $interpx]
    dl_local goodemsy1 [dl_unpack $interpy]

    dl_return [dl_llist $goodemsx1 $goodemsy1]
}

proc smoothsacs_pre_resp { g { control 0 } } {
    set nsteps 25
    dl_local ems [get_ems_pre_response $g]
    
    if { $control == 1 } {
	dg_create temp
	
	for { set i 0 } { $i < [dl_length $ems] } { incr i } {
	    dg_addExistingList temp $ems:${i}
	}

	dl_set temp:sort_order [dl_randfill [dl_length temp:0]]
	dg_sort temp sort_order
	dl_delete temp:sort_order
	dl_local ems [dl_llist temp:0 temp:1]
	dg_delete temp
    }
    
    dl_local velocity [eye_velocity $g]
    dl_local start_inds [dl_sub [dl_div $g:response 5] 5]
    dl_local stop_inds [dl_sub [dl_div $g:endobs 5] 5]
    dl_local inds [dl_fromto $start_inds [dl_sub $stop_inds 3 ]]
    	
    dl_local goodvelocities [dl_choose $velocity $inds]

    #Velocity value of lte 10 works for humans but is too strict for monkeys. 20 works better for monkeys
    dl_local lowvelocity [dl_lte $goodvelocities 20]

    dl_local goodemsx [dl_select $ems:0 $lowvelocity]
    dl_local goodemsy [dl_select $ems:1 $lowvelocity]
    dl_local good_emsxa [dl_select $goodemsx [dl_between $goodemsx -8 8]]
    dl_local good_emsya [dl_select $goodemsy [dl_between $goodemsx -8 8]]
    dl_local good_emsx1 [dl_select $good_emsxa [dl_between $good_emsya -8 8]]
    dl_local good_emsy1 [dl_select $good_emsya [dl_between $good_emsya -8 8]]

    # Get deltas to step from one x,y pos to next
    dl_local dx [dl_div [dl_diff $good_emsx1] $nsteps]
    dl_local dy [dl_div [dl_diff $good_emsy1] $nsteps]

    # Replicate each dx for nsteps
    dl_local dx [dl_choose [dl_deepPack $dx] [dl_llist [dl_llist [dl_zeros $nsteps]]]]
    dl_local dy [dl_choose [dl_deepPack $dy] [dl_llist [dl_llist [dl_zeros $nsteps]]]]

    # And convert to cumulative sums
    dl_local cumdx [dl_cumsum $dx]
    dl_local cumdy [dl_cumsum $dy]

    # Select all but last position
    dl_local all_but_last_x [dl_select $good_emsx1 [dl_not [dl_lastPos $good_emsx1]]]
    dl_local all_but_last_y [dl_select $good_emsy1 [dl_not [dl_lastPos $good_emsy1]]]

    # And add the cumulative sums to interpolate from point to point
    dl_local interpx [dl_add $cumdx $all_but_last_x]
    dl_local interpy [dl_add $cumdy $all_but_last_y]

    dl_local goodemsx1 [dl_unpack $interpx]
    dl_local goodemsy1 [dl_unpack $interpy]

    dl_return [dl_llist $goodemsx1 $goodemsy1]
}

proc smoothems { g { control 0 } } {
    set nsteps 25
    dl_local ems [get_ems_post_response $g]
    
    if { $control == 1 } {
	dg_create temp
	
	for { set i 0 } { $i < [dl_length $ems] } { incr i } {
	    dg_addExistingList temp $ems:${i}
	}

	dl_set temp:sort_order [dl_randfill [dl_length temp:0]]
	dg_sort temp sort_order
	dl_delete temp:sort_order
	dl_local ems [dl_llist temp:0 temp:1]
	dg_delete temp
    }
    
    dl_local velocity [eye_velocity $g]
    dl_local start_inds [dl_sub [dl_div $g:response 5] 5]
    dl_local stop_inds [dl_sub [dl_div $g:endobs 5] 5]
    dl_local inds [dl_fromto $start_inds [dl_sub $stop_inds 3 ]]
    	
    dl_local goodvelocities [dl_choose $velocity $inds]

    #Velocity value of lte 10 works for humans but is too strict for monkeys. 20 works better for monkeys
    dl_local lowvelocity [dl_lte $goodvelocities 20]

    dl_local goodemsx [dl_select $ems:0 $lowvelocity]
    dl_local goodemsy [dl_select $ems:1 $lowvelocity]
    dl_local good_emsxa [dl_select $goodemsx [dl_between $goodemsx -8 8]]
    dl_local good_emsya [dl_select $goodemsy [dl_between $goodemsx -8 8]]
    dl_local good_emsx1 [dl_select $good_emsxa [dl_between $good_emsya -8 8]]
    dl_local good_emsy1 [dl_select $good_emsya [dl_between $good_emsya -8 8]]

    # Get deltas to step from one x,y pos to next
    dl_local dx [dl_div [dl_diff $good_emsx1] $nsteps]
    dl_local dy [dl_div [dl_diff $good_emsy1] $nsteps]

    # Replicate each dx for nsteps
    dl_local dx [dl_choose [dl_deepPack $dx] [dl_llist [dl_llist [dl_zeros $nsteps]]]]
    dl_local dy [dl_choose [dl_deepPack $dy] [dl_llist [dl_llist [dl_zeros $nsteps]]]]

    # And convert to cumulative sums
    dl_local cumdx [dl_cumsum $dx]
    dl_local cumdy [dl_cumsum $dy]

    # Select all but last position
    dl_local all_but_last_x [dl_select $good_emsx1 [dl_not [dl_lastPos $good_emsx1]]]
    dl_local all_but_last_y [dl_select $good_emsy1 [dl_not [dl_lastPos $good_emsy1]]]

    # And add the cumulative sums to interpolate from point to point
    dl_local interpx [dl_add $cumdx $all_but_last_x]
    dl_local interpy [dl_add $cumdy $all_but_last_y]

    dl_local goodemsx1 [dl_unpack $interpx]
    dl_local goodemsy1 [dl_unpack $interpy]

    dl_return [dl_llist $goodemsx1 $goodemsy1]
}


proc get_ems_post_response { g } {
    set sample_interval 5
    dl_local start_inds [dl_div $g:response $sample_interval]
    #if { [dl_eq $g:variant "counting" } {
	#dl_local stop_inds [dl_div [dl_add $g:response $g:hit_target_time] $sample_interval]
    #} else {
    #dl_local stop_inds [dl_div [dl_add $g:response $g:hit_target_time] $sample_interval] 
    #}
    dl_local stop_inds [dl_div $g:endtrial $sample_interval]
    dl_local inds [dl_fromto $start_inds $stop_inds]

    # Get horizontal ems
    dl_local h_ems [dl_unpack [dl_choose $g:ems [dl_llist 1]]]
    dl_local selected_h_ems [dl_choose $h_ems $inds]

    dl_local v_ems [dl_unpack [dl_choose $g:ems [dl_llist 2]]]
    dl_local selected_v_ems [dl_choose $v_ems $inds]

    dl_return [dl_llist $selected_h_ems $selected_v_ems]
}

proc get_ems_pre_response { g } {
    set sample_interval 5
    # Create indices of interest (between stimon and response)
#    dl_local start_inds [dl_div $g:stimon $sample_interval]

    #dl_local start_inds [dl_div [dl_sub $g:stimon 400] $sample_interval]
    dl_local start_inds [dl_div [dl_sub $g:stimon 0] $sample_interval]
    dl_local stop_inds [dl_div $g:response $sample_interval]
    dl_local inds [dl_fromto $start_inds $stop_inds]

    # Get horizontal ems
    dl_local h_ems [dl_unpack [dl_choose $g:ems [dl_llist 1]]]
    dl_local selected_h_ems [dl_choose $h_ems $inds]

    dl_local v_ems [dl_unpack [dl_choose $g:ems [dl_llist 2]]]
    dl_local selected_v_ems [dl_choose $v_ems $inds]

    dl_return [dl_llist $selected_h_ems $selected_v_ems]
}

proc get_nsacs_pre_response { g } {
    dl_local sactimes_pre [dl_between $g:sactimes $g:stimon $g:response]
    dl_set $g:nsaccades_pre [dl_sums $sactimes_pre]
    return $g
}

proc plot_ems { g trial } {

    dl_local ems [get_ems_pre_response $g]
    dl_local sactimes [dl_select $g:sactimes \
			   [dl_between $g:sactimes $g:stimon $g:response]]
    # Get saccade times aligned to stimon
    dl_local sactimes [dl_sub $sactimes $g:stimon]

    # Draw h and v em traces
    set sample_interval 5.
    set length [expr $sample_interval*[dl_length $ems:0:$trial]]

    set p [dlp_newplot]
    dlp_addXData $p [dl_fromto 0 $length 5.]

    set h [dlp_addYData $p $ems:0:$trial]
    dlp_draw $p lines $h -linecolor $::colors(green)

    set v [dlp_addYData $p $ems:1:$trial]
    dlp_draw $p lines $v -linecolor $::colors(red)

    dlp_setyrange $p -10 10

    # Draw vertical bars for each saccade onset
    set sacx [dlp_addXData $p $sactimes:$trial]
    set sacy [dlp_addYData $p 7.5]
    dlp_draw $p markers "$sacx $sacy" -marker fcircle



    return $p
}

proc plot_valid_sacs { g } {
    dlp_setpanels [dl_max $g:nhit] 1
    dl_local subjects [dl_unique $g:subj]
    for { set i 1 } { $i < [expr [dl_max $g:nhit] + 1] } { incr i } {
	set currgroup [dg_copySelected $g [dl_eq $g:nhit $i]]
	dl_set $currgroup:trial [dl_ilist]
	for { set a 0 } { $a < [dl_length $subjects] } { incr a } {
	    dl_concat $currgroup:trial [dl_fromto 0 [dl_sum [dl_eq $currgroup:subj [dl_get $subjects $a]]]]
	}
	set currgroup [sort_by_board $currgroup]
	set length [dl_length $currgroup:rts]
	set p [dlp_newplot]
	dlp_addXData $p [dl_fromto 0 $length]
	set sactrace [dlp_addYData $p $currgroup:validsacs]
	dlp_draw $p lines $sactrace -linecolor $::colors(green)
	dlp_setxrange $p 0 $length
	set balldist [dlp_addYData $p $currgroup:balldist]
	dlp_draw $p lines $balldist -linecolor $::colors(red)
	dlp_subplot $p [expr $i - 1]
    }
    return $currgroup
}

proc plot_post_ems { g trial } {
    dl_local ems [get_ems_post_response $g]
    dl_local sactimes [dl_select $g:sactimes \
			   [dl_between $g:sactimes $g:response $g:endobs]]
    # Get saccade times aligned to stimon
    dl_local sactimes [dl_sub $sactimes $g:response]

    # Draw h and v em traces
    set sample_interval 5.
    set length [expr $sample_interval*[dl_length $ems:0:$trial]]

    set p [dlp_newplot]
    dlp_addXData $p [dl_fromto 0 $length 5.]

    set h [dlp_addYData $p $ems:0:$trial]
    dlp_draw $p lines $h -linecolor $::colors(green)

    set v [dlp_addYData $p $ems:1:$trial]
    dlp_draw $p lines $v -linecolor $::colors(red)

    dlp_setyrange $p -10 10

    # Draw vertical bars for each saccade onset
    set sacx [dlp_addXData $p $sactimes:$trial]
    set sacy [dlp_addYData $p 7.5]
    dlp_draw $p markers "$sacx $sacy" -marker fcircle



    return $p
}


proc plot_dwell_time { g } {
    dl_local sactimes [dl_select $g:sactimes \
			   [dl_between $g:sactimes $g:stimon $g:response]]
    dl_local nsacs [dl_lengths $sactimes]
    dl_local dwell_time [dl_div [dl_div $g:rts 1000.] $nsacs]
    gr::barchart $dwell_time "$g:nplanks $g:elasticity"

}

proc plot_nsacs_by_epoch {g} {
    dl_local sactimes [dl_select $g:sactimes \
			   [dl_between $g:sactimes $g:stimon $g:response]]
    dl_local nsacs [dl_lengths $sactimes]
}

########### Code for converting from degrees to pixels
# pix_w is width of target image in pixels
# deg_w is total width of image in degrees
proc deg2pix_horiz { xvals pix_w deg_w } {
    set pix_per_deg_x [expr double($pix_w)/$deg_w]
    set half_pix [expr $pix_w/2]
    dl_return [dl_round [dl_add [dl_mult $xvals $pix_per_deg_x] $half_pix]]
}

# pix_h is height of target image in pixels
# deg_h is total height of image in degrees
proc deg2pix_vert { yvals pix_h deg_h } {
    set pix_per_deg_y [expr double($pix_h)/$deg_h]
    set half_pix [expr $pix_h/2]
    dl_return [dl_round [dl_sub $pix_h \
			     [dl_add [dl_mult $yvals $pix_per_deg_y] $half_pix]]]
}

########### Code for reconstructing actual worlds

proc get_world { dg trial } {
    set w [dg_create]
        foreach l [dg_tclListnames $dg] {
	    if { [regexp "world" $l m] == 1 } {
		set name [split $l {}]
		#set name [lreplace $name 5 5]
		set name [lrange $name 6 end]
		set name [join $name {}]
		dl_set $w:$name $dg:$l:$trial
	    }
	    #if { [regexp {.+#(.+)} $l m v] == 1 } {
	    #    dl_set $w:$v $dg:$l:$trial
	    #}
    }
    return $w
}

proc print_worlds { group } {
    set width 64
    set height 64
    set x0 -10
    set y0 -10
    set x1 10
    set y1 10
    resizeps $width $height
    dl_local hidelist [dl_slist gate_center b_wall l_wall r_wall]

    for { set i 0 } { $i < [dl_length $group:id] } { incr i } {
	set world [get_world $group $i]
	set world [dg_copySelected $world [dl_not [dl_oneof $world:name $hidelist]]]
	set h [dl_get $group:nhit $i]
	set side [dl_get $group:side $i]

	clearwin
	# Setup the viewport to be the middle of the original display
	setwindow $x0 $y0 $x1 $y1
	show_world $world
	#dlg_markers ball_pos:x ball_pos:y fcircle -color [dlg_rgbcolor 120 120 120] -size 0.5x
	if { $side == 0 } {
	    set fileside "left"
	} elseif { $side == 1 } {
	    set
	}
	#set filename [format "%d_${fileside}_%05d" $h $i]
	set filename "ball_test"
	dumpwin pdf [file join pdf $filename.pdf]

	# Use Tcl MuPDF to open/render/export this as a .png file
	set handle [mupdf::open pdf/$filename.pdf]
	set page [$handle getpage 0]
	$page savePNG [file join png $filename.png]
	mupdf::close $handle

	# Save this world so we can re-simulate if we want
	dg_rename $world $filename
	dg_write $filename [file join world $filename.dgz]
	dg_delete $filename
    }
}

proc try_jitter_combinations { group repetitions { scales 2 } } {
    set returngroup [dg_create]
    dl_set $returngroup:scale [dl_ilist]
    dl_set $returngroup:slope [dl_flist]
    dl_set $returngroup:rsqr [dl_flist]
    foreach scale $scales {
	set thisgroup [average_difficulty_per_trial $group $repetitions $scale]
	set outname [format jittertest_%02d.dgz $scale]
	set outdir jittertests
	dg_write $thisgroup [file join $outdir $outname]
	dl_append $returngroup:scale $scale
	dl_append $returngroup:slope [lindex [dl_lsfit $thisgroup:difficulty $thisgroup:status] 0]
	dl_append $returngroup:rsqr [lindex [dl_lsfit $thisgroup:difficulty $thisgroup:status] 2]
	dg_delete $thisgroup
    }
    return $returngroup
}

proc jitter_world { dg trial scale } {
    dl_local jitter [dl_zrand 1000]
    set world [get_world $dg $trial]
    set transscale [expr 10 - $scale]
    for { set i 0 } { $i < 10 } { incr i } {
	set currentx [dl_sub [dl_get $world:tx $i] [expr [dl_pickone $jitter]/ $transscale]]
	set currenty [dl_sub [dl_get $world:ty $i] [expr [dl_pickone $jitter]/ $transscale]]
	set currentspin [dl_sub [dl_get $world:spin $i] [expr [dl_pickone $jitter] * $scale]]
	dl_set $world:tx [dl_replaceByIndex $world:tx $i $currentx]
	dl_set $world:ty [dl_replaceByIndex $world:ty $i $currenty]
	dl_set $world:spin [dl_replaceByIndex $world:spin $i $currentspin]
    }
    return $world
}

proc average_difficulty_per_trial { group repetitions scale } {
    dl_set $group:difficulty [dl_flist]
    for { set t 0 } { $t < [dl_length $group:rts] } { incr t } {
	dl_local sims [dl_flist]
	for { set i 0 } { $i < $repetitions } { incr i } {
	    set world [jitter_world $group $t $scale]
	    set sim_result [lindex [do_simulation $world center] 0]
	    set hit_point [lindex [do_simulation $world center] 1]
	    if { $sim_result == "target" || $sim_result == "target_left" || $sim_result == "target_right" } {
		dl_append $sims 1
	    } elseif { $sim_result == "target_lure" || $sim_result == "target_left_lure" || $sim_result == "target_right_lure" } {
		dl_append $sims 0
	    } elseif { $sim_result == "l_wall" } {
		if { [dl_get $group:side $t] == 0 } {
		    dl_append $sims 0.5
		} elseif { [dl_get $group:side $t] == 1 } {
		    dl_append $sims 0
		}
	    } elseif { $sim_result == "r_wall" } {
		if { [dl_get $group:side $t] == 1 } {
		    dl_append $sims 0.5
		} elseif { [dl_get $group:side $t] == 0 } {
		    dl_append $sims 0
		}
	    } elseif { $sim_result == "b_wall"} {
		if { $hit_point < 0 && [dl_get $group:side $t] == 0 } {
		    dl_append $sims 0.75
		} elseif { $hit_point < 0 && [dl_get $group:side $t] == 1 } {
		    dl_append $sims 0
		} elseif { $hit_point > 0 && [dl_get $group:side $t] == 1 } {
		    dl_append $sims 0.75
		} elseif { $hit_point > 0 && [dl_get $group:side $t] == 0 } {
		    dl_append $sims 0
		}
	    }
	    dg_delete $world
	}
	dl_set $sims [dl_sub 1 $sims]
	dl_append $group:difficulty [dl_mean $sims]
	dl_delete $sims
    }
    return $group
}

proc unique_outcomes { group repetitions scale } {
    dl_set $group:noutcomes [dl_flist]
    dl_set $group:hit [dl_llist]
    dl_set $group:outcomes [dl_llist]
    for { set t 0 } { $t < [dl_length $group:rts] } { incr t } {
	dl_local outcomes [dl_slist]
	for { set i 0 } { $i < $repetitions } { incr i } {
	    set world [jitter_world $group $t $scale]
	    set sim_result [lindex [do_simulation $world center] 0]
	    set hit_point [lindex [do_simulation $world center] 1]
	    if { $sim_result == "target" || $sim_result == "target_left" || $sim_result == "target_right" } {
		set outcome "target"
	    } elseif { $sim_result == "target_lure" || $sim_result == "target_left_lure" || $sim_result == "target_right_lure" } {
		set outcome "lure"
	    } elseif { $sim_result == "b_wall" && $hit_point < 0 } {
		set outcome "bottom_left"
	    } elseif { $sim_result == "b_wall" && $hit_point > 0 } {
		set outcome "bottom_right"
	    } elseif { $sim_result == "r_wall" } {
		set outcome "r_wall"
	    } elseif { $sim_result == "l_wall" } {
		set outcome "l_wall"
	    }
	    dl_append $outcomes $outcome
	}

	set unique [dl_length [dl_unique $outcomes]]
	set alphabetical [dl_bsort [dl_unique $outcomes]]
	dl_local recoded [dl_recode $outcomes]
	set hist [dl_hist $recoded [dl_min $recoded] [expr [dl_max $recoded] + 1] $unique]
	dl_local total [dl_ilist]
	dl_local actual_outcomes [dl_slist]
	while { [dl_sum $total] < [expr 0.95 * $repetitions] } {
	    set index [dl_maxIndex $hist]
	    dl_append $total [dl_get $hist $index]
	    dl_append $actual_outcomes [dl_get $alphabetical $index]
	    set hist [dl_replaceByIndex $hist $index 0]
	}
	set unique_top [dl_length $total]
	dl_append $group:noutcomes $unique_top
	dl_append $group:hit $outcomes
	dl_append $group:outcomes $actual_outcomes
    }
    return $group
}


proc jitter_simulation { dg trial } {
    dl_local jitter [dl_flist  -1 -0.5 0.5 1]
    dl_local sims [dl_ilist]
    for { set i 0 } { $i < [dl_length $jitter] } { incr i } {
	set thisworld [get_world $dg $trial]
	dl_set $thisworld:sx [dl_replaceByIndex $thisworld:sx 13 3]
	dl_set $thisworld:tx [dl_replaceByIndex $thisworld:tx 14 [dl_get $jitter $i]]
	set sim_result [lindex [do_simulation $thisworld center] 0]

	if { $sim_result == "target" || $sim_result == "target_left" || $sim_result == "target_right" } {
	    dl_append $sims 1
	} else {
	    dl_append $sims 0
	}
    }
    dl_return $sims
}

proc measure_difficulty { group } {
    dl_set $group:jitter_result [dl_llist]
    for { set i 0 } { $i < [dl_length $group:trial] } { incr i } {
	set simresults [jitter_simulation $group $i]
	dl_append $group:jitter_result $simresults
    }
    dl_set $group:difficulty [dl_means $group:jitter_result]
    return $group
}

proc measure_difficulty_per_subject { subjectlist } {
    set returngroup [dg_create]
    dl_set $returngroup:subj [dl_ilist]
    dl_set $returngroup:diff [dl_flist]
    dl_set $returngroup:rts [dl_flist]
    for { set i 0 } { $i < [dl_length $subjectlist] } { incr i } {
	set currentsubj [dl_get $subjectlist $i]
	set current [make_group $currentsubj]
	set new [measure_difficulty $current]
	puts "simulations complete"
	set difficulties [dl_unique $new:difficulty]
	for { set step 0 } { $step < [dl_length $difficulties] } { incr step } {
	    dl_append $returngroup:subj $currentsubj
	    dl_append $returngroup:diff [dl_get $difficulties $step]
	    set selection [dl_eq $new:difficulty [dl_get $difficulties $step]]
	    dl_append $returngroup:rts [dl_mean [dl_select $new:rts $selection]]
	}
    }
    return $returngroup
}

proc pairwise_means { group } {
    set returnmatrix [dg_create]
    dl_local plankshit [dl_unique $group:nhit]
    dl_local difflevels [dl_unique $group:ball_diff]
    for { set diff 0 } { $diff < [dl_length $difflevels] } { incr diff } {
	dl_set $returnmatrix:d[dl_get $difflevels $diff] [dl_flist]
	for { set plank 0 } { $plank < [dl_length $plankshit] } { incr plank } {
		set currmean [dl_mean [dl_select $group:rts_pctile [dl_mult [dl_eq $group:nhit [dl_get $plankshit $plank]] [dl_eq $group:ball_diff [dl_get $difflevels $diff]]]]]
		#puts $currmean
		dl_append $returnmatrix:d[dl_get $difflevels $diff] $currmean
	}
    }
    return $returnmatrix
}

proc show_world { w } {
    global nworld floor blocks sphere
    set nbodies [dl_length $w:type]

    for { set i 0 } { $i < $nbodies } { incr i } {
	set sx [dl_get $w:sx $i]
	set sy [dl_get $w:sy $i]
	set sz [dl_get $w:sz $i]
	set tx [dl_get $w:tx $i]
	set ty [dl_get $w:ty $i]
	set tz [dl_get $w:tz $i]
	set color_r [dl_get $w:color_r $i]
	set color_g [dl_get $w:color_g $i]
	set color_b [dl_get $w:color_b $i]
	set spin [dl_get $w:spin $i]
	set mass [dl_get $w:mass $i]
	set dynamic [dl_get $w:dynamic $i]
	set elasticity [dl_get $w:elasticity $i]
	set show [dl_get $w:show $i]
	set name [dl_get $w:name $i]


	if { [dl_get $w:type $i] == "box" } {
	    set body [show_box $name $tx $ty $tz $sx $sy $sz $spin 0 0 1]
	} elseif { [dl_get $w:type $i] == "sphere" } {
	    set r [expr int($color_r*256)]
	    set g [expr int($color_g*256)]
	    set b [expr int($color_b*256)]
	    set color [dlg_rgbcolor $r $g $b]
	    set body [show_sphere $tx $ty $tz $sx $sy $sz $color]
	}
    }
}

proc show_box { name tx ty tz sx sy sz spin rx ry rz } {

    dl_local x [dl_mult $sx [dl_flist -.5 .5 .5 -.5 -.5 ]]
    dl_local y [dl_mult $sy [dl_flist -.5  -.5 .5 .5 -.5 ]]

    set cos_theta [expr cos(-1*$spin*($::pi/180.))]
    set sin_theta [expr sin(-1*$spin*($::pi/180.))]

    dl_local rotated_x [dl_sub [dl_mult $x $cos_theta] [dl_mult $y $sin_theta]]
    dl_local rotated_y [dl_add [dl_mult $y $cos_theta] [dl_mult $x $sin_theta]]

    dl_local x [dl_add $tx $rotated_x]
    dl_local y [dl_add $ty $rotated_y]

    #if { $spin > 0 } {
	#dlg_lines $x $y -filled 1 -fillcolor $::colors(cyan) -linecolor $::colors(cyan)
    #} elseif { $spin < 0 } {
#	dlg_lines $x $y -filled 1 -fillcolor $::colors(yellow) -linecolor $::colors(yellow)
 #   } else {	
	dlg_lines $x $y  -fillcolor $::colors(white) -linecolor $::colors(black)
  #  }
    set plank_number [lindex [split $name {}] 5]
    set plank [lindex [split $name {}] 0]
    if {$plank == "p" } {
#      dlg_text $tx $ty $plank_number -color $::colors(red) -size 10 -just 1
    }
}

proc show_sphere { tx ty tz sx sy sz color } {
    dlg_markers $tx $ty fcircle -size $sx -scaletype x -color $color
}

proc do_sim_collision { body_dict effect } {
    # if we've hit something other than the target (not planks), we're done

    set has_already_hit ""

    if { $::collided_with != "" } {
	set has_already_hit [lindex $::collided_with 0]
	if { $has_already_hit != "target" } {
	    return
	}
    }

    set d [eval dict create $body_dict]
    set bodies [newton::effectGetBodies $::nworld(world) $effect]
    set point [newton::effectGetContactPoint $::nworld(world) $effect]

    set b0 [dict get $d [lindex $bodies 0]]
    set b1 [dict get $d [lindex $bodies 1]]

    # This catches cases where the ball bounces out of the catcher
    if { $has_already_hit == "target" } {
	if { [string match ?_wall $b1] } {
	    set ::collided_with "$b1 $point $::sim_time"
	}
	return
    }

#    puts "$b0 $b1 $::sim_time"

    if { [string match plank* $b1] } {
	incr ::plank_count
	lappend ::planks_hit $b1
	return
    } elseif { [string match ?_wall $b1] } {
	set ::collided_with "$b1 $point $::sim_time"
    } elseif { ($b1 == "catcher1") || ($b1 == "catcher2") } {
	set ::collided_with "$b1 $point $::sim_time"
    } elseif { [string match catcher*_* $b1] } {
	set ::collided_with "$b1 $point $::sim_time"
    } elseif { [string match plat* $b1] } {
	set ::collided_with "$b1 $point $::sim_time"
    }
}

proc do_simulation { w side } {
    global nworld floor blocks sphere
    set world [newton::create]
    set nworld(world) $world

    # Use this dictionary to track body names and newton_ids
    set body_list [list]

    set nbodies [dl_length $w:type]
    for { set i 0 } { $i < $nbodies } { incr i } {
	set sx [dl_get $w:sx $i]
	set sy [dl_get $w:sy $i]
	set sz [dl_get $w:sz $i]
	set tx [dl_get $w:tx $i]
	set ty [dl_get $w:ty $i]
	set tz [dl_get $w:tz $i]
	set color_r [dl_get $w:color_r $i]
	set color_g [dl_get $w:color_g $i]
	set color_b [dl_get $w:color_b $i]
	set spin [dl_get $w:spin $i]
	set mass [dl_get $w:mass $i]
	set dynamic [dl_get $w:dynamic $i]
	set elasticity [dl_get $w:elasticity $i]
	set show [dl_get $w:show $i]
	set name [dl_get $w:name $i]

	if { [dl_get $w:type $i] == "box" } {
	    set body [make_sim_box $world $tx $ty $tz $sx $sy $sz $spin 0 0 1 $dynamic]
	    if { $name == "gate_left" && $side == "left" } { set platform $body }
	    if { $name == "gate_right" && $side == "right" } { set platform $body }
	    if { $name == "gate_center" && $side == "center" } { set platform $body }

	} elseif { [dl_get $w:type $i] == "sphere" } {
	    set body [make_sim_sphere $world $tx $ty $tz $sx $sy $sz $mass $dynamic]
	}

	lappend body_list $body
	lappend body_list $name

	if { $name == "ball_left" } { set ball_id_left $body }
	if { $name == "ball_right" } { set ball_id_right $body }
	if { $name == "ball_center" } { set ball_id_center $body }
    }

    set id [newton::materialCreateGroupID $world]
    if { $side == "center" } {
	newton::bodySetMaterialGroupID $world $ball_id_center $id
    } else {
	newton::bodySetMaterialGroupID $world $ball_id_left $id
	newton::bodySetMaterialGroupID $world $ball_id_right $id
    }
    newton::materialSetCollisionCallback $world 0 $id [list do_sim_collision $body_list] 2.
    newton::materialSetDefaultElasticity \
	$world 0 $id $elasticity

    if { ![dg_exists ball_pos] } { dg_create ball_pos }
    dl_set ball_pos:time [dl_flist]
    dl_set ball_pos:x [dl_flist]
    dl_set ball_pos:y [dl_flist]

    set ::collided_with {}
    set ::plank_count 0
    set ::planks_hit {}

    # Force simulation to 100Hz to match actual psychophysics
    set elapsed 0.01

    set sim_duration 9.0;	# open gate at 1.0 and sim for 8.0 more
    set gate_open 0

    for { set t 0 } { $t < $sim_duration } { set t [expr $t+$elapsed] } {
	set ::sim_time [expr $t-1.0]
	if { !$gate_open && $::sim_time > 1.0 } {
	    open_sim_gate $platform
	    set gate_time $t
	    set gate_open 1
	}

	newton::update $world $elapsed
#	dl_local m [newton::bodyGetMatrix $world [set ball_id_center]]
	dl_local m [newton::bodyGetMatrix $world [set ball_id_${side}]]
	dl_local cur_translation [newton::mat4_getTranslation $m]

	dl_append ball_pos:time $t
	dl_append ball_pos:x [dl_get $cur_translation 0]
	dl_append ball_pos:y [dl_get $cur_translation 1]
    }

    newton::destroy $world
    set end_time [lindex $::collided_with 4]
    dl_local dx [dl_diff [dl_select ball_pos:x [dl_lte ball_pos:time $end_time]]]
    dl_local dy [dl_diff [dl_select ball_pos:y [dl_lte ball_pos:time $end_time]]]

    dl_local valid_times [dl_between ball_pos:time $gate_time [expr [expr $gate_time + $end_time] - 1.0]]
    dl_local trajectory [dl_llist \
			     [dl_select ball_pos:x $valid_times] \
			     [dl_select ball_pos:y $valid_times]]

    dl_set ball_pos:trajectory_x $trajectory:0
    dl_set ball_pos:trajectory_y $trajectory:1
    #dl_set ball_pos:distances [dl_sqrt [dl_add [dl_mult $dx $dx] [dl_mult $dy $dy]]]

    #set ::planks [dl_pack [dl_transpose $::planks_hit]]
    set distance [dl_sum [dl_sqrt [dl_add [dl_mult $dx $dx] [dl_mult $dy $dy]]]]
    set plank_ids [regsub -all plank $::planks_hit {}]
    set planks_hit [dl_uniqueNoSort [dl_int [regsub -all plank $::planks_hit {}]]]
    set nhit [dl_length $planks_hit]
    set result "$::collided_with $::plank_count $distance [list $plank_ids] $nhit"

    # Clean up global variables
    unset ::collided_with
    unset ::plank_count
    unset ::planks_hit

    return $result
}

proc open_sim_gate { platform } {
    newton::bodySetSimulationState $::nworld(world) $platform 0
}

proc make_sim_box { world tx ty tz sx sy sz spin rx ry rz dynamic } {
    set nbox [newton::createBox $world $sx $sy $sz]
    set box_body [newton::createBody $world $nbox]

    dl_local rot [newton::mat4_angleAxisToRotation $spin $rx $ry $rz]
    dl_local m [newton::mat4_setTranslation $rot [dl_flist $tx $ty $tz]]

    newton::bodySetMatrix $world $box_body $m

    if { $dynamic == 1 } {
	newton::bodySetMassMatrix $world $box_body 1.0 0.0 0.0 0.0
	newton::setupForceAndTorque $world $box_body
    }
    return $box_body
}

proc make_sim_sphere { world tx ty tz sx sy sz mass dynamic } {
    set nsphere [newton::createSphere $world [expr .5*$sx]]
    set sphere_body [newton::createBody $world $nsphere]

    dl_local m [newton::mat4_setTranslation [newton::mat4_identity] [dl_flist $tx $ty $tz]]

    newton::bodySetMatrix $world $sphere_body $m

    if { $dynamic == 1 } {
	set radius [expr $sx/2.0]
	set inertia [expr (2.0*$radius**2*$mass)/5.0]
	newton::bodySetMassMatrix $world $sphere_body $mass $inertia $inertia $inertia
	newton::setupForceAndTorque $world $sphere_body
    }

    return $sphere_body
}

proc pick_smaller_value { l1 l2 } {
    dl_local first_smaller [dl_lt $l1 $l2]
    dl_local retlist [dl_replace $l2 $first_smaller $l1]
    dl_return $retlist
}

proc pick_greater_value { l1 l2 } {
    dl_local first_larger [dl_gt $l1 $l2]
    dl_local retlist [dl_replace $l2 $first_larger $l1]
    dl_return $retlist
}

proc eye_velocity { group } {
# get x distances
    dl_local ems $group:ems
    dl_local x_dist [dl_llist]
    set x_len [dl_length $ems]
    for { set i 0 } { $i < $x_len } { incr i } {
	dl_local x_n $ems:$i:1
	dl_local x_dist_1 [dl_diff $x_n]
	dl_append $x_dist [dl_mult $x_dist_1 $x_dist_1]
    }

# get y distances
    dl_local y_dist [dl_llist]
    set y_len [dl_length $ems]
    for { set i 0 } { $i < $y_len } { incr i } {
	dl_local y_n $ems:$i:2
	dl_local y_dist_1 [dl_diff $y_n]
	dl_append $y_dist [dl_mult $y_dist_1 $y_dist_1]
    }

#calculate total distance
    dl_local sum_dist [dl_sqrt [dl_add $x_dist $y_dist]]
    dl_local velocity [dl_div $sum_dist 0.005]
    dl_return $velocity
}

proc blink_tf { group } {
    set velocities [eye_velocity $group]
    dl_local blinks_tf [dl_gte $velocities 400]
#    dl_local blinks [dl_findAll $blinks_tf 1]
    dl_return $blinks_tf
}

proc blink_ms { group } {
    dl_local blinks_tf [blink_tf $group]
    dl_local blink_ms [dl_mult [dl_indices $blinks_tf] 5]
    dl_return $blink_ms
}

proc ems_minus_blinks { group } {
    set ems [dg_create]
    dl_local blinks_tf [dl_eq [blink_tf $group] 1]
    dl_set $ems:sorted_ems_x [dl_llist]
    dl_set $ems:sorted_ems_y [dl_llist]
    set length [dl_length $group:ems]
    for { set i 0 } { $i < $length } { incr i } {
	dl_local sorted_x [dl_select $group:ems:$i:1 $blinks_tf:$i]
	dl_append $ems:sorted_ems_x $sorted_x
	dl_local sorted_y [dl_select $group:ems:$i:2 $blinks_tf:$i]
	dl_append $ems:sorted_ems_y $sorted_y
    }
    return $ems
}


proc sort_by_board { group } {
    set sorted [dg_create]
    dl_set $group:total [dl_add $group:horizontal_planks $group:balldist]
    set unique [dl_length [dl_unique $group:total]]

    dl_set $sorted:boardid [dl_fromto 0 $unique]
    if { [dl_exists $group:rts] } {	
	dl_local sorted_rts [dl_sortedFunc $group:rts $group:total]
	dl_set $sorted:rts $sorted_rts:1
    }
    
    if { [dl_exists $group:rts_pctile] } {	
	dl_local sorted_rts_pctile [dl_sortedFunc $group:rts_pctile $group:total]
	dl_set $sorted:rts_pctile $sorted_rts_pctile:1
    }
    
    dl_local sorted_nhit [dl_sortedFunc $group:nhit $group:total]
    dl_local sorted_balldist [dl_sortedFunc $group:balldist $group:total]
    #dl_local sorted_hit_time [dl_sortedFunc $group:hit_target_time $group:total]
   
    if { [dl_exists $group:status] } {	
	dl_local sorted_status [dl_sortedFunc $group:status $group:total]
	dl_set $sorted:status $sorted_status:1
    }
    if { [dl_exists $group:eds] } {
	dl_local sorted_eds [dl_sortedFunc $group:eds $group:total]
	dl_set $sorted:eds $sorted_eds:1
    }
    if { [dl_exists $group:uncertainty] } {
	dl_local sorted_uncertainty [dl_sortedFunc $group:uncertainty $group:total]
	dl_set $sorted:uncertainty $sorted_uncertainty:1
    }
    if { [dl_exists $group:ambiguity#agreement] } {
	dl_local sorted_rule [dl_sortedFunc $group:ambiguity#agreement $group:total]
	dl_set $sorted:ambiguity#agreement $sorted_rule:1
    }

    if { [dl_exists $group:new_cnn_pctile] } {
	dl_local sorted_cnn [dl_sortedFunc $group:new_cnn_pctile $group:total]
	dl_set $sorted:new_cnn_pctile $sorted_cnn:1
    }
    
    #dl_local sorted_cnn_diff [dl_sortedFunc $group:cnn_diff $group:total]
    #dl_local sorted_phit [dl_sortedFunc $group:phit $group:total]
    #dl_local sorted_spin_mean [dl_sortedFunc $group:spin_mean $group:total]
    #dl_local sorted_spin_variance [dl_sortedFunc $group:spin_var $group:total]
    #dl_local sorted_validsacs [dl_sortedFunc $group:validsacs $group:total]
    if { [dl_exists $group:difficulty] } {
	dl_local sorted_difficulty [dl_sortedFunc $group:difficulty $group:total]
	dl_set $sorted:difficulty $sorted_difficulty:1
    }
    #dl_local sorted_intersection_pctile [dl_sortedFunc $group:intersection_pctile $group:total]
    if { [dl_exists $group:intersection ] } {
	dl_local sorted_intersection [dl_sortedFunc $group:intersection $group:total]
	dl_set $sorted:intersection $sorted_intersection:1
    }
    #dl_local sorted_translation [dl_sortedFunc $group:translation $group:total]


    dl_set $sorted:total $sorted_balldist:0
    dl_set $sorted:balldist $sorted_balldist:1
    dl_set $sorted:nhit [dl_int $sorted_nhit:1]


    #dl_set $sorted:uncertainty $sorted_uncertainty:1
    #dl_set $sorted:cnn_diff $sorted_cnn_diff:1
    #dl_set $sorted:phit $sorted_phit:1
    #dl_set $sorted:spin_mean $sorted_spin_mean:1
    #dl_set $sorted:spin_variance $sorted_spin_variance:1
    #dl_set $sorted:eds $sorted_eds:1
    #dl_set $sorted:validsacs $sorted_validsacs:1
    #dl_set $sorted:hit_target_time $sorted_hit_time:1

    #dl_set $sorted:intersection_pctile $sorted_intersection_pctile:1
    #dl_set $sorted:intersection $sorted_intersection:1
    #dl_set $sorted:translation $sorted_translation:1
    dl_delete $group:total
    return $sorted
}

proc average_sac_slopes { group } {
    dl_local meansacslope [dl_flist]
    set g [dg_create]
    for { set trial 0 } { $trial < [dl_length $group:stimon] } { incr trial } {
	set stimon [dl_get $group:stimon $trial]
	set response [dl_get $group:response $trial]
	dl_local valid_sacs [dl_between $group:sactimes:$trial $stimon $response]
	dl_local sactos [dl_select $group:sactos:$trial $valid_sacs]
	dl_local sac_1 [dl_unpack [dl_choose $sactos [dl_llist 0]]]
	dl_local sac_2 [dl_unpack [dl_choose $sactos [dl_llist 1]]]
        dl_local sac_x [dl_select $sac_1 [dl_gte $sac_2 -7]]
	dl_local sac_y [dl_select $sac_2 [dl_gte $sac_2 -7]]
	dl_local rise [dl_diff $sac_y]
	dl_local run [dl_diff $sac_x]
	dl_local sacslopes [dl_div $rise $run]
	set meanslope [dl_mean $sacslopes]
	dl_append $meansacslope $meanslope
    }
    dl_set $g:meansacslope $meansacslope
    return $g
    #dl_return $meansacslope
}

proc lowest_saccade { group { bounds 2.5 } } {
    set comparisongroup [dg_create]
    dl_local prediction [dl_ilist]
    dl_set $comparisongroup:eye_side [dl_ilist]
    for { set trial 0 } { $trial < [dl_length $group:rts] } { incr trial } {
	set stimon [dl_get $group:stimon $trial]
	set response [dl_get $group:response $trial]
    	dl_local valid_sacs [dl_between $group:sactimes:$trial $stimon $response]
	if { [dl_sum $valid_sacs] } {
	    dl_local sactos [dl_select $group:sactos:$trial $valid_sacs]
	    dl_local sac_1 [dl_unpack [dl_choose $sactos [dl_llist 0]]]
	    dl_local sac_2 [dl_unpack [dl_choose $sactos [dl_llist 1]]]
	    dl_local sac_x [dl_select $sac_1 [dl_gte $sac_2 -6]]
	    dl_local sac_y [dl_select $sac_2 [dl_gte $sac_2 -6]]
	    if { [dl_length $sac_x] > 0 } {
		set lowest [dl_minIndex $sac_y]
		dl_local xy [dl_flist]
		dl_append $xy [dl_get $sac_x $lowest]
		dl_append $xy [dl_get $sac_y $lowest]
		dl_local distlist [dl_flist]
		for { set planks 0 } { $planks < 10 } { incr planks } {
		    set sx [dl_get $group:world#sx:$trial $planks]
		    set sy [dl_get $group:world#sy:$trial $planks]
		    set spin [dl_get $group:world#spin:$trial $planks]
		    set tx [dl_get $group:world#tx:$trial $planks]
		    set ty [dl_get $group:world#ty:$trial $planks]

		    dl_local x [dl_mult $sx [dl_flist -.5 .5 .5 -.5  ]]
		    dl_local y [dl_mult $sy [dl_flist -.5  -.5 .5 .5  ]]

		    set cos_theta [expr cos(-1*$spin*($::pi/180.))]
		    set sin_theta [expr sin(-1*$spin*($::pi/180.))]

		    dl_local rotated_x [dl_sub [dl_mult $x $cos_theta] [dl_mult $y $sin_theta]]
		    dl_local rotated_y [dl_add [dl_mult $y $cos_theta] [dl_mult $x $sin_theta]]

		    dl_local x [dl_add $tx $rotated_x]
		    dl_local y [dl_add $ty $rotated_y]

		    dl_append $x $tx
		    dl_append $y $ty

		    for { set z 0 } { $z < [dl_length $x] } {incr z } {
			set xval [expr [dl_get $x $z] - [dl_get $xy 0]]
			set xsqr [expr $xval * $xval]
			set yval [expr [dl_get $y $z] - [dl_get $xy 1]]
			set ysqr [expr $yval * $yval]
			set distance [dl_sqrt [expr $xsqr + $ysqr ]]
			dl_append $distlist [dl_get $distance 0]
		    }
		}

		set lowestdist [dl_floor [expr [dl_find $distlist [dl_min $distlist]] / 5]]
		set pspin [dl_get $group:world#spin:$trial [dl_get $lowestdist 0]]
	        set x_pos [dl_get $group:world#tx:$trial [dl_get $lowestdist 0]]
		set x_len [dl_get $group:world#sx:$trial [dl_get $lowestdist 0]]

		if { $x_pos < 0 } {
		    dl_append $comparisongroup:eye_side 0
		} else {
		    dl_append $comparisongroup:eye_side 1
		}

		dl_local selectionlist [dl_repeat 0 [expr 5 * [dl_get $lowestdist 0]]]
		dl_local toreplace [dl_repeat 1 5]
		dl_concat $selectionlist $toreplace
		set remainder [expr 50 - [dl_length $selectionlist]]
		dl_local rest [dl_repeat 0 $remainder]
		dl_concat $selectionlist $rest

		set distlist1 [dl_replace $distlist $selectionlist 100]
		set secondlowest [dl_floor [expr [dl_find $distlist1 [dl_min $distlist1]] / 5]]
		set secondspin [dl_get $group:world#spin:$trial [dl_get $secondlowest 0]]
		set second_x [dl_get $group:world#tx:$trial [dl_get $secondlowest 0]]

		if { $x_pos > [expr $bounds * -1] && $x_pos < $bounds} {
		    if { $pspin < 0 } {
			dl_append $prediction 0
		    } elseif { $pspin > 0 } {
			dl_append $prediction 1
		    } elseif { $pspin == 0 } {
			if { $second_x > [expr $bounds * -1 ] && $second_x < $bounds } {
			    if { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x <= [expr $bounds * -1] } {
			    if { $secondspin > 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x >= $bounds } {
    			    if { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			}
		    }
		} elseif { $x_pos <= [expr $bounds * -1] } {
		    if { $pspin > 0 } {
			#dl_append $prediction 0
			if { [expr $x_pos + [expr $x_len / 2]] > -1.5 } {
			    dl_append $prediction 1
			} else {
			    dl_append $prediction 0
			}
		    } elseif { $pspin < 0 } {
			dl_append $prediction 0
		    } elseif { $pspin == 0 } {
			if { $second_x > [expr $bounds * -1 ] && $second_x < $bounds } {
			    if { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x <= [expr $bounds * -1] } {
			    if { $secondspin > 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x >= $bounds } {
    			    if { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			}
		    }
		} elseif { $x_pos >= $bounds } {
		    if { $pspin > 0 } {
			dl_append $prediction 1
		    } elseif { $pspin < 0 } {
			#dl_append $prediction 1
			if { [expr $x_pos - [expr $x_len / 2]] < 1.5 } {
			    dl_append $prediction 0
			} else {
			    dl_append $prediction 1
			}
		    } elseif { $pspin == 0 } {
			if { $second_x > [expr $bounds * -1 ] && $second_x < $bounds } {
			    if { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x <= [expr $bounds * -1] } {
			    if { $secondspin > 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x >= $bounds } {
    			    if { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			}
		    }
		}
	    } else { dl_append $prediction 2
	    }
	} else {dl_append $prediction 2
	}
    }
    dl_set $comparisongroup:trialnumber [dl_fromto 0 [dl_length $group:rts]]
    dl_set $comparisongroup:nhit $group:nhit
    dl_set $comparisongroup:balldist $group:balldist
    dl_set $comparisongroup:response $group:resp
    dl_set $comparisongroup:status $group:status
    dl_set $comparisongroup:prediction $prediction
    dl_set $comparisongroup:correct [dl_eq $group:resp $prediction]
    dl_set $comparisongroup:logicalselection [dl_lt $comparisongroup:prediction 2]
    #dg_select $comparisongroup $comparisongroup:logicalselection
    return $comparisongroup
}


proc valid_saccades { subjectlist } {
    set validgroup [dg_create]
    #dl_local valid [dl_ilist]
    for { set subj 0 } { $subj < [dl_length $subjectlist] } { incr subj } {
	set group [make_group [dl_get $subjectlist $subj]]
	dl_set $validgroup:s[dl_get $subjectlist $subj] [dl_mean $group:validsacs]
    }
    return $validgroup
}

proc shuffle_group { group nreps } {
    set out [dg_create]
    dl_local shuffled_lists [dl_llist]
    for { set i 0 } { $i < $nreps } { incr i } {
	dl_append $shuffled_lists [dl_shuffleLists $group:saccade_plank_assignment]
    }
    dl_set $out:trial $group:trial
    dl_set $out:planks_hit $group:planks_hit
    dl_set $out:shuffled $shuffled_lists
    return $out
}

proc read_group { subjectlist } {
    set totalgroup [dg_read s[dl_get $subjectlist 0]_intersection.dgz]
    for { set s 1 } { $s < [dl_length $subjectlist] } {incr s } {
	set tempgroup [dg_read s[dl_get $subjectlist $s]_intersection.dgz]
	dg_append $totalgroup $tempgroup
	dg_delete $tempgroup
    }
    return $totalgroup
}


proc make_group { s } {
    #set totalgroup [load_data h[dl_get $s 0]_simulation_0*]
    set totalgroup [load_data l_full_${s}*.dgz]
    set trial_too_long [dl_lt [dl_add $totalgroup:response [dl_mult $totalgroup:time 1000]] 16000]
    set totalgroup [dg_copySelected $totalgroup $trial_too_long]
    
    # New lists to be added based on simulations
    dl_set $totalgroup:ballpos [dl_llist]
    dl_set $totalgroup:hit_target_time [dl_ilist]
    dl_set $totalgroup:planks_hit [dl_llist]

    set lsactos $totalgroup:sactos
    dl_local sac_2 [dl_unpack [dl_choose $lsactos [dl_llist [dl_llist 1]]]]
    dl_local good_sacs [dl_gt $sac_2 -6]
    foreach svar "sactimes sactos sacfroms sacstops sacamps sacdirs sacvels" {
	dl_set $totalgroup:$svar [dl_select $totalgroup:$svar $good_sacs]
    }

    dl_local valid_sacs [dl_between $totalgroup:sactimes $totalgroup:stimon $totalgroup:response]
    dl_set $totalgroup:validsacs [dl_sums $valid_sacs]
    dl_local bool [dl_gt [dl_sums $valid_sacs] 1]
    dg_select $totalgroup $bool
    dl_set $totalgroup:trial [dl_fromto 0 [dl_length $totalgroup:rts]]
    dl_set $totalgroup:hit_target_time [dl_int [dl_mult $totalgroup:time 1000]]

    for { set i 0 } { $i < [dl_length $totalgroup:trial] } { incr i } {
	set world [get_world $totalgroup $i]
	set sim_result [do_simulation $world center]
	#set hit_target_time [expr int([lindex $sim_result 4]*1000)]
	#set planks_hit [lindex $sim_result 7]
	dl_local ballpos [dl_llist ball_pos:x ball_pos:y]
	dl_append $totalgroup:ballpos $ballpos
	#dl_append $totalgroup:hit_target_time $hit_target_time
	#dl_append $totalgroup:planks_hit [eval dl_ilist $planks_hit]
    #}
    #dl_set $totalgroup:planks_hit [dl_uniqueNoSort $totalgroup:planks_hit]
    saccade_planks $totalgroup
    dl_set $totalgroup:shuffled $totalgroup:saccade_plank_assignment
    set finalgroup [dg_copySelected $totalgroup [dl_gt [dl_sums $totalgroup:saccade_plank_assignment] 0]]
    for { set i 0 } { $i < [dl_length $finalgroup:shuffled] } { incr i } {
	dl_set $finalgroup:shuffled:$i [dl_permute $finalgroup:shuffled:$i [dl_randfill [dl_length $finalgroup:shuffled:$i]]]
    }
    dl_set $finalgroup:trial [dl_fromto 0 [dl_length $finalgroup:rts]]

    #Quick fix for wrong side walls
    #dl_local wrong_sidewalls [dl_and [dl_or [dl_regmatch $finalgroup:world#name target_left*] [dl_regmatch $finalgroup:world#name target_right*]] [dl_eq $finalgroup:side 1]]
    #dl_local invert [dl_replace [dl_replicate [dl_llist [dl_ones 21]] [dl_length $finalgroup:ids]] $wrong_sidewalls -1]
    #dl_set $finalgroup:world#tx [dl_mult $finalgroup:world#tx $invert]
    return $finalgroup
}

proc make_groups { subjectlist } {
    set totalgroup [make_group [dl_get $subjectlist 0]]
    dg_sort $totalgroup rts
    set step [expr 100 / [dl_get [dl_float [dl_length $totalgroup:rts]] 0]]
    dl_set $totalgroup:rts_pctile [dl_fromto 0 99.996 $step]
    for { set s 1 } { $s < [dl_length $subjectlist] } {incr s } {
	set tempgroup [make_group [dl_get $subjectlist $s]]
  dg_sort $tempgroup rts
	set step [expr 100 / [dl_get [dl_float [dl_length $tempgroup:rts]] 0]]
	dl_set $tempgroup:rts_pctile [dl_fromto 0 99.996 $step]
	dg_append $totalgroup $tempgroup
	dg_delete $tempgroup
    }
    return $totalgroup
}

proc all_subjects { subjectlist } {
    set predictiontable [dg_create]
    for {set i 0 } { $i < [dl_length $subjectlist] } {incr i } {
	dl_local predictions [dl_flist]
	set currsubj [make_group [dl_get $subjectlist $i]]
	set currgroup [combined_prediction $currsubj]
	set outname [format "s%02d_prediction.dgz" [dl_get $subjectlist $i]]
	dg_write $currgroup $outname
	dl_append $predictions [dl_mean $currgroup:correct]
	dl_set $predictiontable:predictions[dl_get $subjectlist $i] $predictions
	dl_delete $predictions
    }
    return $predictiontable
}

proc random_plank { group  { bounds 2.5 } } {
    set comparisongroup [dg_create]
    dl_local prediction [dl_ilist]
    for { set trial 0 } { $trial < [dl_length $group:rts] } { incr trial } {
	set plankindex [dl_randchoose 10 1]
	set pspin [dl_get $group:world#spin:$trial [dl_get $plankindex 0]]
	set x_pos [dl_get $group:world#tx:$trial [dl_get $plankindex 0]]

	set plankindex1 [dl_randchoose 10 1]
	if { $plankindex1 == $plankindex } {
	    set plankindex1 [dl_randchoose 10 1]
	}

	set secondspin [dl_get $group:world#spin:$trial [dl_get $plankindex1 0]]
	set second_x [dl_get $group:world#tx:$trial [dl_get $plankindex1 0]]

    		if { $x_pos > [expr $bounds * -1] && $x_pos < $bounds} {
		    if { $pspin < 0 } {
			dl_append $prediction 0
		    } elseif { $pspin > 0 } {
			dl_append $prediction 1
		    } elseif { $pspin == 0 } {
			if { $second_x > [expr $bounds * -1 ] && $second_x < $bounds } {
			    if { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x <= [expr $bounds * -1] } {
			    if { $secondspin > 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x >= $bounds } {
    			    if { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			}
		    }
		} elseif { $x_pos <= [expr $bounds * -1] } {
		    if { $pspin > 0 } {
			dl_append $prediction 0
			#if { $x_len < 3.1 } {
			#    dl_append $prediction 0
			#} elseif { $x_len > 3.1 } {
			#    dl_append $prediction 1
			#}
		    } elseif { $pspin < 0 } {
			dl_append $prediction 0
		    } elseif { $pspin == 0 } {
			if { $second_x > [expr $bounds * -1 ] && $second_x < $bounds } {
			    if { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x <= [expr $bounds * -1] } {
			    if { $secondspin > 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x >= $bounds } {
    			    if { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			}
		    }
		} elseif { $x_pos >= $bounds } {
		    if { $pspin > 0 } {
			dl_append $prediction 1
		    } elseif { $pspin < 0 } {
			dl_append $prediction 1
			#if { $x_len < 3.1 } {
			#    dl_append $prediction 1
			#} elseif { $x_len > 3.1 } {
			#    dl_append $prediction 0
			#}
		    } elseif { $pspin == 0 } {
			if { $second_x > [expr $bounds * -1 ] && $second_x < $bounds } {
			    if { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x <= [expr $bounds * -1] } {
			    if { $secondspin > 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x >= $bounds } {
    			    if { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			}
		    }
		}
    }
    dl_set $comparisongroup:trialnumber [dl_fromto 0 [dl_length $group:rts]]
    dl_set $comparisongroup:nhit $group:nhit
    dl_set $comparisongroup:response $group:resp
    dl_set $comparisongroup:status $group:status
    dl_set $comparisongroup:prediction $prediction
    dl_set $comparisongroup:correct [dl_eq $group:resp $prediction]
    dl_set $comparisongroup:logicalselection [dl_lt $comparisongroup:prediction 2]
    dg_select $comparisongroup $comparisongroup:logicalselection
    return $comparisongroup
}

proc random_repeat { group bounds  n } {
    set b [dg_create]
    dl_local meanlist [dl_flist]
    for { set i 0 } { $i < $n } {incr i } {
	set a [ random_plank $group $bounds ]
	set mean [dl_mean $a:correct]
	dl_append $meanlist $mean
    }
    dl_set $b:meanlist $meanlist
    return $b
}

proc last_saccade { group { bounds 2.5 } } {
    set comparisongroup [dg_create]
    dl_local prediction [dl_ilist]
    for { set trial 0 } { $trial < [dl_length $group:rts] } { incr trial } {
	set stimon [dl_get $group:stimon $trial]
	set response [dl_get $group:response $trial]
    	dl_local valid_sacs [dl_between $group:sactimes:$trial $stimon $response]
	if { [dl_sum $valid_sacs] } {
	    dl_local sactos [dl_select $group:sactos:$trial $valid_sacs]
	    dl_local sac_1 [dl_unpack [dl_choose $sactos [dl_llist 0]]]
	    dl_local sac_2 [dl_unpack [dl_choose $sactos [dl_llist 1]]]
	    dl_local sac_x [dl_select $sac_1 [dl_gte $sac_2 -6]]
	    dl_local sac_y [dl_select $sac_2 [dl_gte $sac_2 -6]]
	    if { [dl_length $sac_x] > 0 } {
		set last [expr [dl_length $sac_x] - 1 ]
		dl_local xy [dl_flist]
		dl_append $xy [dl_get $sac_x $last]
		dl_append $xy [dl_get $sac_y $last]
		dl_local distlist [dl_flist]
		for { set planks 0 } { $planks < 10 } { incr planks } {
		    set sx [dl_get $group:world#sx:$trial $planks]
		    set sy [dl_get $group:world#sy:$trial $planks]
		    set spin [dl_get $group:world#spin:$trial $planks]
		    set tx [dl_get $group:world#tx:$trial $planks]
		    set ty [dl_get $group:world#ty:$trial $planks]

		    dl_local x [dl_mult $sx [dl_flist -.5 .5 .5 -.5  ]]
		    dl_local y [dl_mult $sy [dl_flist -.5  -.5 .5 .5  ]]

		    set cos_theta [expr cos(-1*$spin*($::pi/180.))]
		    set sin_theta [expr sin(-1*$spin*($::pi/180.))]

		    dl_local rotated_x [dl_sub [dl_mult $x $cos_theta] [dl_mult $y $sin_theta]]
		    dl_local rotated_y [dl_add [dl_mult $y $cos_theta] [dl_mult $x $sin_theta]]

		    dl_local x [dl_add $tx $rotated_x]
		    dl_local y [dl_add $ty $rotated_y]

		    dl_append $x $tx
		    dl_append $y $ty

		    for { set z 0 } { $z < [dl_length $x] } {incr z } {
			set xval [expr [dl_get $x $z] - [dl_get $xy 0]]
			set xsqr [expr $xval * $xval]
			set yval [expr [dl_get $y $z] - [dl_get $xy 1]]
			set ysqr [expr $yval * $yval]
			set distance [dl_sqrt [expr $xsqr + $ysqr ]]
			dl_append $distlist [dl_get $distance 0]
		    }
		}
		set lowestdist [dl_floor [expr [dl_find $distlist [dl_min $distlist]] / 5]]
		set pspin [dl_get $group:world#spin:$trial [dl_get $lowestdist 0]]
		set x_pos [dl_get $group:world#tx:$trial [dl_get $lowestdist 0]]
		set x_len [dl_get $group:world#sx:$trial [dl_get $lowestdist 0]]

		dl_local selectionlist [dl_repeat 0 [expr 5 * [dl_get $lowestdist 0]]]
		dl_local toreplace [dl_repeat 1 5]
		dl_concat $selectionlist $toreplace
		set remainder [expr 50 - [dl_length $selectionlist]]
		dl_local rest [dl_repeat 0 $remainder]
		dl_concat $selectionlist $rest

		set distlist1 [dl_replace $distlist $selectionlist 100]
		set secondlowest [dl_floor [expr [dl_find $distlist1 [dl_min $distlist1]] / 5]]
		set secondspin [dl_get $group:world#spin:$trial [dl_get $secondlowest 0]]
		set second_x [dl_get $group:world#tx:$trial [dl_get $secondlowest 0]]

		if { $x_pos > [expr $bounds * -1] && $x_pos < $bounds} {
		    if { $pspin < 0 } {
			dl_append $prediction 0
		    } elseif { $pspin > 0 } {
			dl_append $prediction 1
		    } elseif { $pspin == 0 } {
			if { $second_x > [expr $bounds * -1 ] && $second_x < $bounds } {
			    if { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x <= [expr $bounds * -1] } {
			    if { $secondspin > 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x >= $bounds } {
    			    if { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			}
		    }
		} elseif { $x_pos <= [expr $bounds * -1] } {
		    if { $pspin > 0 } {
			dl_append $prediction 0
			#if { $x_len < 3.1 } {
			#    dl_append $prediction 0
			#} elseif { $x_len > 3.1 } {
			#    dl_append $prediction 1
			#}
		    } elseif { $pspin < 0 } {
			dl_append $prediction 0
		    } elseif { $pspin == 0 } {
			if { $second_x > [expr $bounds * -1 ] && $second_x < $bounds } {
			    if { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x <= [expr $bounds * -1] } {
			    if { $secondspin > 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x >= $bounds } {
    			    if { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			}
		    }
		} elseif { $x_pos >= $bounds } {
		    if { $pspin > 0 } {
			dl_append $prediction 1
		    } elseif { $pspin < 0 } {
			dl_append $prediction 1
			#if { $x_len < 3.1 } {
			#    dl_append $prediction 1
			#} elseif { $x_len > 3.1 } {
			#    dl_append $prediction 0
			#}
		    } elseif { $pspin == 0 } {
			if { $second_x > [expr $bounds * -1 ] && $second_x < $bounds } {
			    if { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x <= [expr $bounds * -1] } {
			    if { $secondspin > 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 0
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			} elseif { $second_x >= $bounds } {
    			    if { $secondspin > 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin < 0 } {
				dl_append $prediction 1
			    } elseif { $secondspin == 0 } {
				dl_append $prediction 2
			    }
			}
		    }
		}
	    } else { dl_append $prediction 2
	    }
	} else {dl_append $prediction 2
	}
    }
    dl_set $comparisongroup:trialnumber [dl_fromto 0 [dl_length $group:rts]]
    dl_set $comparisongroup:nhit $group:nhit
    dl_set $comparisongroup:balldist $group:balldist
    dl_set $comparisongroup:response $group:resp
    dl_set $comparisongroup:status $group:status
    dl_set $comparisongroup:prediction $prediction
    dl_set $comparisongroup:correct [dl_eq $group:resp $prediction]
    dl_set $comparisongroup:logicalselection [dl_lt $comparisongroup:prediction 2]
    #dg_select $comparisongroup $comparisongroup:logicalselection
    return $comparisongroup
}

proc combined_prediction { group { bounds 2.5 } } {
    set lowestpred [lowest_saccade $group $bounds]
    set lastpred [last_saccade $group $bounds]
    for { set i 0 } { $i < [dl_length $lowestpred:correct] } { incr i } {
	set currlowpred [dl_get $lowestpred:prediction $i]
	set currlastpred [dl_get $lastpred:prediction $i]
	if { $currlowpred == 2  &&  $currlastpred != 2 } {
	    dl_set $lowestpred:prediction [dl_replaceByIndex $lowestpred:prediction $i $lastpred:prediction:$i]
	} elseif { $currlowpred == 2 && $currlastpred == 2 } {
	    dl_set $lowestpred:prediction [dl_replaceByIndex $lowestpred:prediction $i $lowestpred:eye_side:$i]
	}
    }
    dl_set $lowestpred:correct [dl_eq $lowestpred:response $lowestpred:prediction]
    dl_set $lowestpred:logicalselection [dl_lt $lowestpred:prediction 2]
    dg_select $lowestpred $lowestpred:logicalselection
    return $lowestpred
}

proc zerogroup { g alignto } {
    set pretime 200
    set rate [dl_first $g:ems:0:0]
    dl_local starts [dl_int [dl_div [dl_sub $g:$alignto $pretime] $rate]]
    dl_local stops [dl_add $starts [expr int($pretime/$rate)]]
    dl_local pretarginds [dl_fromto $starts $stops]
    dl_local hem [dl_unpack [dl_select $g:ems [dl_llist "0 1 0"]]]
    dl_local vem [dl_unpack [dl_select $g:ems [dl_llist "0 0 1"]]]

    dl_local hpretarg [dl_choose $hem $pretarginds]
    dl_local vpretarg [dl_choose $vem $pretarginds]

    dl_local zero [dl_float [dl_zeros [dl_length $g:ems]]]
    dl_local pre_horiz [dl_means $hpretarg]
    dl_local pre_vert [dl_means $vpretarg]

    dl_local offsets [dl_llist $zero $pre_horiz $pre_vert]
    dl_local offsets [dl_transpose $offsets]
    dl_set $g:ems [dl_sub $g:ems $offsets]

    return $g
}

proc zerogroup2 { g alignto } {
    set pretime 200
    set rate [dl_first $g:ems:0:0]
    dl_local starts [dl_int [dl_div [dl_sub $g:$alignto $pretime] $rate]]
    dl_local stops [dl_add $starts [expr int($pretime/$rate)]]
    dl_local pretarginds [dl_fromto $starts $stops]
    dl_local hem [dl_unpack [dl_select $g:ems [dl_llist "0 1 0"]]]
    dl_local vem [dl_unpack [dl_select $g:ems [dl_llist "0 0 1"]]]

    dl_local hpretarg [dl_choose $hem $pretarginds]
    dl_local vpretarg [dl_choose $vem $pretarginds]

    dl_local pre_horiz [dl_means $hpretarg]
    dl_local pre_vert [dl_means $vpretarg]
    dl_local hste [dl_flist]
    dl_local vste [dl_flist]
    for { set b 0 } { $b < [dl_length $hpretarg] } { incr b } {
	dl_local ste1 [dl_div [dl_bstds $hpretarg:$b] [dl_sqrt [dl_length $hpretarg:$b]]]
	dl_local ste2 [dl_div [dl_bstds $vpretarg:$b] [dl_sqrt [dl_length $vpretarg:$b]]]
	dl_append $hste [dl_get $ste1 0]
	dl_append $vste [dl_get $ste2 0]
	dl_delete $ste2 $ste1
    }

    dl_local offsets [dl_llist $pre_horiz $pre_vert]
    dl_local offsets [dl_transpose $offsets]
    for { set i 0 } { $i < [dl_length $g:sactos] } { incr i } {
	for { set a 0 } { $a < [dl_length $g:sactos:$i] } { incr a } {
	    dl_set $g:sactos:$i:$a [dl_sub $g:sactos:$i:$a $offsets:$i]
	}
    }
    return $g
}

proc totaloffset { g alignto } {
    set pretime 200
    set rate [dl_first $g:ems:0:0]
    dl_local starts [dl_int [dl_div [dl_sub $g:$alignto $pretime] $rate]]
    dl_local stops [dl_add $starts [expr int($pretime/$rate)]]
    dl_local pretarginds [dl_fromto $starts $stops]
    dl_local hem [dl_unpack [dl_select $g:ems [dl_llist "0 1 0"]]]
    dl_local vem [dl_unpack [dl_select $g:ems [dl_llist "0 0 1"]]]

    dl_local hpretarg [dl_choose $hem $pretarginds]
    dl_local vpretarg [dl_choose $vem $pretarginds]

    dl_local pre_horiz [dl_means $hpretarg]
    dl_local pre_vert [dl_means $vpretarg]
    dl_local hste [dl_flist]
    dl_local vste [dl_flist]
    for { set b 0 } { $b < [dl_length $hpretarg] } { incr b } {
	dl_local ste1 [dl_div [dl_bstds $hpretarg:$b] [dl_sqrt [dl_length $hpretarg:$b]]]
	dl_local ste2 [dl_div [dl_bstds $vpretarg:$b] [dl_sqrt [dl_length $vpretarg:$b]]]
	dl_append $hste [dl_get $ste1 0]
	dl_append $vste [dl_get $ste2 0]
	dl_delete $ste2 $ste1
    }

    dl_local combinedoffset [dl_flist]
    for { set i 0 } { $i < [dl_length $pre_horiz] } { incr i } {
	set a [dl_add [dl_pow $pre_horiz:$i 2] [dl_pow $pre_vert:$i 2]]
	set sqrt [dl_sqrt $a]
	dl_append $combinedoffset [dl_get $sqrt 0]
    }
    dl_return $combinedoffset
}


proc dump_all_boards { g } {
    for { set trial 0 } { $trial < [dl_length $g:side] } { incr trial } {
	clearwin
	setwindow -16 -12 16 12
	set filename "board_${trial}"
	set w [get_world $g $trial]
	do_simulation $w center	
	set w [dg_copySelected $w [dl_not [dl_oneof $w:name [dl_slist gate_center b_wall l_wall r_wall]]]]
	dlg_markers ball_pos:x ball_pos:y fcircle -color [dlg_rgbcolor 120 120 120] -size 1x
	show_world $w

	
	dumpwin pdf [file join pdf $filename.pdf]
	# Use Tcl MuPDF to open/render/export this as a .png file
	set handle [mupdf::open pdf/$filename.pdf]
	set page [$handle getpage 0]
	$page savePNG [file join png $filename.png]
	#mupdf::close $handle
    }

}

proc sort_by_board_new { group } {
    set sorted [dg_create]
    dl_set $sorted:variant [dl_slist]
    set of_interest { board_id time balldist variant_id rts rts_pctile nhit uncertainty discrete_uncertainty nsaccades}
    dg_sort $group board_id
    
    foreach l $of_interest { 
	if { [dl_exists $group:$l] } {
	    dl_local sorted_list [dl_sortedFunc $group:$l $group:board_id]
	    dl_set $sorted:$l $sorted_list:1
	}
    }

    dl_set $sorted:board_id [dl_int $sorted:board_id]
    return $sorted
}

proc write_for_matlab_human { filename subj } {
    set currgroup [load_data $filename]
    set file [dl_get $currgroup:filename 0]
    foreach l [dg_tclListnames $currgroup] {
	if { [string match world* $l] } {
	    set name [split $l {}]
	    set name [lreplace $name 5 5]
	    set name [join $name {}]
	    dl_set $currgroup:$name $currgroup:$l
	    dl_delete $currgroup:$l
	}
    }

    dl_set $currgroup:pre_resp_onsets [dl_div [dl_add $currgroup:obs_times $currgroup:stimon] 1000.00]
    dl_set $currgroup:pre_resp_durations [dl_div [dl_sub $currgroup:response $currgroup:stimon] 1000.00]
    dl_set $currgroup:response_onsets [dl_div [dl_add $currgroup:obs_times $currgroup:response] 1000.00]
    dl_set $currgroup:fixon_onsets [dl_div [dl_add $currgroup:obs_times $currgroup:fixon] 1000.00]
    dl_set $currgroup:post_resp_duration [dl_div [dl_sub $currgroup:stimoff $currgroup:response] 1000.00]   
       
    dl_delete $currgroup:ems_kern
    dl_delete $currgroup:ems_source
    dl_delete $currgroup:load_params
    dl_delete $currgroup:filename
    dl_delete $currgroup:version
    set newgroup [dg_copySelected $currgroup [dl_noteq $currgroup:variant fixation]]
    dg_write $newgroup "C:/Users/lab/Documents/MRI_data/$subj/behavior/task/$file"
}


proc write_for_matlab_monkey { filename subj session } {
    set currgroup [load_data -noresp 1 $filename]
    set file [dl_get $currgroup:filename 0]
    foreach l [dg_tclListnames $currgroup] {
	if { [string match world* $l] } {
	    set name [split $l {}]
	    set name [lreplace $name 5 5]
	    set name [join $name {}]
	    dl_set $currgroup:$name $currgroup:$l
	    dl_delete $currgroup:$l
	}
    }

    dl_set $currgroup:pre_resp_onsets [dl_div [dl_add $currgroup:obs_times $currgroup:stimon] 1000.00]
    dl_set $currgroup:pre_resp_durations [dl_div [dl_sub $currgroup:response $currgroup:stimon] 1000.00]
    dl_set $currgroup:response_onsets [dl_div [dl_add $currgroup:obs_times $currgroup:response] 1000.00]
    dl_set $currgroup:fixon_onsets [dl_div [dl_add $currgroup:obs_times $currgroup:fixon] 1000.00]
    dl_set $currgroup:post_resp_duration [dl_div [dl_sub $currgroup:stimoff $currgroup:response] 1000.00]
    
    dl_local plank_list [dl_slist plank0 plank1 plank2 plank3 plank4 plank5 plank6 plank7 plank8 plank9]
    dl_local less_than_zero [dl_sums [dl_lt [dl_select $currgroup:worldspin [dl_oneof $currgroup:worldname $plank_list]] 0]]
    dl_local greater_than_zero [dl_sums [dl_between [dl_select $currgroup:worldspin [dl_oneof $currgroup:worldname $plank_list]] 0 89]]
    dl_local plank_angle_prediction [dl_lt $less_than_zero $greater_than_zero]
    dl_local equal_numbers [dl_mult [dl_eq $less_than_zero $greater_than_zero] 2]
    dl_local plank_angle_prediction [dl_add $plank_angle_prediction $equal_numbers]
    dl_set $currgroup:plank_angle_prediction $plank_angle_prediction
    
    dl_local left_planks [dl_sums [dl_lt [dl_select $currgroup:worldtx [dl_oneof $currgroup:worldname $plank_list]] 0]]
    dl_local right_planks [dl_sums [dl_gt [dl_select $currgroup:worldtx [dl_oneof $currgroup:worldname $plank_list]] 0]]
    dl_local plank_clustering_prediction [dl_gt $left_planks $right_planks]
    dl_local equal_distribution [dl_mult [dl_eq $left_planks $right_planks] 2]
    dl_local plank_clustering_prediction [dl_add $plank_clustering_prediction $equal_distribution]
    dl_set $currgroup:plank_clustering_prediction $plank_clustering_prediction

    
    dl_delete $currgroup:ems_kern
    dl_delete $currgroup:ems_source
    dl_delete $currgroup:load_params
    dl_delete $currgroup:filename
    dl_delete $currgroup:version
    set newgroup [dg_copySelected $currgroup [dl_noteq $currgroup:variant fixation]]
    
    dl_local trajectory [dl_select $newgroup:trajectory [dl_noteq $newgroup:trajectory 0]]
    dl_local predicted [dl_ilist]
    set variant [dl_get $newgroup:variant 0]
    
    if { $variant == "counting" } {
	dl_set $newgroup:trajectory_prediction_valid [dl_repeat 2 [dl_length $newgroup:side]]
    } else {
	for {set i 0 } { $i < [dl_length $newgroup:side] } { incr i } {
	    if { [dl_get $trajectory:$i:0 0] < 0 } {
		dl_append $predicted 0
	    } elseif { [dl_get $trajectory:$i:0 0] > 0 } {
		dl_append $predicted 1
	    }
	}
	dl_set $newgroup:trajectory_prediction_valid [dl_eq $predicted $newgroup:side]
    }
    
    set subject "sub-${subj}0${session}"
    set outfolder "C:/Users/lab/Documents/MRI_monkey/Glenn/${subject}/ses-01/behavior/task"
    if { ![file exists ${outfolder}] } {
	file mkdir $outfolder
    }

    dg_write $newgroup "C:/Users/lab/Documents/MRI_monkey/Glenn/${subject}/ses-01/behavior/task/$file"

}

#example
#cd L:/projects/encounter/data/glenn
#set file_list [glob l_full_031221*]
#set subj Glenn
#set session 15
#write_all $file_list $subj $session
proc write_all { file_list subj session} {
    for { set i 0 } { $i < [llength $file_list] } { incr i } {
	write_for_matlab_monkey [lindex $file_list $i] $subj $session
	#write_for_matlab_human [lindex $file_list $i] $subj
    }
}
    
proc make_group_monkey { s dates {files -1 } } {
    if {$files == -1 } {
	set totalgroup [load_data "${s}_full_${dates}*.dgz"]
    } else {
    }
    set trial_too_long [dl_lt [dl_add $totalgroup:response [dl_mult $totalgroup:time 1000]] 16000]
    set totalgroup [dg_copySelected $totalgroup $trial_too_long]

    set lsactos $totalgroup:sactos
    dl_local sac_2 [dl_unpack [dl_choose $lsactos [dl_llist [dl_llist 1]]]]
    dl_local good_sacs [dl_gt $sac_2 -9]
    foreach svar "sactimes sactos sacfroms sacstops sacamps sacdirs sacvels" {
	dl_set $totalgroup:$svar [dl_select $totalgroup:$svar $good_sacs]
    }

    dl_local valid_sacs [dl_between $totalgroup:sactimes $totalgroup:stimon $totalgroup:response]
    dl_set $totalgroup:validsacs [dl_sums $valid_sacs]
    dl_local bool [dl_gt [dl_sums $valid_sacs] 1]
    dg_select $totalgroup $bool
    dl_set $totalgroup:trial [dl_fromto 0 [dl_length $totalgroup:rts]]
    dl_set $totalgroup:hit_target_time [dl_ilist]
    dl_set $totalgroup:hit_target_time [dl_int [dl_mult $totalgroup:time 1000]]

    saccade_planks $totalgroup
    dl_set $totalgroup:shuffled $totalgroup:saccade_plank_assignment
    set finalgroup [dg_copySelected $totalgroup [dl_gt [dl_sums $totalgroup:saccade_plank_assignment] 0]]
    for { set i 0 } { $i < [dl_length $finalgroup:shuffled] } { incr i } {
	dl_set $finalgroup:shuffled:$i [dl_permute $finalgroup:shuffled:$i [dl_randfill [dl_length $finalgroup:shuffled:$i]]]
    }
    dl_set $finalgroup:trial [dl_fromto 0 [dl_length $finalgroup:rts]]

    return $finalgroup
}

proc eye_movement_movie { group trial } {
    set width 512
    set height 512
    set x0 -10
    set y0 -10
    set x1 10
    set y1 10
    resizeps $width $height
    dl_local hidelist [dl_slist gate_center b_wall l_wall r_wall]
    
    set world [get_world $group $trial]
    set world [dg_copySelected $world [dl_not [dl_oneof $world:name $hidelist]]]
    
    clearwin
    # Setup the viewport to be the middle of the original display
    setwindow $x0 $y0 $x1 $y1
    show_world $world
    dl_local ems [get_ems_pre_response $group]

    
    for { set i 0 } { $i < [dl_length $ems:0:$trial] } { incr i } {
	dlg_markers [dl_get $ems:0:$trial $i] [dl_get $ems:1:$trial $i] -marker fcircle -color $::colors(cyan)
	
	set filename "trial_${trial}_em_${i}"
	dumpwin pdf [file join pdf $filename.pdf]

	# Use Tcl MuPDF to open/render/export this as a .png file
	set handle [mupdf::open pdf/$filename.pdf]
	set page [$handle getpage 0]
	$page savePNG [file join png $filename.png]
	#$handle closeallpages
	#mupdf::close $handle

    }
}

proc show_eye_movement_movie { group trial  { show_post 0 } { show_info 1 } } {
    set delay 10
    set x0 -10
    set y0 -10
    set x1 10
    set y1 10
    dl_local hidelist [dl_slist gate_center b_wall l_wall r_wall]
    
    set world [get_world $group $trial]
    set world [dg_copySelected $world [dl_not [dl_oneof $world:name $hidelist]]]
    
    clearwin
    # Setup the viewport to be the middle of the original display
    setwindow $x0 $y0 $x1 $y1
    show_world $world
    dl_local ems [get_ems_pre_response $group]
    dl_local pursuit [get_ems_post_response $group]
    dl_local trajectory $group:trajectory
    #puts [dl_length $pursuit:0]
    #puts [dl_length $trajectory:$trial:0]

    
    for { set i 0 } { $i < [dl_length $ems:0:$trial] } { incr i } {
	dlg_markers [dl_get $ems:0:$trial $i] [dl_get $ems:1:$trial $i] -marker fcircle -color $::colors(cyan)
	update
	after $delay
    }
    #puts "pre response done"

    if { $show_post == 1 } {
	for { set i 1 } { $i < [dl_length $pursuit:0:$trial] } { incr i } {
	    set eye_index [expr $i - 1]
	    dlg_markers [dl_get $pursuit:0:$trial $eye_index] [dl_get $pursuit:1:$trial $eye_index] -marker fcircle -color $::colors(yellow)

	    set denominator [expr [dl_length $pursuit:0:$trial] / [dl_get [dl_float [dl_length $trajectory:$trial:0]] 0]]

	    set divisible [expr [dl_get [dl_round [expr $i/$denominator]] 0]]

	    if { $divisible < [dl_length $trajectory:$trial:0] } {
		dlg_markers [dl_get $trajectory:$trial:0 $divisible] [dl_get $trajectory:$trial:1 $divisible] -marker fcircle -color $::colors(white)
	    }
	    
	    update
	    after $delay
	    #puts "$i"
	}
    }
    #puts "post response done"	      
        
    if { $show_info == 1 } {	
	set status [dl_get $group:status $trial]
	set rts [dl_get $group:rts $trial]
	set side [dl_get $group:side $trial]
	#set shadow [dl_get $group:show_shadow $trial]
	set uncertainty [dl_get $group:uncertainty $trial]
	set discrete_uncertainty [dl_get $group:discrete_uncertainty $trial]
	
	if { $side == 0 } {
	    set answer "Correct Answer: Left"
	} else {
	    set answer "Correct Answer: Right"
	}
	
	setwindow 0 0 1 1
	set text [format "Trial %d" $trial]
	dlg_text  0.98 0.95 $text -just 1 -size 14
	set text [format "Status %.2f" $status]
	dlg_text  0.98 0.9 $text -just 1
	set text [format "RT: %.2f" $rts]
	dlg_text  0.98 0.85 $text -just 1
	#set text [format "Shadow: %.2f" $shadow]
	#dlg_text  0.98 0.80 $text -just 1
	set text [format "Uncertainty: %.2f" $uncertainty]
	dlg_text  0.98 0.75 $text -just 1
	set text [format "Discrete Uncertainty: %.2f" $discrete_uncertainty]
	dlg_text  0.98 0.70 $text -just 1
    }
    
    dl_delete $ems
    dl_delete $pursuit
    dl_delete $trajectory
    puts "trial $trial complete"

}

proc show_eye_movement_movies { group } {
    set ::activator::execcmd "show_eye_movement_movie $group %n"
    activator::setup

}

proc check_congruence { group } {
    dl_local last_plank_x [dl_flist]
    
    for { set i 0 } { $i < [dl_length $group:nhit] } { incr i } {
	set planks_hit [dl_get $group:planks_hit $i]
	set last_plank_hit [dl_get $planks_hit [expr [dl_length $planks_hit] - 1]]
	dl_append $last_plank_x [dl_get [dl_get [dl_select $group:world#tx:$i [dl_eq $group:world#name plank$last_plank_hit]] 0] 0]
    }

    dl_set $group:last_plank_x $last_plank_x
    dl_local plank_side [dl_gt $last_plank_x 0]
    dl_local congruent [dl_eq $plank_side $group:side]
    dl_set $group:congruent $congruent
}

proc check_angle_rule { group } {
    
    #Figure out a way to glob world#name for this list so it doesn't need to be hardcoded
    dl_local plank_list [dl_slist plank0 plank1 plank2 plank3 plank4 plank5 plank6 plank7 plank8 plank9]

    set all_angles $group:world#spin
    set angle_group [dg_create]
    for {set i 0 } { $i < [dl_length $all_angles] } { incr i } {
	dl_local selection_list [dl_oneof $group:world#name:$i $plank_list]
	dl_set $angle_group:$i [dl_select $all_angles:$i $selection_list]
    }
    
    dl_set $group:angle_prediction [dl_ilist]

    for { set i 0 } { $i < [dl_length $group:world#spin] } { incr i } {
	set orientation_majority [dl_sum [dl_gt $angle_group:$i 0]]
	if { $orientation_majority > 5 } {
	    dl_append $group:angle_prediction 1
	} elseif { $orientation_majority < 5 } {
	    dl_append $group:angle_prediction 0
	} else {
	    dl_append $group:angle_prediction 2
	}
    }

    dl_set $group:angle_prediction_correct [dl_eq $group:side $group:angle_prediction]
    dl_set $group:angle_prediction_correct [dl_sub $group:angle_prediction_correct [dl_eq $group:angle_prediction 2]]

}


proc check_cluster_rule { group } {
    
    #Figure out a way to glob world#name for this list so it doesn't need to be hardcoded
    dl_local plank_list [dl_slist plank0 plank1 plank2 plank3 plank4 plank5 plank6 plank7 plank8 plank9]

    set all_tx $group:world#tx
    set tx_group [dg_create]
    for {set i 0 } { $i < [dl_length $all_tx] } { incr i } {
	dl_local selection_list [dl_oneof $group:world#name:$i $plank_list]
	dl_set $tx_group:$i [dl_select $all_tx:$i $selection_list]
    }
    
    dl_set $group:cluster_prediction [dl_ilist]

    for { set i 0 } { $i < [dl_length $group:world#tx] } { incr i } {
	set cluster_majority [dl_sum [dl_gt $tx_group:$i 0]]
	if { $cluster_majority > 5 } {
	    dl_append $group:cluster_prediction 0
	} elseif { $cluster_majority < 5 } {
	    dl_append $group:cluster_prediction 1
	} else {
	    dl_append $group:cluster_prediction 2
	}
    }

    dl_set $group:cluster_prediction_correct [dl_eq $group:side $group:cluster_prediction]
    dl_set $group:cluster_prediction_correct [dl_sub $group:cluster_prediction_correct [dl_eq $group:cluster_prediction 2]]

}

proc check_bounce_rule { group } {
  
    set all_trajectories [dl_select $group:trajectory [dl_noteq $group:trajectory 0]]
    dl_set $group:bounce_prediction [dl_ilist]

    for { set i 0 } { $i < [dl_length $group:trajectory] } { incr i } {
	set bounce_side [dl_get $all_trajectories:$i:0 0]
	if { $bounce_side > 0 } {
	    dl_append $group:bounce_prediction 1
	} elseif { $bounce_side < 0 } {
	    dl_append $group:bounce_prediction 0
	} else {
	    dl_append $group:bounce_prediction 2
	}
    }

    dl_set $group:bounce_prediction_correct [dl_eq $group:side $group:bounce_prediction]
    dl_set $group:bounce_prediction_correct [dl_sub $group:bounce_prediction_correct [dl_eq $group:bounce_prediction 2]]

}

proc equidistant_points { group trial } {
    dl_local dx [dl_diff $group:trajectory:${trial}:0]
    dl_local dy [dl_diff $group:trajectory:${trial}:1]
    dl_local distances [dl_sqrt [dl_add [dl_mult $dx $dx] [dl_mult $dy $dy]]]
    set distance [dl_sum $distances]
    set step_length [expr [expr $distance + 1] /16]

    dl_local cumulative_distances [dl_cumsum $distances]
    dl_local selections [dl_fromto 0 $distance $step_length]
    dl_local selection_indices [dl_ilist]

    for { set i 0 } { $i < [dl_length $selections] } { incr i } {
	dl_local off [dl_sub $cumulative_distances [dl_get $selections $i]]
	set closest [dl_find [dl_floor $off] 0]
	dl_append $selection_indices $closest
    }

    set outgroup [dg_create]
    dl_set $outgroup:x [dl_select $group:trajectory:$trial:0 $selection_indices]
    dl_set $outgroup:y [dl_select $group:trajectory:$trial:1 $selection_indices]

    return $outgroup
    
}

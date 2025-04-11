#
# VARIANTS
#   hapticvis identify
#
# DESCRIPTION
#   variant dictionary for visual and haptic identity learning
#


#
# Currently only supports 4, 6, 8 choices properly
#   The "correct_choice" is numbered 1-n_choices
#    but the centers differ depending on the number of choices so
#
package require haptic

namespace eval hapticvis::identify {

    # state system parameters
    
    variable params_defaults { delay_time 100 }
    variable params_visual {
	interblock_time 500
	pre_stim_time 100
	sample_duration 1500
	choice_delay 0
	sample_delay 500
	choice_duration 30000
	stim_duration 30000
	post_response_time 500
    }
    variable params_haptic {
	interblock_time 500
	pre_stim_time 100
	sample_delay 0
	sample_duration 5000
	choice_duration 30000
	choice_delay 0
	stim_duration 30000
	post_response_time 500
    }
    variable params_haptic_follow_dial          $params_haptic
    variable params_haptic_follow_pattern       $params_haptic
    variable params_haptic_constrained_locked   $params_haptic
    variable params_haptic_constrained_unlocked $params_haptic

    # variant description

    set subject_ids [dl_tcllist [dl_fromto 0 36]]
    set subject_sets [dl_tcllist [dl_fromto 0 5]]

    variable variants {
        visual {
            description "learn visual objects"
            loader_proc setup_visual
            loader_options {
              subject_id { $subject_ids }
              subject_set { $subject_sets }
              n_per_set { 6 }
              shape_scale { 3 4 5 6 }
              noise_type { none circles }
              n_rep { 2 4 6 8 10 20 }
              rotations {
                  {three {60 180 300}} {single {180}} 
                }
            }
        }
        haptic {
            description "learn haptic objects"
            loader_proc setup_haptic
            loader_options {
              subject_id { $subject_ids }
              subject_set { $subject_sets }
              n_per_set { 6 }
              n_rep { 2 4 6 8 10 20 }
		rotations {
                  {three {60 180 300}} {single {180}} 
                }
            }
        }
        haptic_follow_dial {
            description "learn haptic using dial"
            loader_proc setup_haptic_follow_dial
            loader_options {
              subject_id { $subject_ids }
              subject_set { $subject_sets }
              n_per_set { 4 }
              n_rep { 10 8 }
		rotations {
		    {three {60 180 300}} {single {180}}
                }
            }
        }
        haptic_follow_pattern {
            description "learn haptic with passive following"
            loader_proc setup_haptic_follow_pattern
            loader_options {
              subject_id { $subject_ids }
              subject_set { $subject_sets }
              n_per_set { 4 }
              n_rep { 10 8 }
		rotations {
		    {three {60 180 300}} {single {180}}
                }
            }
        }
	haptic_constrained_locked {
            description "learn haptic with constraint (locked)"
            loader_proc setup_haptic_constrained_locked
            loader_options {
              subject_id { $subject_ids }
              subject_set { $subject_sets }
              n_per_set { 4 }
              n_rep { 10 8 }
		rotations {
                  {three {60 180 300}} {single {180} }
                }
            }
        }
        haptic_constrained_unlocked {
            description "learn haptic with constraint (unlocked)"
            loader_proc setup_haptic_constrained_unlocked
            loader_options {
              subject_id { $subject_ids }
              subject_set { $subject_sets }
              n_per_set { 4 }
              n_rep { 10 8 }
		rotations {
                  {three {60 180 300}} {single {180}}
                }
            }
        }
    }

    # substitute variables in variant description above
    set variants [subst $variants]
    
    proc variants_init { s } {
	
        $s add_method visual_init {} {
            rmtSend "setBackground 100 100 100"
        }
	
	$s add_method add_subject_blocks { subject_id block_id dbfile shapedb } {
	    set row [dl_find [trialdb:subject $subject_id]]
	    if { $row >= 0 } { return }
	}
	
	$s add_method setup_visual { subject_id subject_set n_per_set \
					 shape_scale noise_type n_rep rotations } {
	     my setup_trials identity $subject_id $subject_set $n_per_set visual \
		 $shape_scale $noise_type $n_rep $rotations
	}
	
	$s add_method setup_haptic { subject_id subject_set n_per_set \
					 n_rep rotations } {
	     set shape_scale 1
	     set noise_type none
	     my setup_trials identity $subject_id $subject_set $n_per_set haptic \
		 $shape_scale $noise_type $n_rep $rotations
	}

	$s add_method setup_haptic_constrained_locked { \
					 subject_id subject_set n_per_set \
					 n_rep rotations } {
            set shape_scale 1
            set noise_type none
            my setup_trials contour $subject_id $subject_set $n_per_set haptic \
		 $shape_scale $noise_type $n_rep $rotations
            dl_set stimdg:constrained [dl_ones [dl_length stimdg:stimtype]]
            dl_set stimdg:constraint_locked [dl_ones [dl_length stimdg:stimtype]]
	}
	
	$s add_method setup_haptic_constrained_unlocked { \
					 subject_id subject_set n_per_set \
					 n_rep rotations } {
            set shape_scale 1
            set noise_type none
            my setup_trials contour $subject_id $subject_set $n_per_set haptic \
		 $shape_scale $noise_type $n_rep $rotations
            dl_set stimdg:constrained [dl_ones [dl_length stimdg:stimtype]]
	}
	
	$s add_method setup_haptic_follow_dial { subject_id subject_set n_per_set \
						     n_rep rotations } {
            set shape_scale 1
            set noise_type none
            my setup_trials contour $subject_id $subject_set $n_per_set haptic \
		 $shape_scale $noise_type $n_rep $rotations
            dl_set stimdg:follow_dial [dl_ones [dl_length stimdg:stimtype]]
            dl_set stimdg:constrained [dl_ones [dl_length stimdg:stimtype]]
        }
	
	$s add_method setup_haptic_follow_pattern { subject_id subject_set n_per_set \
							n_rep rotations } {
            set shape_scale 1
            set noise_type none
            my setup_trials contour $subject_id $subject_set $n_per_set haptic \
		 $shape_scale $noise_type $n_rep $rotations
	    dl_set stimdg:follow_pattern [dl_replicate [dl_slist sine] [dl_length stimdg:stimtype]]
            dl_set stimdg:constrained [dl_ones [dl_length stimdg:stimtype]]
	}
	
	$s add_method setup_trials { db_prefix subject_id subject_set n_per_set \
		 trial_type shape_scale noise_type n_rep rotations } {
	     # find database
	     set db {}
	     set p ${::ess::system_path}/$::ess::current(project)/hapticvis/db
	     variable shapedb_file [file join $p shape_db]

	     if { $db_prefix == "identity" } {
		 # the number of sets depends on set size (4->8, 8->2, 6->3)
		 set n_sets [dict get {4 4 8 2 6 3} $n_per_set]
		 variable trialdb_file \
		     [file join $p trial_db_${n_per_set}_${n_sets}]
	     } else {
		 # the number of sets depends on set size (4->4, 8->2, 6->3)
		 set n_sets [dict get {4 8 8 2 6 3} $n_per_set]
		 variable trialdb_file \
		     [file join $p contour_db_${n_per_set}_${n_sets}]
	     }

	     
	     # build our stimdg
	     if { [dg_exists stimdg] } { dg_delete stimdg }
	     set g [dg_create stimdg]
	     dg_rename $g stimdg
	     
	     # shape coords are in shapedb_file
	     if {![file exists $shapedb_file]} { error "db file not found" }
	     if { [dg_exists shape_db] } { dg_delete shape_db }
	     dg_rename [dg_read $shapedb_file] shape_db
	     
	     # trial info in trialdb_file
	     # trial_db contains columns: subject target_ids dist_ids
	     if { [dg_exists trial_db] } { dg_delete trial_db }
	     dg_rename [dg_read $trialdb_file] trial_db
	     
	     set row [dl_find trial_db:subject $subject_id]
	     if { $row < 0 } { error "subject not in database \"$trialdb_file\"" }
	     set targets trial_db:target_ids:$row:$subject_set

	     if { $db_prefix == "identity" } {
		 set dists   trial_db:dist_ids:$row:$subject_set
		 
		 if { ![dl_exists $targets] || ![dl_exists $dists] } {
		     error "subject set does not exist"
		 }
	     } else {
		 if { ![dl_exists $targets] } {
		     error "subject set does not exist"
		 }
	     }
	     
	     dl_set stimdg:stimtype         [dl_ilist]
	     
	     dl_set stimdg:trial_type       [dl_slist]
	     dl_set stimdg:subject_id       [dl_ilist]
	     dl_set stimdg:subject_set      [dl_ilist]
	     dl_set stimdg:shape_set        [dl_ilist]
	     dl_set stimdg:shape_set_size   [dl_ilist]
	     dl_set stimdg:shape_id         [dl_ilist]
	     dl_set stimdg:shape_coord_x    [dl_ilist]
	     dl_set stimdg:shape_coord_y    [dl_ilist]
	     dl_set stimdg:shape_center_x   [dl_flist]
	     dl_set stimdg:shape_center_y   [dl_flist]
	     dl_set stimdg:shape_rot_deg_cw [dl_flist]
	     dl_set stimdg:shape_scale      [dl_flist]
	     dl_set stimdg:shape_filled     [dl_ilist]
	     dl_set stimdg:noise_elements   [dl_llist]
	     dl_set stimdg:correct_choice   [dl_ilist]
	     dl_set stimdg:correct_location [dl_slist]
	     dl_set stimdg:n_choices        [dl_ilist]
	     dl_set stimdg:choice_centers   [dl_llist]
	     dl_set stimdg:choice_scale     [dl_flist]
	     dl_set stimdg:is_cued          [dl_ilist]
	     dl_set stimdg:cued_choices     [dl_llist]
	     dl_set stimdg:feedback_type    [dl_slist]
	     dl_set stimdg:follow_dial      [dl_ilist]
	     dl_set stimdg:follow_pattern   [dl_slist]
	     dl_set stimdg:constrained      [dl_ilist]
	     dl_set stimdg:constraint_locked [dl_ilist]
	     
	     
	     # go into table and find info about sets/subject
	     set shape_ids [dl_tcllist $targets]
	     
	     # get coords for each shape
	     dl_local shape_inds [haptic::get_shape_indices shape_db:id $shape_ids]
	     dl_local coord_x [dl_choose shape_db:x $shape_inds]
	     dl_local coord_y [dl_choose shape_db:y $shape_inds]
	     
	     # close the shape_db and trial_db
	     dg_delete shape_db
	     dg_delete trial_db
	     
	     # total number of trials
	     set n_rotations [llength $rotations]
	     set n_shapes [llength $shape_ids]
	     set n_obs [expr {$n_rep * $n_rotations * $n_shapes}]
	     
	     set is_cued      0
	     set shape_filled 1
	     
	     set shape_reps [expr {$n_rep*$n_rotations}]
	     dl_local shape_id [dl_repeat [dl_ilist {*}$shape_ids] $shape_reps]
	     
	     set choice_ecc 5
	     set choice_scale 1.5
	     set n_choices $n_shapes
	     
	     if { $n_choices == 4 } {
		 dl_local slots [dl_ilist 1 3 5 7]
		 dl_local choice_locs [dl_slist UR UL DL DR]
	     } elseif { $n_choices == 6 } {
		 dl_local slots [dl_ilist 1 2 3 5 6 7]
		 dl_local choice_locs [dl_slist UR U UL DL D DR]
	     } else {
		 dl_local slots [dl_ilist 0 1 2 3 4 5 6 7]
		 dl_local choice_locs [dl_slist R UR U UL L DL D DR]
	     }
	     
	     dl_local choice_angles \
		 [dl_mult [expr (2*$::pi)/8.] $slots]
	     
	     dl_local choice_center_x \
		 [dl_mult [dl_cos $choice_angles] $choice_ecc]
	     dl_local choice_center_y \
		 [dl_mult [dl_sin $choice_angles] $choice_ecc]
	     dl_local choice_centers \
		 [dl_llist \
		      [dl_transpose \
			   [dl_llist $choice_center_x $choice_center_y]]]
	     
	     if { $noise_type == "none"} {
		 dl_local noise_elements \
		     [dl_replicate [dl_llist [dl_llist]] $n_obs]
	     } elseif { $noise_type == "circles"} {
		 set nelements 50
		 set nprop 0.18; # proportion of scale for radii
		 set njprop 0.2; # jitter proportion
		 set total_elements [expr {${n_obs}*$nelements}]
		 set hscale [expr {${shape_scale}/2.0}]
		 dl_local xs [dl_sub \
				  [dl_mult [dl_urand $total_elements] \
				       $shape_scale] $hscale]
		 dl_local ys [dl_sub \
				  [dl_mult [dl_urand $total_elements] \
				       $shape_scale] $hscale]
		 set rscale [expr {${shape_scale}*${nprop}}]
		 dl_local rjitter [dl_sub \
				       [dl_mult [dl_urand $total_elements] \
					    [expr $rscale*${njprop}]] \
				       [expr $rscale*.1]]
		 dl_local rs [dl_add $rjitter $rscale]
		 dl_local noise_elements \
		     [dl_reshape [dl_transpose \
				      [dl_llist $xs $ys $rs]] $n_obs $nelements]
	     }
	     
	     dl_set stimdg:stimtype     [dl_fromto 0 $n_obs]
	     dl_set stimdg:trial_type   [dl_repeat [dl_slist $trial_type] \
					     $n_obs]
	     dl_set stimdg:subject_id   [dl_repeat $subject_id $n_obs]
	     dl_set stimdg:subject_set  [dl_repeat $subject_set $n_obs]
	     
	     dl_set stimdg:shape_set      [dl_repeat [dl_ilist -1] $n_obs]
	     dl_set stimdg:shape_set_size [dl_repeat $n_shapes $n_obs]
	     dl_set stimdg:shape_id       $shape_id
	     dl_set stimdg:shape_coord_x  [dl_repeat $coord_x $shape_reps]
	     dl_set stimdg:shape_coord_y  [dl_repeat $coord_y $shape_reps]
	     dl_set stimdg:shape_center_x [dl_zeros $n_obs.]
	     dl_set stimdg:shape_center_y [dl_zeros $n_obs.]
	     dl_set stimdg:shape_rot_deg_cw \
		 [dl_replicate [dl_flist {*}$rotations] [expr $n_rep*$n_shapes]]
	     dl_set stimdg:shape_scale    [dl_repeat $shape_scale $n_obs]
	     dl_set stimdg:shape_filled   [dl_repeat $shape_filled $n_obs]
	     dl_set stimdg:noise_elements $noise_elements
	     dl_set stimdg:correct_choice \
		 [dl_repeat [dl_add 1 [dl_fromto 0 $n_choices]] $shape_reps]
	     dl_set stimdg:correct_location \
		 [dl_repeat $choice_locs $shape_reps]
	     dl_set stimdg:n_choices      [dl_repeat $n_choices $n_obs]
	     dl_set stimdg:choice_centers [dl_repeat $choice_centers $n_obs]
	     dl_set stimdg:choice_scale   [dl_repeat $choice_scale $n_obs]
	     dl_set stimdg:is_cued        [dl_repeat $is_cued $n_obs]
	     dl_set stimdg:cued_choices   [dl_repeat [dl_llist [dl_llist]] $n_obs]
	     dl_set stimdg:feedback_type  [dl_repeat [dl_slist color] $n_obs]

	     dl_set stimdg:follow_dial    [dl_zeros $n_obs]
	     dl_set stimdg:follow_pattern [dl_replicate [dl_slist 0] $n_obs]
	     dl_set stimdg:constrained    [dl_zeros $n_obs]
	     dl_set stimdg:constraint_locked [dl_zeros $n_obs]
	     
	     dl_set stimdg:remaining      [dl_ones $n_obs]
	     
	     return $g
	 }
    }
}

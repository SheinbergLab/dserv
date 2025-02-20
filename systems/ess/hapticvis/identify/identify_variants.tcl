#
# VARIANTS
#   hapticvis identify
#
# DESCRIPTION
#   variant dictionary
#

package require haptic

namespace eval hapticvis::identify {

    # state system parameters
    variable params_defaults { delay_time 100 }

    # variant description

    set subject_ids [dl_tcllist [dl_fromto 0 30]]
    set subject_sets [dl_tcllist [dl_fromto 0 4]]

    # could be options...
    variable n_per_set 4
    variable n_sets    4
    
    variable variants {
        visual {
            description "learn visual objects"      
            loader_proc setup_trials
            loader_options {
		subject_id { $subject_ids }
		subject_set { $subject_sets }
		trial_type visual
		shape_scale { 3 4 5 6 }
		n_rep { 2 4 6 8 10 }
		rotations {
		    {single {0}} {three {-120 0 120}} {four {0 90 180 270}}
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
			 
        $s add_method setup_trials { subject_id subject_set trial_type shape_scale n_rep rotations } {
	    # find database
	    set db {}
	    set p ${::ess::system_path}/$::ess::current(project)/hapticvis/db
	    variable shapedb_file [file join $p shape_db]
	    variable trialdb_file [file join $p trial_db]


            # build our stimdg
            if { [dg_exists stimdg] } { dg_delete stimdg }
            set g [dg_create stimdg]
            dg_rename $g stimdg

	    # shape coords are in shapedb_file
            if {![file exists $shapedb_file]} { error "db file not found" }
	    if { [dg_exists shape_db] } { dg_delete shape_db }
	    dg_rename [dg_read $shapedb_file] shape_db

	    # trial info in trialdb_file
	    if { [dg_exists trialdb] } { dg_delete trialdb }
	    dg_rename [dg_read $trialdb_file] trialdb

	    set targets trialdb:target_ids:$row:$subject_set
	    set dists   trialdb:dist_ids:$row:$subject_set

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
            dl_set stimdg:correct_choice   [dl_ilist]
            dl_set stimdg:n_choices        [dl_ilist]
            dl_set stimdg:choice_centers   [dl_llist]
            dl_set stimdg:choice_scale     [dl_flist]
            dl_set stimdg:is_cued          [dl_ilist]
            dl_set stimdg:cued_choices     [dl_llist]
            dl_set stimdg:feedback_type    [dl_slist]

            # go into table and find info about sets/subject
	    set shape_ids [dl_tcllist $targets]

            # get coords for each shape
	    dl_local shape_inds [haptic::get_shape_indices shape_db:id $shape_ids]
	    dl_local coord_x [dl_choose shape_db:x $shape_inds]
	    dl_local coord_y [dl_choose shape_db:y $shape_inds]
	    
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
            dl_local choice_angles \
            [dl_mult [expr (2*$::pi)/$n_choices] [dl_fromto 0 $n_choices]]

            dl_local choice_center_x \
              [dl_mult [dl_cos $choice_angles] $choice_ecc]
            dl_local choice_center_y \
              [dl_mult [dl_sin $choice_angles] $choice_ecc]
            dl_local choice_centers \
              [dl_llist \
                [dl_transpose [dl_llist $choice_center_x $choice_center_y]]]

            dl_set stimdg:stimtype     [dl_fromto 0 $n_obs]
            dl_set stimdg:trial_type   [dl_repeat [dl_slist $trial_type] $n_obs]
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
            dl_set stimdg:correct_choice \
              [dl_repeat [dl_series 1 $n_shapes] $shape_reps]
            dl_set stimdg:n_choices      [dl_repeat $n_choices $n_obs]
            dl_set stimdg:choice_centers [dl_repeat $choice_centers $n_obs]
            dl_set stimdg:choice_scale   [dl_repeat $choice_scale $n_obs]
            dl_set stimdg:is_cued        [dl_repeat $is_cued $n_obs]
            dl_set stimdg:cued_choices   [dl_repeat [dl_llist] $n_obs]
            dl_set stimdg:feedback_type  [dl_repeat [dl_slist color] $n_obs]
            dl_set $g:remaining [dl_ones $n_obs]

            # close the db
	    dg_delete shape_db

            return $g
        }
    }
}

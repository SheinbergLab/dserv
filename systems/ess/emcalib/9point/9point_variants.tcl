#
# VARIANTS
#   emcalib 9point
#
# DESCRIPTION
#   variants/loaders for 9point version of emcalib
#

namespace eval emcalib::9point {
    variable params_defaults         {}

    variable variants {
	spots {
	    description "standard 9 point"
	    loader_proc basic_calib
	    loader_options {
		nr { 2 4 }
	    }
	}
    }	
    
    proc variants_init { s } {
	$s add_method basic_calib { nr } {
	    set n_rep $nr
	    
	    if { [dg_exists stimdg] } { dg_delete stimdg }
	    set g [dg_create stimdg]
	    dg_rename $g stimdg

	    set targ_radius 0.3
	    set jump_scale 5
	    
	    dl_local xlocs \
		[dl_mult $jump_scale [dl_replicate [dl_ilist -1 0 1] 3]]
	    dl_local ylocs \
		[dl_mult $jump_scale [dl_repeat    [dl_ilist -1 0 1] 3]]
	    set npos [dl_length $xlocs]
	    set n_obs [expr $npos*$n_rep]
	    
	    dl_set $g:stimtype [dl_fromto 0 $n_obs]
	    dl_set $g:position_id [dl_replicate [dl_fromto 0 $npos] $n_rep]

	    dl_set $g:fix_targ_x [dl_repeat 0.0 $n_obs]
	    dl_set $g:fix_targ_y [dl_repeat 0.0 $n_obs]
	    dl_set $g:fix_targ_r [dl_repeat $targ_radius $n_obs]

	    dl_set $g:jump_targ_x [dl_replicate $xlocs $n_rep]
	    dl_set $g:jump_targ_y [dl_replicate $ylocs $n_rep]
	    dl_set $g:jump_targ_r [dl_repeat $targ_radius $n_obs]
	    
	    dl_set $g:remaining [dl_ones $n_obs]
	    
	    return $g
	}
    }
}


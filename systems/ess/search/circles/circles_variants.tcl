#
# VARIANTS
#   search circles
#
# DESCRIPTION
#   variant dictionary
#

namespace eval search::circles {
    variable params_defaults          { n_rep 50 }

    variable variants {
	single {
	    description "no distractors"
	    loader_proc basic_search
	    loader_options {
		nr { 200 100 50 }
		nd { 0 }
		targ_r { 1.5 2.0 }
		dist_prop { 1 }
		mindist { 1.5 }
		targ_range { 8 9 10 }
		targ_color {
		    { cyan { 0 1 1 } }
		    { red { 1 0 0 } }
		}
	    }
	}
	variable {
	    description "variable number of distractors"
	    loader_proc basic_search
	    loader_options {
		nr { 40 60 100 }
		nd {
		    { 0,2,4,6,8 { [dl_tcllist [dl_series 0 8 2]] } }
		    { 0,5,10    { 0 5 10 } }
		}
		targ_r { 1.5 2.0 }
		dist_prop { 1.2 1.1 0.9 0.8 }
		mindist {1.5 2.0}
		targ_range { 8 9 10 }
		targ_color {
		    { cyan { 0 1 1 } }
		    { red { 1 0 0 } }
		}
	    }
	}
	distractors {
	    description "fixed number of distractors"
	    loader_proc basic_search
	    loader_options {
		nr { 100 200 }
		nd { 4 6 8 }
		targ_r { 1.5 2.0 }
		dist_prop { 1.2 1.1 0.9 0.8 }
		mindist { 2.0 3.0 }
		targ_range { 8 9 10 }
		targ_color {
		    { cyan { 0 1 1 } }
		    { red { 1 0 0 } }
		}
	    }
	}
    }
    # use subst to replace variables in variant definition above
    set variants [subst $variants]
    
    proc variants_init { s } {
	
	$s add_method single_init {} {
	    rmtSend "setBackground 10 10 10"
	}
	$s add_method single_deinit {} {}
	
	$s add_method distractors_init {} {
	    rmtSend "setBackground 100 100 100"
	}
	
	$s add_method basic_search { nr nd targ_r dist_prop mindist targ_range targ_color } {
	    set n_rep $nr
	    set ndists $nd
	    
	    if { [dg_exists stimdg] } { dg_delete stimdg }
	    set g [dg_create stimdg]
	    dg_rename $g stimdg
	    
	    set n_obs [expr [llength $ndists]*$n_rep]
	    
	    set scale $targ_r
	    set maxx [expr $screen_halfx]
	    set maxy [expr $screen_halfy]
	    
	    dl_set $g:stimtype [dl_fromto 0 $n_obs]
	    dl_set $g:targ_x \
		[dl_mult 2 [dl_sub [dl_urand $n_obs] 0.5] $targ_range]
	    dl_set $g:targ_y \
		[dl_mult 2 [dl_sub [dl_urand $n_obs] 0.5] $targ_range]
	    dl_set $g:targ_r \
		[dl_repeat $targ_r $n_obs]
	    dl_set $g:targ_color \
		[dl_repeat [dl_slist $targ_color] $n_obs]
	    dl_set $g:targ_pos \
		[dl_reshape [dl_interleave $g:targ_x $g:targ_y] - 2]
	    
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
	    dl_set $g:dist_xs \
		[dl_unpack [dl_choose $g:dists_pos [dl_llist [dl_llist 0]]]]
	    dl_set $g:dist_ys \
		[dl_unpack [dl_choose $g:dists_pos [dl_llist [dl_llist 1]]]]
	    
	    set dist_r [expr $scale*$dist_prop]
	    set dist_color $targ_color
	    
	    dl_set $g:dist_rs [dl_repeat $dist_r [dl_llength $g:dist_xs]]
	    dl_set $g:dist_colors \
		[dl_repeat [dl_slist $dist_color] [dl_llength $g:dist_xs]]
	    
	    dl_set $g:remaining [dl_ones $n_obs]
	    
	    return $g
	}
    }
}


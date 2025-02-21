#
# VARIANTS
#   planko training
#
# DESCRIPTION
#   variant dictionary
#

namespace eval planko::training {
    package require planko
    
    variable params_defaults          { n_rep 50 }

    variable variants {
	single {
	    description "one plank"
	    loader_proc basic_planko
	    loader_options {
		nr { 100 200 }
		nplanks { 1 }
		wrong_catcher_alpha 1
		params { { defaults {} } }
	    }
	}
	jitter {
	    description "jitter ball start"
	    loader_proc basic_planko
	    loader_options {
		nr { 50 100 200 }
		nplanks { 1 }
		wrong_catcher_alpha { 1 0.5 }
		params { { jittered { ball_jitter_x 8 ball_start_y 5 ball_jitter_y 1 } } }
	    }
	}
	zero_one {
	    description "hit zero or one plank"
	    loader_proc basic_planko
	    loader_options {
		nr { 50 100 200 400 800}
		nplanks { 1 }
		wrong_catcher_alpha { 1.0 0.98 0.95 0.9 0.8 0.7 }
		params { { jittered { ball_jitter_x 10 ball_start_y 0 ball_jitter_y 3 minplanks 0 } } }
	    }
	}
 	two_plus {
	    description "show 2+ planks, hit 1+ plank"
	    loader_proc basic_planko
	    loader_options {
		nr { 50 100 200 400 800}
		nplanks { 2 3 4}
		wrong_catcher_alpha { 1.0 0.98 0.95 0.9 0.8 0.7 }
		params { { jittered { ball_jitter_x 10 ball_start_y 0 ball_jitter_y 3 minplanks 1 } } 
  		 	 { higher { ball_jitter_x 10 ball_start_y 5 ball_jitter_y 3 minplanks 1 } } 
    		         { two_plank { ball_jitter_x 10 ball_start_y 5 ball_jitter_y 3 minplanks 2 } } }
	    }
	}
    }	
    
    proc variants_init { s } {
	$s add_method single_init {} {
	    rmtSend "setBackground 10 10 10"
	}

	$s add_method single_deinit {} {}

	$s add_method basic_planko { nr nplanks wrong_catcher_alpha params } {
	    set n_rep $nr
	    
	    if { [dg_exists stimdg] } { dg_delete stimdg }

	    
	    set n_obs [expr [llength $nplanks]*$n_rep]
	    
	    set maxx [expr $screen_halfx]
	    set maxy [expr $screen_halfy]

	    # this is a set of params to pass into generate_worlds
	    set p "nplanks $nplanks $params"
	    set g [planko::generate_worlds $n_obs $p]
	    dl_set $g:wrong_catcher_alpha \
		[dl_repeat [dl_flist $wrong_catcher_alpha] $n_obs]

	    # rename id column to stimtytpe
	    dg_rename $g:id stimtype 
	    dl_set $g:remaining [dl_ones $n_obs]
	    
	    dg_rename $g stimdg
	    return $g
	}
    }
}


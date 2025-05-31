#
# VARIANTS
#   planko training
#
# DESCRIPTION
#   loader functions for planko variants
#

namespace eval planko::training {
    package require planko
    
    proc loaders_init { s } {
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


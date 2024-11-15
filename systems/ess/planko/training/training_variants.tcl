#
# VARIANTS
#   planko training
#
# DESCRIPTION
#   variant dictionary
#

namespace eval planko::training {
    variable basic_planko_defaults    { nr 100  nd 0 mindist 1.5 }
    variable params_defaults          { n_rep 50 }

    variable basic_planko_single      { nr 200 nplanks 1 }
    
    variable variants {
	single      { basic_planko single    "one plank"}
    }	

    proc variants_init { s } {
	
	$s add_method single_init {} {
	    rmtSend "setBackground 10 10 10"
	}
	$s add_method single_deinit {} {}
	
	$s add_method basic_planko { nr nplanks } {
	    set n_rep $nr
	    
	    if { [dg_exists stimdg] } { dg_delete stimdg }
	    set g [dg_create stimdg]
	    dg_rename $g stimdg
	    
	    set n_obs [expr [llength $nplanks]*$n_rep]
	    
	    set maxx [expr $screen_halfx]
	    set maxy [expr $screen_halfy]
	    
	    dl_set $g:stimtype [dl_fromto 0 $n_obs]
	    dl_set $g:nplanks [dl_repeat [dl_ilist $nplanks] $nr]
	    dl_set $g:remaining [dl_ones $n_obs]
	    
	    return $g
	}
    }
}


#
# PROTOCOL
#   search::circles
#
# DESCRIPTION
#   Present a target circle and zero or more distractor circles
#

set circles [ess::create_protocol search circles]

$circles add_method basic_search { args } {
    
    set vars { n_rep targ_radius targ_range targ_color
	dist_prop ndists screen_halfx screen_halfy
    }
    
    foreach v $vars {
	set $v [[my system] get_variable $v]
    }
    
    # dictionaries are passed as args that can override variable settings
    foreach d $args {
	# now set the variables passed in for this variant
	dict for { var val } $d {
	    set $var $val
	}
    }    
    
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
    #	    set mindist [expr sqrt($scale * $scale * 2)]
    
    set mindist 1.5
    
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

ess::create_variant search circles single      basic_search { n_rep 10 ndists 0 }
ess::create_variant search circles variable    basic_search { n_rep 20 ndists {0 2 4 8} }
ess::create_variant search circles distractors basic_search { n_rep 100 ndists 6 }

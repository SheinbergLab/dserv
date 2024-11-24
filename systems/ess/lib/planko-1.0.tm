# -*- mode: tcl -*-

#
# planko.tcl
#   Package for generating planko boards giving set of parameters and constraints
#

package require box2d
package require dlsh

namespace eval planko {
    variable params
    
    proc default_params {} {
	variable params
	set params(xrange)          12.0; # x range for selecting plank locations
	set params(yrange)          12.0; # y range for selecting plank locations
	set params(lcatcher_x)        -3; # x location of left catcher
	set params(lcatcher_y)      -7.5; # y location of left catcher
	set params(rcatcher_x)         3; # x location of right catcher
	set params(rcatcher_y)      -7.5; # y location of right catcher
	set params(ball_start_x)       0; # x location of ball start
	set params(ball_start_y)     8.0; # y location of ball start
	set params(ball_jitter_x)      0; # x jitter for ball start
	set params(ball_jitter_y)      0; # y jitter for ball start
	set params(ball_radius)      0.5; # radius of ball
	set params(nplanks)           10; # number of planks in world
	set params(minplanks)          1; # mininum number of planks hit
	set params(ball_restitution) 0.2; # restitution of the ball
	set params(step_size) \
	    [expr 59.9/1000.0];	          # step size of simulation (sec)
	return
    }
    
    proc create_ball_dg {} {
	variable params
	set b2_staticBody 0
	
	set g [dg_create]

	dl_set $g:id [dl_ilist]
	dl_set $g:name [dl_slist ball]
	dl_set $g:shape [dl_slist Circle]
	dl_set $g:type $b2_staticBody
	dl_set $g:tx [dl_flist $params(ball_xpos)]
	dl_set $g:ty [dl_flist $params(ball_ypos)]
	dl_set $g:sx [dl_flist $params(ball_radius)]
	dl_set $g:sy [dl_flist $params(ball_radius)]
	dl_set $g:angle [dl_flist 0.0]
	dl_set $g:restitution [dl_flist $params(ball_restitution)]
	
	return $g
    }
    
    proc create_catcher_dg { tx ty name } {
	set b2_staticBody 0

	set y [expr $ty-(0.5+0.5/2)]

	set g [dg_create]

	dl_set $g:id [dl_ilist]
	dl_set $g:name [dl_slist ${name}_b ${name}_r ${name}_l]
	dl_set $g:shape [dl_repeat [dl_slist Box] 3]
	dl_set $g:type [dl_repeat $b2_staticBody 3]
	dl_set $g:tx [dl_flist $tx [expr {$tx+2.5}] [expr {$tx-2.5}]]
	dl_set $g:ty [dl_flist $y $ty $ty]
	dl_set $g:sx [dl_flist 5 0.5 0.5]
	dl_set $g:sy [dl_flist 0.5 2 2]
	dl_set $g:angle [dl_zeros 3.]
	dl_set $g:restitution [dl_zeros 3.]
	
	return $g
    }
    
    proc create_plank_dg {}  {
	variable params
	set b2_staticBody 0
	
	set n $params(nplanks)
	set xrange $params(xrange)
	set xrange_2 [expr {$xrange/2}]
	set yrange $params(yrange)
	set yrange_2 [expr {$yrange/2}]
	
	set g [dg_create]
	
	dl_set $g:id [dl_ilist]
	dl_set $g:name [dl_paste [dl_repeat [dl_slist plank] $n] [dl_fromto 0 $n]]
	dl_set $g:shape [dl_repeat [dl_slist Box] $n]
	dl_set $g:type [dl_repeat $b2_staticBody $n]
	dl_set $g:tx [dl_sub [dl_mult $xrange [dl_urand $n]] $xrange_2]
	dl_set $g:ty [dl_sub [dl_mult $yrange [dl_urand $n]] $yrange_2]
	dl_set $g:sx [dl_repeat 3. $n]
	dl_set $g:sy [dl_repeat .5 $n]
	dl_set $g:angle [dl_mult 2 $::pi [dl_urand $n]]
	dl_set $g:restitution [dl_zeros $n.]
    
	return $g
    }


    proc make_world {} {
	variable params
	set planks [create_plank_dg]

	set params(lcatcher_x) -3.0
	set params(lcatcher_y) -7.5
	set left_catcher \
	    [create_catcher_dg $params(lcatcher_x) $params(lcatcher_y) catchl]
	set params(rcatcher_x) 3.0
	set params(rcatcher_y) -7.5
	set right_catcher \
	    [create_catcher_dg $params(rcatcher_x) $params(rcatcher_y) catchr]

	set ball [create_ball_dg]

	foreach p "$ball $left_catcher $right_catcher" {
	    dg_append $planks $p
	    dg_delete $p
	}
	return $planks
    }

    proc build_world { dg } {
	
	# create the world
	set world [box2d::createWorld]
	
	set n [dl_length $dg:name]
	
	# load in objects
	for { set i 0 } { $i < $n } { incr i } {
	    foreach v "name shape type tx ty sx sy angle restitution" {
		set $v [dl_get $dg:$v $i]
	    }
	    
	    if { $shape == "Box" } {
		set body [box2d::createBox $world $name $type $tx $ty $sx $sy $angle]
	    } elseif { $shape == "Circle" } {
		set body [box2d::createCircle $world $name $type $tx $ty $sx]
	    }
	    box2d::setRestitution $world $body $restitution

	    # will return ball handle
	    if { $name == "ball" } {
		set ball $body
	    }
	}
	
	# create a dynamic circle
	return "$world $ball"
    }
    
    proc test_simulation { world ball { simtime 6 } } {
	set g [dg_create]
	dl_set $g:t [dl_flist]
	dl_set $g:x [dl_flist]
	dl_set $g:y [dl_flist]
	dl_set $g:contact_bodies [dl_slist]
	dl_set $g:contact_t [dl_flist]
	
	variable params
	box2d::setBodyType $world $ball 2    
	set step $params(step_size)
	set nsteps [expr {int(ceil(6.0/$step))}]
	set contacts {}
	for { set t 0 } { $t < $simtime } { set t [expr $t+$step] } {
	    box2d::step $world $step
	    if { [set c [box2d::getContactBeginEventCount $world]] } {
		set events [box2d::getContactBeginEvents $world]
		for { set i 0 } { $i < $c } { incr i } { 
		    dl_append $g:contact_t $t
		    dl_append $g:contact_bodies [lindex $events $i]
		}
	    }
	    lassign [box2d::getBodyInfo $world $ball] tx ty a
	    dl_append $g:t $t
	    dl_append $g:x $tx
	    dl_append $g:y $ty
	}
	return  $g
    }
    
    proc isPlank { pair } { return [string match plank* [lindex $pair 0]] }
    
    proc uniqueList {list} {
	set new {}
	foreach item $list {
	    if {$item ni $new} {
		lappend new $item
	    }
	}
	return $new
    }
    
    proc accept_board { g } {
	variable params
	set x [dl_last $g:x]
	set y [dl_last $g:y]
	set contacts [dl_tcllist $g:contact_bodies]

	# assumes both catchers are at same y value
	set catcher_y $params(lcatcher_y)
	
	set upper [expr $catcher_y+0.01]
	set lower [expr $catcher_y-0.01]
	
	if { [expr {$y < $upper && $y > $lower}] } {
	    set result [expr {$x>0}]
	} else {
	    return "-1 0"
	}
	
	set planks [lmap c $contacts \
			{ expr { [isPlank $c] ? [lindex [lindex $c 0] 0] : [continue] } }]
	set planks [uniqueList $planks]
	set nhit [llength $planks]
	if { $nhit < $params(minplanks) } { return -1 }
	
	return "$result $nhit"
    }
    
    proc generate_world { accept_proc } {
	variable params
	set done 0
	while { !$done } {
	    box2d::destroy all

	    # allow jitter for ball start before making world
	    set params(ball_xpos) [expr { $params(ball_start_x) +
				     rand()*$params(ball_jitter_x) -
				     0.5*$params(ball_jitter_x) } ]
	    set params(ball_ypos) [expr { $params(ball_start_y) +
				     rand()*$params(ball_jitter_y) -
				     0.5*$params(ball_jitter_y) } ]
	    
	    set new_world [make_world]
	    
	    lassign [build_world $new_world] world ball

	    set sim_dg [test_simulation $world $ball]
	    lassign [$accept_proc $sim_dg] result nhit

	    if { $result != -1 } {
		dl_set $new_world:side     [dl_ilist $result]
		dl_set $new_world:nhit     [dl_ilist $nhit]
		dl_set $new_world:nplanks  [dl_ilist $params(nplanks)]
		dl_set $new_world:ball_start_x \
		    [dl_flist $params(ball_xpos)]
		dl_set $new_world:ball_start_y \
		    [dl_flist $params(ball_ypos)]
		dl_set $new_world:ball_radius \
		    [dl_flist $params(ball_radius)]
		dl_set $new_world:ball_restitution \
		    [dl_flist $params(ball_restitution)]
		dl_set $new_world:lcatcher_x [dl_flist $params(lcatcher_x)]
		dl_set $new_world:lcatcher_y [dl_flist $params(lcatcher_y)]
		dl_set $new_world:rcatcher_x [dl_flist $params(rcatcher_x)]
		dl_set $new_world:rcatcher_y [dl_flist $params(rcatcher_y)]
		dl_set $new_world:ball_t $sim_dg:t				 
		dl_set $new_world:ball_x $sim_dg:x
		dl_set $new_world:ball_y $sim_dg:y
		dl_set $new_world:contact_t      $sim_dg:contact_t
		dl_set $new_world:contact_bodies $sim_dg:contact_bodies
		dg_delete $sim_dg
		return $new_world
	    } else {
		dg_delete $sim_dg
		dg_delete $new_world
	    }
	}
    }


    proc pack_world { g } {
	# these are columns that are lists for each world
	set cols "name shape type tx ty sx sy angle restitution ball_t ball_x ball_y contact_t contact_bodies"
	
	# put the lists into a list of lists so we can append worlds together
	foreach c $cols {
	    dl_set $g:$c [dl_llist $g:$c]
	}
	return $g
    }
    

    proc generate_worlds { n d } {
	variable params
	default_params

	dict for { k v } $d { set params($k) $v }
	
	set acc accept_board
	set worlds [pack_world [generate_world $acc]]
	for { set i 1 } { $i < $n } { incr i } {
	    set world [pack_world [generate_world $acc]]
	    if { $world != "" } {
		dg_append $worlds $world
		dg_delete $world
	    } else {
		error "unable to generate desired world type"
	    }
	}
	
	# first column is id - set to unique ids between 0 and n
	dl_set $worlds:id [dl_fromto 0 $n]

	return $worlds
    }
}





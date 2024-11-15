# NAME
#   circles_stim.tcl
#
# DESCRIPTION
#   search with circles
#
# REQUIRES
#   polygon
#   metagroup
#
# AUTHOR
#   DLS
#

# connect to data server receive stimdg updates
package require qpcs
qpcs::dsStimRegister $dservhost
qpcs::dsStimAddMatch $dservhost stimdg

# to stimdg is sent as b64 encoded string, this proc unpacks into stim
proc readdg { args } {
    dg_fromString64 [lindex $args 4]
}

# this sets the callback upon receipt of stimdg
set ::dsCmds(stimdg) readdg

# simulate touchscreen using  mouse from stim2 (don't need Release)
namespace inscope :: {
    proc onMousePress {} {
	global dservhost
	dl_local coords [dl_create short $::MouseXPos $::MouseYPos]
	qpcs::dsSetData $dservhost mtouch/touchvals $coords
    }
}


package require box2d


##############################################################
##                    Create Worlds                        ###
##############################################################

set params(xrange) 16
set params(yrange) 12
set params(catcher_y) -7.5
set params(ball_start) 8.0
set params(minplanks) 1
set params(nplanks) 10

proc create_ball_dg {} {
    global params
    set b2_staticBody 0

    set g [dg_create]

    dl_set $g:name [dl_slist ball]
    dl_set $g:shape [dl_slist Circle]
    dl_set $g:type $b2_staticBody
    dl_set $g:tx [dl_flist 0]
    dl_set $g:ty [dl_flist $params(ball_start)]
    dl_set $g:sx [dl_flist 0.5]
    dl_set $g:sy [dl_flist 0.5]
    dl_set $g:angle [dl_flist 0.0]
    dl_set $g:restitution [dl_flist 0.2]
    
    return $g
}

proc create_catcher_dg { tx name } {
    global params
    set b2_staticBody 0

    set catcher_y $params(catcher_y)
    set y [expr $catcher_y-(0.5+0.5/2)]

    set g [dg_create]

    dl_set $g:name [dl_slist ${name}_b ${name}_r ${name}_l]
    dl_set $g:shape [dl_repeat [dl_slist Box] 3]
    dl_set $g:type [dl_repeat $b2_staticBody 3]
    dl_set $g:tx [dl_flist $tx [expr {$tx+2.5}] [expr {$tx-2.5}]]
    dl_set $g:ty [dl_flist $y $catcher_y $catcher_y]
    dl_set $g:sx [dl_flist 5 0.5 0.5]
    dl_set $g:sy [dl_flist 0.5 2 2]
    dl_set $g:angle [dl_zeros 3.]
    dl_set $g:restitution [dl_zeros 3.]
    
    return $g
}

proc create_plank_dg {}  {
    global params
    set b2_staticBody 0

    set n $params(nplanks)
    set xrange $params(xrange)
    set xrange_2 [expr {$xrange/2}]
    set yrange $params(yrange)
    set yrange_2 [expr {$yrange/2}]

    set g [dg_create]

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
    set planks [create_plank_dg]
    set left_catcher [create_catcher_dg -3 catchl]
    set right_catcher [create_catcher_dg 3 catchr]
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
    box2d::setBodyType $world $ball 2    
    set step [expr {[screen_set FrameDuration]/1000.0}]
    set nsteps [expr {int(ceil(6.0/$step))}]
    set contacts {}
    for { set t 0 } { $t < $simtime } { set t [expr $t+$step] } {
	box2d::step $world $step
        if { [box2d::getContactBeginEventCount $world] } { 
	    lappend contacts [box2d::getContactBeginEvents $world]
        }
    }
    lassign [box2d::getBodyInfo $world $ball] tx ty a
    return  "$tx $ty [list $contacts]"
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

proc accept_board { x y contacts } {
    global params
    set catcher_y $params(catcher_y)

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

proc generate_world { n accept_proc } {
    set done 0
    while { !$done } {
	box2d::destroy all

	set new_world [make_world]
	
	lassign [build_world $new_world] world ball
	lassign [test_simulation $world $ball] x y contacts
	lassign [$accept_proc $x $y $contacts] result nhit
	
	if { $result != -1 } {
	    return $new_world
	} else {
	    dg_delete $new_world
	}
    }
}


##############################################################
##                      Show Worlds                        ###
##############################################################

proc make_stims { n } {
    set dg [generate_world $n accept_board]
    
    resetObjList		 ;# unload existing objects
    glistInit 1			 ;# initialize stimuli

    set bworld [Box2D]
    glistAddObject $bworld 0
 
    set n [dl_length $dg:name]
    for { set i 0 } { $i < $n } { incr i } {
	foreach v "name shape type tx ty sx sy angle restitution" {
	    set $v [dl_get $dg:$v $i]
	} 
	if { $shape == "Box" } {
	    set body [create_box $bworld $name $type $tx $ty $sx $sy $angle { 9. 9. 9. 0.8 }]
	} elseif { $shape == "Circle" } {
	    set body [create_circle $bworld $name $type $tx $ty $sx $angle { 0 1 1 }]
	}
	Box2D_setRestitution $bworld [setObjProp $body body] $restitution
	
	glistAddObject $body 0

	# track this so we can set in motion
	if { $name == "ball" } { set ::ball $body }
    }

    dg_delete $dg
    
    glistSetDynamic 0 1
    return $bworld
}

# create a box2d body and visual box (angle is in degrees)
proc create_box { bworld name type tx ty sx sy { angle 0 } { color { 1 1 1 } } } {
    # create the box2d box
    set body [Box2D_createBox $bworld $name $type $tx $ty $sx $sy $angle]

    # make a polygon to visualize the box
    set box [make_rect]
    scaleObj $box [expr 1.0*$sx] [expr 1.0*$sy]
    translateObj $box $tx $ty
    rotateObj $box $angle 0 0 1
    polycolor $box {*}$color

    # create object matrix for updating
    set m [dl_tcllist [mat4_createTranslationAngle $tx $ty $angle]]
    setObjMatrix $box {*}$m
    
    # link the box2d box to the polygon
    Box2D_linkObj $bworld $body $box
    setObjProp $box body $body
    setObjProp $box bworld $bworld
    
    return $box
}

# create a box2d body and visual circle (angle is in degrees)
proc create_circle { bworld name type tx ty radius { angle 0 } { color { 1 1 1 } } } {
    # create the box2d circle
    set body [Box2D_createCircle $bworld $name $type $tx $ty $radius $angle]

    # make a polygon to visualize the circle
    set circ [make_circle]
    scaleObj $circ [expr 2.0*$radius] [expr 2.0*$radius]
    translateObj $circ $tx $ty
    polycolor $circ {*}$color

    # create object matrix for updating
    set m [dl_tcllist [mat4_createTranslationAngle $tx $ty $angle]]
    setObjMatrix $circ {*}$m
    
    # link the box2d circle to the polygon
    Box2D_linkObj $bworld $body $circ
    setObjProp $circ body $body
    setObjProp $circ bworld $bworld
    
    return $circ
}

# Create a square which can be scaled to create rects
proc make_rect {} {
    set s [polygon]
    return $s
}

# Create a circle
proc make_circle {} {
    set circ [polygon]
    polycirc $circ 1
    return $circ
}

proc nexttrial { id } {
    glistInit 1
    resetObjList

    set ::world [make_stims 2]
}
    
proc stimon {} {
    glistSetCurGroup 0
    glistSetVisible 1
    redraw
}

proc stimoff {} {
    glistSetVisible 0
    redraw
}

proc reset { } {
    glistSetVisible 0; redraw;
}

proc clearscreen { } {
    glistSetVisible 0; redraw;
}







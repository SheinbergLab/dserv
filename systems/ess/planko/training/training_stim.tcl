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

package require box2d

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


##############################################################
##                      Show Worlds                        ###
##############################################################

proc make_stims { trial } {
    
    resetObjList		 ;# unload existing objects
    glistInit 1			 ;# initialize stimuli

    set dg stimdg
    
    set bworld [Box2D]
    glistAddObject $bworld 0
 
    set n [dl_length $dg:name:$trial]
    for { set i 0 } { $i < $n } { incr i } {
	foreach v "name shape type tx ty sx sy angle restitution" {
	    set $v [dl_get $dg:$v:$trial $i]
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
    set ::world [make_stims $id]
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







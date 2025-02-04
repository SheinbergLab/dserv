# NAME
#   hapticvis_stim.tcl
#
# DESCRIPTION
#   match_to_sample with visual and graspomatic generated shapes
#
# REQUIRES
#   polygon
#   metagroup
#
# AUTHOR
#   DLS
#

proc create_shape { id } {
    set scale [dl_get stimdg:shape_scale $id]
    set rotation [dl_get stimdg:shape_rot_deg_cw $id]
    set s [polygon]

    polyverts $s stimdg:shape_coord_x:$id stimdg:shape_coord_y:$id
    polytype $s lines
    polycolor $s 1 1 1
    scaleObj $s $scale

    # verify if z should be 1 or -1...
    rotateObj $s $rotation 0 0 1
    
    return $s
}

proc create_circle { r g b { a 1 } } {
    set c [polygon]
    polycirc $c 1
    polycolor $c $r $g $b $a
    return $c
}

proc nexttrial { id } {
    glistInit 2
    resetObjList

    set trialtype [dl_get stimdg:trial_type $id]
    set scale [dl_get stimdg:choice_scale $id]
    set nchoices [dl_get stimdg:n_choices $id]
    # add the visual sample for VV trials, no visual sample for HV trials
    if { $trialtype == "visual" } {
	set sample [create_shape $id]
	glistAddObject $sample 0
    } else {

    }
    
    # choice circles
    set mg [metagroup]
    for { set i 0 } { $i < $nchoices } { incr i } {
	set s [create_circle 1 1 1 0.3]
	translateObj $s {*}[dl_tcllist stimdg:choice_centers:$id:$i]
	scaleObj $s [dl_get stimdg:choice_scale $id]
	metagroupAdd $mg $s
    }
    glistAddObject $mg 0
}
    
proc sample_on {} {
    glistSetCurGroup 0
    glistSetVisible 1
    redraw
}

proc sample_off {} {
    glistSetVisible 0
    redraw
}

proc choices_on {} {
    glistSetCurGroup 1
    glistSetVisible 1
    redraw
}

proc choices_off {} {
    glistSetVisible 0
    redraw
}

proc reset { } {
    glistSetVisible 0; redraw;
}

proc clearscreen { } {
    glistSetVisible 0; redraw;
}



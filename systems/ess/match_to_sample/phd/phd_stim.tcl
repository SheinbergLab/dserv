# NAME
#   phd_stim.tcl
#
# DESCRIPTION
#   match_to_sample with visual and phd generated shapes
#
# REQUIRES
#   polygon
#   metagroup
#
# AUTHOR
#   DLS
#

package require hex
package require sqlite3

#
# nexttrial
#    create a stimulus based on the hex "qrs" notation which contains triples
# speficying the location of "pins" to show
#
proc create_shape { qrs scale { pinsize 12 } } {
    set scale 0.2
    dl_local centers [dl_transpose [dl_unpack [::hex::pointy_to_pixel $qrs $scale]]]

    set s [polygon]
    dl_set centers $centers
    polyverts $s $centers:0 $centers:1
    polytype $s points
    polypointsize $s $pinsize
    polycolor $s 0 0 0

    return $s
}

proc nexttrial { id } {
    glistInit 2
    resetObjList

    set trialtype [dl_get stimdg:trial_type $id]
    set scale [dl_get stimdg:choice_scale $id]
    set scale 1.0
    set choice_centers [dl_get stimdg:choice_centers $id]
    
    # add the visual sample for VV trials, no visual sample for HV trials
    if { $trialtype == "VV" } {
	set sample [create_shape stimdg:sample_qrs:$id $scale]
	glistAddObject $sample 0
    } else {
	set object_id [dl_get stimdg:sample_id $id]
#	set phd_command [list print haptic_on $object_id]
#	glistSetInitCmd $phd_command 0
    }
    
    # match choices
    set mg [metagroup]
    foreach i "0 1 2 3" {
	set s [create_shape stimdg:choice_qrs:$id:$i $scale]
	translateObj $s {*}[dl_tcllist stimdg:choice_centers:$id:$i]
	#scaleObj $s [dl_get stimdg:choice_scale $id]
	metagroupAdd $mg $s
    }
    glistAddObject $mg 1
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



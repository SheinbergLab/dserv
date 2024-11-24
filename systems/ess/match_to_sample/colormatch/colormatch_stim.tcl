# NAME
#   colormatch_stim.tcl
#
# DESCRIPTION
#   match_to_sample with colored squares
#
# REQUIRES
#   polygon
#   metagroup
#
# AUTHOR
#   DLS
#

#
# nexttrial
#    create a triad of stimuli with sample on top and choices below
#
#
proc nexttrial { id } {
    glistInit 2
    resetObjList

    # grab relevant variables from stimdg
    foreach t "sample match nonmatch" {
	foreach p "${t}_x ${t}_y ${t}_r ${t}_color" {
	    set $p [dl_get stimdg:$p $id]
	}
    }

    # add the sample
    foreach t sample {
	set obj [polygon]
	polycolor $obj {*}[dl_tcllist [set ${t}_color]]
	translateObj $obj [set ${t}_x] [set ${t}_y]
	scaleObj $obj [expr 2*[set ${t}_r]]; # diameter is 2*r
	glistAddObject $obj 0
    }

    # add the choices
    foreach t "match nonmatch" {
	set obj [polygon]
	polycolor $obj {*}[dl_tcllist [set ${t}_color]]
	translateObj $obj [set ${t}_x] [set ${t}_y]
	scaleObj $obj [expr 2*[set ${t}_r]]; # diameter is 2*r
	glistAddObject $obj 1
    }
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




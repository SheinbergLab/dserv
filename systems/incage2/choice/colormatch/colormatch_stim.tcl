# NAME
#   search_stim.tcl
#
# DESCRIPTION
#   search for target among distractors
#
# REQUIRES
#   polygon
#   metagroup
#
# AUTHOR
#   DLS
#

if { $tcl_platform(os) != "QNX" } {
    # connect to data server on QNX host to receive stimdg updates
    package require qpcs
    qpcs::dsStimRegister $qnxhost
    qpcs::dsStimAddMatch $qnxhost stimdg

    # to stimdg is sent as b64 encoded string, this proc unpacks into stim
    proc readdg { args } {
	   dg_fromString64 [lindex $args 4]
    }

    # this sets the callback upon receipt of stimdg
    set ::dsCmds(stimdg) readdg

    # simulate touchscreen using  mouse from stim2 (don't need Release)
    namespace inscope :: {
	proc onMousePress {} {
	    global qnxhost
	    qpcs::dsSet $qnxhost mtouch/touch "0 0 $::MouseXPos $::MouseYPos"
	}
    }
}

#
# nexttrial
#    create a triad of stimuli with sample on top and choices below
#
#
proc nexttrial { id } {
    glistInit 1
    resetObjList

    # grab relevant variables from stimdg
    foreach t "sample match nonmatch" {
	foreach p "${t}_x ${t}_y ${t}_r ${t}_color" {
	    set $p [dl_get stimdg:$p $id]
	}
    }

    # add the individual stimuli
    foreach t "sample match nonmatch" {
	set obj [polygon]
	polycolor $obj {*}[dl_tcllist [set ${t}_color]]
	translateObj $obj [set ${t}_x] [set ${t}_y]
	scaleObj $obj [expr 2*[set ${t}_r]]; # diameter is 2*r
	glistAddObject $obj 0
    }
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




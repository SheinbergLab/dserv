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
	qpcs::dsSet $dservhost mtouch/touch "0 0 $::MouseXPos $::MouseYPos"
    }
}

proc nexttrial { id } {
    glistInit 1
    resetObjList
    
    foreach p "targ_x targ_y targ_r targ_color" {
	set $p [dl_get stimdg:$p $id]
    }
    
    # load the target
    set obj [polygon]
    polycirc $obj 1
    polycolor $obj {*}$targ_color
    translateObj $obj $targ_x $targ_y
    scaleObj $obj [expr 2*$targ_r]; # diameter is 2r
    glistAddObject $obj 0

    # now the distractors
    set ndists [dl_get stimdg:dists_n $id]
    if { $ndists > 0 } {
	set dists [metagroup]
	for { set i 0 } { $i < $ndists } { incr i } {
	    set dist_x [dl_get stimdg:dist_xs:$id $i]
	    set dist_y [dl_get stimdg:dist_ys:$id $i]
	    set dist_r [dl_get stimdg:dist_rs:$id $i]
	    set dist_color [dl_get stimdg:dist_colors:$id $i]
	    
	    set obj [polygon]
	    polycirc $obj 1
	    polycolor $obj {*}$dist_color
	    translateObj $obj $dist_x $dist_y
	    scaleObj $obj [expr 2*$dist_r]; # diameter is 2r
	    metagroupAdd $dists $obj
	}
	glistAddObject $dists 0
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







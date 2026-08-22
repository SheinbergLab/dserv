# NAME
#   blink_stim.tcl
#
# DESCRIPTION
#   Stim2 code for blinky: one circle, shown and hidden.
#
# REQUIRES
#   polygon
#   metagroup
#
# SUPPORTED RMT CALLS
#   nexttrial <id>
#   blink_on
#   blink_off
#   reset
#

proc nexttrial { id } {
    glistInit 1
    resetObjList

    foreach p "blink_x blink_y blink_r" { set $p [dl_get stimdg:$p $id] }
    # rgb comes from stimdg; the loader owns the name -> rgb table
    set color [list [dl_get stimdg:blink_r $id] \
                    [dl_get stimdg:blink_g $id] \
                    [dl_get stimdg:blink_b $id]]

    set mg [metagroup]
    set obj [polygon]
    polycirc $obj 1
    polycolor $obj {*}$color
    translateObj $obj $blink_x $blink_y
    # scaleObj takes a DIAMETER, not a radius -- the 2x bug that cost an
    # afternoon elsewhere in this tree
    scaleObj $obj [expr {2*$blink_r}]
    metagroupAdd $mg $obj
    glistAddObject $mg 0
}

proc blink_on  {} { glistSetCurGroup 0; glistSetVisible 1; redraw }
proc blink_off {} { glistSetVisible 0; redraw }
proc reset     {} { glistSetVisible 0; redraw }
proc clearscreen {} { glistSetVisible 0; redraw }

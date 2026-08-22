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

# name -> rgb, so a loader option is one word and stimdg reads plainly
proc color_rgb { name } {
    switch -exact -- $name {
        green  { return {0.0 0.8 0.2} }
        amber  { return {0.9 0.6 0.0} }
        blue   { return {0.2 0.5 1.0} }
        red    { return {0.9 0.2 0.2} }
        white  { return {0.9 0.9 0.9} }
    }
    return {0.0 0.8 0.2}
}

proc nexttrial { id } {
    glistInit 1
    resetObjList

    foreach p "blink_x blink_y blink_r" { set $p [dl_get stimdg:$p $id] }
    set color [color_rgb [dl_get stimdg:blink_color $id]]

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

#
# Monitor touch regions
#

lappend auto_path /usr/local/lib
package require Tk
package require qpcs

font create myDefaultFont -family {Helvetica} -size 11
option add *font myDefaultFont

set status(touch_hor) 0
set status(touch_ver) 0
set status(touchwin) 0

proc server_cmd { server cmd } {
    set sock [socket $server 2570]
    fconfigure $sock -buffering line
    puts $sock $cmd
    set result [gets $sock]
    close $sock
    return $result
}

proc initialize_vars { server } {
    set status(touchwin) \
	[lindex [lindex [qpcs::dsGet $server qpcs/touch_region_status] 5] 1]
    if { $status(touchwin) == "" } { set status(touchwin) 0 }
    update_touchwin_indicators
}

proc connect_to_server { server } {
    initialize_vars $server
    if { [qpcs::dsRegister $server] != 1 } {
	error "Unable to register with $server"
    }
    qpcs::dsAddCallback process_data
    qpcs::dsAddMatch $server qpcs/*
}

proc disconnect_from_server {} {
    qpcs::dsUnregister 
}


proc update_touch_regions {} {
    foreach reg "0 1 2 3 4 5 6 7" {
	server_cmd $::datahub "touchGetRegionInfo $reg"
    }
}

proc resize_disp_canvas { w h } {
    global status
    set status(disp_canvas_width) $w 
    set status(disp_canvas_height) $h
    update_touch_marker $status(touch_hor) $status(touch_ver)	
}

proc add_touchwin_indicators {} {
    global widgets
    for { set i 0 } { $i < 8 } { incr i } {
	$widgets(touchwin_canvas) \
	    create oval 0 0 1 1 -fill black -tag reg$i
    }
    position_touchwin_indicators
}

proc position_touchwin_indicators {} {
    global status widgets
    set msize 5
    set hwidth [expr $status(touchwin_canvas_width)/2]
    set hheight [expr $status(touchwin_canvas_height)/2]
    
    set x [expr {8+($hwidth+$msize)/2}]
    set inc [expr ($hwidth-(2*(8-$msize)))/8.]
    for { set i 0 } { $i < 8 } { incr i } {
	$widgets(touchwin_canvas) coords reg[expr 7-$i] \
	    [expr $x-$msize] [expr $hheight-$msize] \
	    [expr $x+$msize] [expr $hheight+$msize]
	set x [expr $x+$inc]
    }
}

    
proc update_touchwin_indicators {} {
    global status widgets
    for { set i 0 } { $i < 8 } { incr i } {
	if { [expr $status(touchwin)&(1<<$i)] != 0 } {
	    $widgets(touchwin_canvas) itemconfigure reg$i -fill yellow
	} else {
	    $widgets(touchwin_canvas) itemconfigure reg$i -fill black
	}
    }
}


proc resize_touchwin_canvas { w h } {
    global status
    set status(touchwin_canvas_width) $w 
    set status(touchwin_canvas_height) $h
    position_touchwin_indicators
}
    
proc update_touch_region_setting { reg active state type cx cy dx dy args } {
    global widgets status
    set msize_x [expr {($dx/3)}]
    set msize_y [expr {($dy/3)}]
    set csize 2
    set x0 [expr ($cx/3)]
    set y0 [expr ($cy/3)]

    if { $active == 1 } { set cstate normal } { set cstate hidden }
    if  { $type == 1 } { 
	$widgets(disp_canvas) \
	    coords $widgets(touch_reg_ellipse_$reg) \
	    [expr $x0-$msize_x] [expr $y0-$msize_y] \
	    [expr $x0+$msize_x] [expr $y0+$msize_y]
	$widgets(disp_canvas) \
	    coords $widgets(touch_reg_center_$reg) \
	    [expr $x0-$csize] [expr $y0-$csize] \
	    [expr $x0+$csize] [expr $y0+$csize]
	$widgets(disp_canvas) itemconfigure regE$reg -state $cstate
	$widgets(disp_canvas) itemconfigure regC$reg -state $cstate
	$widgets(disp_canvas) itemconfigure regR$reg -state hidden
    } else {
	$widgets(disp_canvas) \
	    coords $widgets(touch_reg_rect_$reg) \
	    [expr $x0-$msize_x] [expr $y0-$msize_y] \
	    [expr $x0+$msize_x] [expr $y0+$msize_y]
	$widgets(disp_canvas) \
	    coords $widgets(touch_reg_center_$reg) \
	    [expr $x0-$csize] [expr $y0-$csize] \
	    [expr $x0+$csize] [expr $y0+$csize]
	$widgets(disp_canvas) itemconfigure regR$reg -state $cstate
	$widgets(disp_canvas) itemconfigure regC$reg -state $cstate
	$widgets(disp_canvas) itemconfigure regE$reg -state hidden
    }
}

proc update_touch_marker { x y } {
    global status widgets
    set x0 [expr ($x/3)]
    set y0 [expr ($y/3)]
    set msize 3
    $widgets(disp_canvas) \
	coords $widgets(touch_marker) [expr $x0-$msize] [expr $y0-$msize] \
	[expr $x0+$msize] [expr $y0+$msize] 
    
}


proc process_data { ev args } {
    global status widgets
    set name [lindex $args 0]
    set val [lindex $args 4]
    switch -glob $name {
	qpcs/touch {
	    set status(touch_hor) [lindex $val 0]
	    set status(touch_ver) [lindex $val 1]
	    update_touch_marker $status(touch_hor) $status(touch_ver)	
	}

	qpcs/touch_region_setting {
	    update_touch_region_setting {*}$val
	}
	qpcs/touch_region_status {
	    set status(touchwin) [lindex $val 1]
	    update_touchwin_indicators
	}
    }
}
    
proc setup_view {} {
    wm title . "Touch Region Viewer"
    
    global status widgets
    
    set status(disp_canvas_width) 200
    set status(disp_canvas_height) 200
    set widgets(disp_canvas) \
	[canvas .dispc \
	     -width $status(disp_canvas_width) \
	     -height $status(disp_canvas_height) -background black]
    set widgets(touch_marker) \
	[.dispc create oval 94 94 106 106 -outline white]
    
    foreach reg "0 1 2 3 4 5 6 7" {
	set widgets(touch_reg_ellipse_$reg) \
	    [.dispc create oval 90 90 110 110 -outline red -state hidden -tag regE$reg]
	set widgets(touch_reg_rect_$reg) \
	    [.dispc create rect 90 90 110 110 -outline red -state hidden -tag regR$reg]
	set widgets(touch_reg_center_$reg) \
	    [.dispc create oval 90 90 110 110 -outline red -state hidden -tag regC$reg]
	
    }
    
    bind .dispc <Configure> { resize_disp_canvas %w %h } 
    grid .dispc -sticky nsew
    
    
    set status(touchwin_canvas_width) 200
    set status(touchwin_canvas_height) 35
    
    set widgets(touchwin_canvas) \
	[canvas .fixc -width $status(touchwin_canvas_width) \
	     -height $status(touchwin_canvas_height) -background lightgray]
    
    add_touchwin_indicators
    
    bind .fixc <Configure> { resize_touchwin_canvas %w %h } 
    grid .fixc -sticky nsew
    
    grid columnconfigure . 0 -weight 1
    grid rowconfigure . 3 -weight 1
    
    bind . <Control-h> {console show}
}


if { $argc > 0 } { set ::datahub [lindex $argv 0] }
setup_view
connect_to_server $datahub 

after 300 update_touch_regions


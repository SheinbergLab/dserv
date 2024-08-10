package require Tk
package require qpcs


proc update_ain_position { x y } {
    dl_local coords [dl_create short [expr {$y*16}] [expr {$x*16}]]
    global ds_sock
    for { set i 0 } { $i < 20 } { incr i } { 
	qpcs::dsSocketSetData $ds_sock ain/vals $coords
    }
}

proc markerStash { w x y } {
    global marker
    $w addtag curmarker withtag current
    set marker(x) $x
    set marker(y) $y
    set marker(moved) 0
}

proc markerMove { w x y } {
    global marker
    set item [$w find withtag curmarker]
    if { $item == "" } return
    $w move $item  [expr $x-$marker(x)] [expr $y-$marker(y)]
    set marker(moved) 1
    set marker(x) $x
    set marker(y) $y

    update_ain_position $x $y
}

proc markerAccept { w } {
    global marker
    set marker(moved) 0
    
    set currentItem [$w find withtag curmarker]
    set curtags [$w gettags $currentItem]
    
    $w dtag curmarker
    # now do something
}


proc add_fixwin_indicators {} {
    global widgets
    for { set i 0 } { $i < 8 } { incr i } {
	$widgets(fixwin_canvas) \
	    create oval 0 0 1 1 -fill black -tag reg$i
    }
    position_fixwin_indicators
}

proc position_fixwin_indicators {} {
    global widgets
    set msize 5
    set hwidth [expr $::status(fixwin_canvas_width)/2]
    set hheight [expr $::status(fixwin_canvas_height)/2]
    
    set x [expr {8+($hwidth+$msize)/2}]
    set inc [expr ($hwidth-(2*(8-$msize)))/8.]
    for { set i 0 } { $i < 8 } { incr i } {
	$widgets(fixwin_canvas) coords reg[expr 7-$i] \
	    [expr $x-$msize] [expr $hheight-$msize] \
	    [expr $x+$msize] [expr $hheight+$msize]
	set x [expr $x+$inc]
    }
}

proc update_fixwin_indicators {} {
    for { set i 0 } { $i < 8 } { incr i } {
	if { [expr $::status(fixwin)&(1<<$i)] != 0 } {
	    $::widgets(fixwin_canvas) itemconfigure reg$i -fill yellow
	} else {
	    $::widgets(fixwin_canvas) itemconfigure reg$i -fill black
	}
    }
}

proc update_em_region_setting { reg active state type cx cy dx dy args } {
    global params
    foreach p "cx cy dx dy type" { set params($p,$reg) [set $p] }
    
    set sx [expr ($cx-2048)/200.]
    set sy [expr -1*($cy-2048)/200.]
    set w $::status(disp_canvas_width)
    set h $::status(disp_canvas_height)
    set aspect [expr {1.0*$h/$w}]
    set range_h 20.0
    set hrange_h [expr {0.5*$range_h}]
    set range_v [expr {$range_h*$aspect}]
    set hrange_v [expr {0.5*$range_v}]
    set pix_per_deg_h [expr $w/$range_h]
    set pix_per_deg_v [expr $h/$range_v]
    set msize_x [expr {($dx/200.)*$pix_per_deg_h}]
    set msize_y [expr {($dy/200.)*$pix_per_deg_v}]
    set csize 2
    set hw [expr $w/2]
    set hh [expr $h/2]
    set x0 [expr (($sx/$hrange_h)*$hw)+$hw]
    set y0 [expr $hh-(($sy/$hrange_v)*$hh)]
    if { $active == 1 } { set cstate normal } { set cstate hidden }
    if  { $type == 1 } { 
	$::widgets(disp_canvas) \
	    coords $::widgets(em_reg_ellipse_$reg) \
	    [expr $x0-$msize_x] [expr $y0-$msize_y] \
	    [expr $x0+$msize_x] [expr $y0+$msize_y]
	$::widgets(disp_canvas) \
	    coords $::widgets(em_reg_center_$reg) \
	    [expr $x0-$csize] [expr $y0-$csize] \
	    [expr $x0+$csize] [expr $y0+$csize]
	$::widgets(disp_canvas) itemconfigure regE$reg -state $cstate
	$::widgets(disp_canvas) itemconfigure regC$reg -state $cstate
	$::widgets(disp_canvas) itemconfigure regR$reg -state hidden
    } else {
	$::widgets(disp_canvas) \
	    coords $::widgets(em_reg_rect_$reg) \
	    [expr $x0-$msize_x] [expr $y0-$msize_y] \
	    [expr $x0+$msize_x] [expr $y0+$msize_y]
	$::widgets(disp_canvas) \
	    coords $::widgets(em_reg_center_$reg) \
	    [expr $x0-$csize] [expr $y0-$csize] \
	    [expr $x0+$csize] [expr $y0+$csize]
	$::widgets(disp_canvas) itemconfigure regR$reg -state $cstate
	$::widgets(disp_canvas) itemconfigure regC$reg -state $cstate
	$::widgets(disp_canvas) itemconfigure regE$reg -state hidden
    }
}


proc process_data { ev args } {
    global status
    set name [lindex $args 0]
    set val [lindex $args 4]
    switch -glob $name {
	proc/windows/settings {
	    dl_local v [qpcs::dsData $args]
	    update_em_region_setting {*}[dl_tcllist $v]
	}
	proc/windows/status {
	    dl_local v [qpcs::dsData $args]
	    set status(fixwin) [lindex [dl_tcllist $v] 1]
	    update_fixwin_indicators
	}
    }
}

proc get_ain_coords {} { 
    if { [catch { dl_local curpos [qpcs::dsGetData $::server ain/vals] }] } {
	set x 128
	set y 128
    } else {
	lassign [dl_tcllist $curpos] y x
	set x [expr $x/16]
	set y [expr $y/16]
    }
    set ::marker(x) $x
    set ::marker(y) $y
    return [list $x $y]
}

proc reset_ain_coords { w } {
    lassign [get_ain_coords] x y
    set coords "[expr $x-5] [expr $y-5] [expr $x+5] [expr $y+5]"
    $w itemconfigure ainMarker -coords 
}


proc ainRegionState { reg state } {
    ess_cmd [list processSetParam windows active $state $reg]
}
proc ainGetRegionInfo { reg } {
    ess_cmd [list processSetParam windows settings 1 $reg]
}


proc ainSetParam { p v } { processSetParam "windows" $p $v }
proc ainSetIndexedParam { i p v } {  processSetParam "windows" $p $v $i }
proc ainSetIndexedParams { win args } {
    if { [expr {[llength $args]%2}] } {
	error "wrong number of arguments"
    }
    for { set i 0 } { $i < [llength $args] } { incr i 2 } {
	lassign [lrange $args $i [expr {$i+2}]] p v
	processSetParam "windows" $p $v $win
    }
}


proc ainGetParam { p } { processGetParam "windows" $p }
proc ainGetIndexedParam { i p } {  processGetParam "windows" $p $i }

proc update_setting { win } {
    ainSetIndexedParam $win settings 0
}

proc check_state { win } {
    variable em_windows
    set state [ainGetIndexedParam $win state]
    if { $state } {
	set em_windows(states) [expr $em_windows(states) | (1<<$win)]
    } else {
	set em_windows(states) [expr $em_windows(states) & ~(1<<$win)]
    }
    return $state
}

proc region_on { win } {
    ainSetIndexedParam $win active 1
    check_state $win
    update_setting $win
}

proc region_off { win } {
    ainSetIndexedParam $win active 0
    check_state $win
    update_setting $win
}

proc region_set { win type center_x center_y
		  plusminus_x plusminus_y } {
    ainSetIndexedParams $win type $type \
	center_x $center_x center_y $center_y \
	plusminus_x $plusminus_x plusminus_y $plusminus_y
    check_state $win
    update_setting $win
}


proc set_active { region w } {
    if { [lsearch [$w state] selected] > 0 } { set active 1 } else { set active 0 }
    ainRegionState $region $active
    ainGetRegionInfo $region
}

proc setup {} {
    global status widgets
    
    labelframe .analog -text "Monitor"
    set f [frame .analog.f]

    lassign [get_ain_coords] x y

    
    set status(disp_canvas_width) 256
    set status(disp_canvas_height) 256
    set widgets(disp_canvas) $f.c
    canvas $widgets(disp_canvas) -background black \
	-width $status(disp_canvas_width) -height $status(disp_canvas_height)
    set circ [$f.c create oval \
		  "[expr $x-5] [expr $y-5] [expr $x+5] [expr $y+5]" \
		  -tags ainMarker]		 
    $f.c itemconfigure $circ -fill white -outline white -width 2

    foreach reg "0 1 2 3 4 5 6 7" {
	set widgets(em_reg_ellipse_$reg) \
	    [$widgets(disp_canvas) create oval 90 90 110 110 -outline red -state hidden -tag regE$reg]
	set widgets(em_reg_rect_$reg) \
	    [$widgets(disp_canvas) create rect 90 90 110 110 -outline red -state hidden -tag regR$reg]
	set widgets(em_reg_center_$reg) \
	    [$widgets(disp_canvas) create oval 90 90 110 110 -outline red -state hidden -tag regC$reg]
    }
    
    $f.c bind ainMarker <Button-1> { focus %W; markerStash %W %x %y }
    $f.c bind ainMarker <B1-Motion> { markerMove %W %x %y }
    $f.c bind ainMarker <ButtonRelease-1> { markerAccept %W }
    $f.c bind ainMarker <Any-Enter> "$f.c itemconfig current -outline #aaaaaa"
    $f.c bind ainMarker <Any-Leave> "$f.c itemconfig current -outline white"

    # update ain/vals on double click
    bind $f.c <Double-1> { update_ain_position $::marker(x) $::marker(y) }

    set status(fixwin_canvas_width) 200
    set status(fixwin_canvas_height) 35

    set widgets(fixwin_canvas) $f.indicators
    canvas $widgets(fixwin_canvas) -width $::status(fixwin_canvas_width) \
	-height $::status(fixwin_canvas_height) -background lightgray
    add_fixwin_indicators

    pack $f.c $widgets(fixwin_canvas)
    pack $f
    pack .analog -fill both -expand true

    global params
    labelframe .windows -text Windows
    foreach i "0 1 2 3 4 5 6 7" {
	set f [frame .windows.win${i}]
	label $f.label -text "Win${i}"
	::ttk::spinbox $f.cx -from 0 -to 4096 -text cx -width 5 -textvariable params(cx,$i)
	::ttk::spinbox $f.cy -from 0 -to 4096 -text cy -width 5 -textvariable params(cy,$i)
	::ttk::spinbox $f.px -from 0 -to 4096 -text rx -width 5 -textvariable params(dx,$i)
	::ttk::spinbox $f.py -from 0 -to 4096 -text ry -width 5 -textvariable params(dy,$i)
	::ttk::spinbox $f.type -from 0 -to 1 -text type -width 5 -textvariable params(type,$i)
	::ttk::checkbutton $f.active -text "Active" -style Toolbutton -command "set_active $i $f.active"
	pack $f.label $f.cx $f.cy $f.px $f.py $f.type $f.active -side left
	pack $f
    }
    pack .windows -expand true -fill x
}

proc initialize_vars { server } {
    global widgets status
}

proc server_open { { host 127.0.0.1 } } {
    global sock
    set sock [socket $host 2570]
    fconfigure $sock -buffering line
    fileevent $sock readable [list echo_line $sock]
}

proc server_close {} {
    global sock
    if { $sock != {} } { close $sock }
}


proc initialize_regions {} {
    foreach i "0 1 2 3 4 5 6 7" {
	ainGetRegionInfo $i
    }
}

set dir [file dirname [info script]]
set iconfile [file join $dir play.png]
image create photo essicon -file $iconfile
wm title . "ESS Input"
wm iconphoto . -default essicon

setup

proc ess_cmd { cmd } {
    puts $::ess_sock $cmd
    return [gets $::ess_sock]
}

proc ds_cmd { cmd } {
    puts $::ds_sock $cmd
    return [gets $::ds_sock]
}

if { [llength $argv] > 0 } { set server [lindex $argv 0] } { set server 127.0.0.1 }
set ess_port 2570
set ess_sock [socket $server $ess_port]
fconfigure $ess_sock -buffering line

set ds_port 4620
set ds_sock [socket $server $ds_port]
fconfigure $ds_sock -buffering line

initialize_vars $server
if { [qpcs::dsRegister $server] != 1 } {
    error "Unable to register with $server"
}

qpcs::dsAddCallback process_data
qpcs::dsAddMatch $server proc/windows/*

initialize_regions

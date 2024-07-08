package require Tk
package require qpcs

proc update_eye_position { x y } {
    dl_local coords [dl_create short [expr {$y*16}] [expr {$x*16}]]
    set ds [qpcs::dsSocketOpen $::server]
    for { set i 0 } { $i < 20 } { incr i } { 
	qpcs::dsSocketSetData $ds ain/vals $coords
    }
    close $ds
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

    update_eye_position $x $y
}

proc markerAccept { w } {
    global marker
    set marker(moved) 0
    
    set currentItem [$w find withtag curmarker]
    set curtags [$w gettags $currentItem]
    
    $w dtag curmarker
    # now do something
}

proc get_eye_coords {} { 
    if { [catch { dl_local curpos [qpcs::dsGetData localhost ain/vals] }] } {
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

proc reset_eye_coords { w } {
    lassign [get_eye_coords] x y
    set coords "[expr $x-5] [expr $y-5] [expr $x+5] [expr $y+5]"
    $w itemconfigure eyeMarker -coords 
}

proc setup {} {
    global status widgets
    
    labelframe .analog -text "Eye Position"
    set f [frame .analog.f]

    lassign [get_eye_coords] x y
    
    canvas $f.c -background black -width 256 -height 256
    set circ [$f.c create oval \
		  "[expr $x-5] [expr $y-5] [expr $x+5] [expr $y+5]" \
		  -tags eyeMarker]		 
    $f.c itemconfigure $circ -fill white -outline white -width 2

    $f.c bind eyeMarker <Button-1> { focus %W; markerStash %W %x %y }
    $f.c bind eyeMarker <B1-Motion> { markerMove %W %x %y }
    $f.c bind eyeMarker <ButtonRelease-1> { markerAccept %W }
    $f.c bind eyeMarker <Any-Enter> "$f.c itemconfig current -outline #aaaaaa"
    $f.c bind eyeMarker <Any-Leave> "$f.c itemconfig current -outline white"

    # update ain/vals on double click
    bind $f.c <Double-1> { update_eye_position $::marker(x) $::marker(y) }

    pack $f.c
    pack $f
    pack .analog
    
    labelframe .buttons -text Buttons
    ttk::button .buttons.left -text "Left" 
    ttk::button .buttons.right -text "Right"
    
    bind .buttons.left  <ButtonPress-1> { left 1 }
    bind .buttons.left  <ButtonRelease-1> { left 0 }
    bind .buttons.right <ButtonPress-1> { right 1 }
    bind .buttons.right <ButtonRelease-1> { right 0 }
    
    pack .buttons.left .buttons.right -side left -expand true -fill x
    pack .buttons -fill x -expand true -anchor n
}

proc initialize_vars { server } {
    global widgets status
    set stateinfo [qpcs::dsGet $server qpcs/state]
    set status(state) [string totitle [lindex $stateinfo 5]]
}

proc connect_to_server { server } {
    initialize_vars $server
    set connected 1
}

proc disconnect_from_server {} {
}

proc left  { state } { update_button left $state  }
proc right { state } { update_button right $state }

proc terminal_output { line } {
    .essterm.output configure -state normal
    .essterm.output insert end $line
    .essterm.output insert end \n
    .essterm.output configure -state disabled
}

proc echo_line { s } {
    set line [gets $s]
    terminal_output $line
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


proc update_button { side state } {
    set mappings "left 24 right 25"
    qpcs::dsSet $::server gpio/input/[dict get $mappings $side] $state
}

set dir [file dirname [info script]]
set iconfile [file join $dir play.png]
image create photo essicon -file $iconfile
wm title . "Experimental Control"
wm iconphoto . -default essicon

setup

if { [llength $argv] > 0 } { set server [lindex $argv 0] } { set server 127.0.0.1 }


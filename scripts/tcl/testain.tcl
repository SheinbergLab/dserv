package require dlsh
package require qpcs

proc test_tcl_eval { n } {
    set s [socket localhost 2570]
    fconfigure $s -buffering line
    for { set i 0 } { $i < $n } { incr i } { 
	set script {expr 5*5}
	puts $s $script
	gets $s
    }
    close $s
}

proc setup_window_processor {} {
    set s [socket localhost 2570]
    fconfigure $s -buffering line
    set script { ainSetProcessor windows;
	ainSetParam dpoint proc/windows;
	dservAddExactMatch proc/windows/status;
	#dpointSetScript proc/windows/status { set x [dservGet proc/windows/status] }
	dpointSetScript proc/windows/status { set x 100 }
    }
    puts $s $script
    gets $s
    close $s
}


proc update_eye_position { ds x y } {
    dl_local coords [dl_create short $y $x]
    for { set i 0 } { $i < 20 } { incr i } {
	qpcs::dsSocketSetData $ds ain/vals $coords
	after 1
    }
}


proc touch_position { x y } {
    dl_local coords [dl_create short $x $y]
    set ds [qpcs::dsSocketOpen $::server]
    qpcs::dsSocketSetData $ds mtouch/touchvals $coords
    close $ds
}

proc do_test { ds n ms } {
    for { set ::count 0 } { $::count < $n } { incr ::count } {
	set x [expr int(rand()*4096)]
	set y [expr int(rand()*4096)]
	update_eye_position $ds $x $y
	after $ms
	update
    }
}


setup_window_processor

set server localhost
set ds [qpcs::dsSocketOpen $::server]
set n 100
set pause 10
set count 0

frame .p
label .p.nlabel -text "N: "
spinbox .p.n -from 1 -to 10000 -textvariable n
pack .p.nlabel .p.n -side left
pack .p

frame .c
button .c.run -text "Run" -command { do_test $ds $::n $::pause }
label .c.count -textvariable count -width 5
pack .c.run .c.count -side left
pack .c

# send a bunch of script only
#test_tcl_eval 100

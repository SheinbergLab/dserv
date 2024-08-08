package require dlsh
package require qpcs

proc update_eye_position { ds x y } {
    dl_local coords [dl_create short $y $x]
    for { set i 0 } { $i < 20 } { incr i } {
	qpcs::dsSocketSetData $ds ain/vals $coords
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




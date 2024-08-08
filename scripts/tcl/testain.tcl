package require dlsh
package require qpcs

set server localhost
proc update_eye_position { x y } {
    dl_local coords [dl_create short $y $x]
    set ds [qpcs::dsSocketOpen $::server]
    for { set i 0 } { $i < 20 } { incr i } {
	qpcs::dsSocketSetData $ds ain/vals $coords
    }
    close $ds
}


proc touch_position { x y } {
    dl_local coords [dl_create short $x $y]
    set ds [qpcs::dsSocketOpen $::server]
    qpcs::dsSocketSetData $ds mtouch/touchvals $coords
    close $ds
}

proc do_test {} {
    for { set x 0 } { $x < 4096 } { incr x } {
	update_eye_position $x 2048
	puts "$x 2048"
    }
}
    
do_test

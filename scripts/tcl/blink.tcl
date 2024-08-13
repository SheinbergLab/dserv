set ledOn 0
set count 0
proc blink { args } {
    if { [incr ::count] > 20 } {
	timerRemoveScript 0
    }
    global ledOn last
    set current [now]
    set elapsed [expr $current-$last]
    set last $current
    if { $ledOn } { print "blink_off $elapsed"} { print "blink_on  $elapsed" }
    set ledOn [expr 1-$ledOn]
}

timerSetScript 0 blink
timerTickInterval 0 10 10
set last [now]

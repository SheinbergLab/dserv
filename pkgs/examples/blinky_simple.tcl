source ess-2.0.tm
set blinky [System new blinky]

$blinky add_variable n     100
$blinky add_variable count 0
$blinky add_variable start_time 0

$blinky set_init_callback {
    variable n
    variable count
    set n 100
    set count 0
}
$blinky set_reset_callback { variable count; set count 0 }
$blinky set_start_callback { variable start_time; set start_time [now] }

$blinky set_start      blink_on

$blinky add_action     blink_on {
    variable start_time
    variable count
    set elapsed [expr {[now]-$start_time}]
    print "on:  $elapsed"; rpioPinOn 27; timerTick 0 10; incr count
}
$blinky add_transition blink_on { if [timerExpired 0] { return blink_off } }

$blinky add_action     blink_off {
    variable start_time
    set elapsed [expr {[now]-$start_time}]
    print "off: $elapsed"; rpioPinOff 27; timerTick 0 10
}
$blinky add_transition blink_off {
    variable count
    variable n
    if [timerExpired 0] {
	if { $count >= $n } {
	    return end
	} else {
	    return blink_on
	}
    }
}

$blinky set_end {
    variable n
    variable start_time
    print "[expr ([now]-$start_time)/1000]ms ([expr $n*20]ms)"
}

ess::system_init blinky

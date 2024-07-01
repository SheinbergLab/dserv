package require ess
ess::clear_systems blinky
set blinky [System new blinky]


############### parameters ##################

$blinky add_param nblinks     100  variable int
$blinky add_param blink_on_t  100  time int
$blinky add_param blink_off_t 100  time int
$blinky add_param delay       1000 time int
$blinky add_param led_pin     27   variable int
$blinky add_param use_rmt     1    variable bool
$blinky add_param rmt_ip      $ess::rmt_host variable string

############### local variable ##############

$blinky add_variable blink_count 0

################## start ####################

$blinky set_start delay

################## delay ####################

$blinky add_action delay { timerTick 0 $delay }
$blinky add_transition delay { if [timerExpired 0] { return blink_on } }

################# blink_on ##################

$blinky add_action blink_on {
    incr blink_count
    if { $nblinks < 0 || $blink_count < $nblinks } {
	rpioPinOn $led_pin
	if { $use_rmt } { rmtSend "setBackground 200 200 200" }
	timerTick 0 $blink_on_t
    }
}
$blinky add_transition blink_on {
    if { $nblinks >= 0 && [expr $blink_count >= $nblinks] } { return end }
    if [timerExpired 0] { return blink_off }
}


################ blink_off ##################

$blinky add_action blink_off {
    rpioPinOff $led_pin
    if { $use_rmt } { rmtSend "setBackground 0 0 0" }
    timerTick 0 $blink_off_t
}
$blinky add_transition blink_off {
    if [timerExpired 0] { return blink_on }
}

################### end #####################

$blinky set_end {
    rpioPinOff $led_pin
    if { $use_rmt } { rmtSend "setBackground 0 0 0" }
}

$blinky set_start_callback {
    if { $use_rmt } { rmtOpen $rmt_ip } { rmtClose }
}

$blinky set_quit_callback {
    rpioPinOff $led_pin
    if { $use_rmt } { rmtSend "setBackground 0 0 0" }
}

$blinky set_init_callback {
    ess::init
}

$blinky set_deinit_callback {
    if { $use_rmt } { rmtClose }
}

$blinky set_reset_callback {
    set blink_count 0
}


return $blinky

package require ess
set blinky [System new blinky]


############### parameters ##################

$blinky add_param nblinks     100  variable int
$blinky add_param blink_on_t  100  time int
$blinky add_param blink_off_t 100  time int
$blinky add_param delay       1000 time int
$blinky add_param led_pin     27   variable int
$blinky add_param use_rmt     0    variable bool
$blinky add_param rmt_ip      $ess::rmt_host variable string

############ system variables ################

$blinky add_variable blink_count 0

################## start ####################

$blinky set_start delay

################## delay ####################

$blinky add_action delay { variable delay; timerTick 0 $delay }
$blinky add_transition delay { if [timerExpired 0] { return blink_on } }

################# blink_on ##################

$blinky add_action blink_on {
    variable nblinks
    variable blink_count
    variable led_pin
    variable blink_on_t
    variable use_rmt

    incr blink_count
    if { $nblinks < 0 || $blink_count < $nblinks } {
	rpioPinOn $led_pin
	if { $use_rmt } { rmtSend "setBackground 200 200 200" }
	timerTick 0 $blink_on_t
    }
}
$blinky add_transition blink_on {
    variable nblinks
    variable blink_count
    if { $nblinks >= 0 && [expr $blink_count >= $nblinks] } { return end }
    if [timerExpired 0] { return blink_off }
}


################ blink_off ##################

$blinky add_action blink_off {
    variable blink_off_t
    variable led_pin
    variable use_rmt
    
    rpioPinOff $led_pin
    if { $use_rmt } { rmtSend "setBackground 0 0 0" }
    timerTick 0 $blink_off_t
}
$blinky add_transition blink_off {
    if [timerExpired 0] { return blink_on }
}

################### end #####################

$blinky set_end {
    variable led_pin
    variable use_rmt
    rpioPinOff $led_pin
    if { $use_rmt } { rmtSend "setBackground 0 0 0" }
}

$blinky set_start_callback {
    variable use_rmt
    variable rmt_ip
    if { $use_rmt } { rmtOpen $rmt_ip } { rmtClose }
}

$blinky set_quit_callback {
    variable led_pin
    variable use_rmt
    rpioPinOff $led_pin
    if { $use_rmt } { rmtSend "setBackground 0 0 0" }
}

$blinky set_init_callback {
    ess::init
}

$blinky set_deinit_callback {
    variable use_rmt
    if { $use_rmt } { rmtClose }
}

$blinky set_reset_callback {
    variable blink_count
    set blink_count 0
}



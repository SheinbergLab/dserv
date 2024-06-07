source ess-2.0.tm
set blinky [System new blinky]

$blinky set_start      blink_on

$blinky add_action     blink_on { rpioPinOn 27; timerTick 1000 }
$blinky add_transition blink_on { if [timerExpired] { return blink_off } }

$blinky add_action     blink_off { rpioPinOff 27; timerTick 1000 }
$blinky add_transition blink_off { if [timerExpired] { return blink_on } }


ess::set_system $blinky

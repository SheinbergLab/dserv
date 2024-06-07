source ess-2.0.tm
set blinky [System new blinky]

$blinky add_variable nblinks     -1
$blinky add_variable blink_count 0
$blinky add_variable blink_on_t  20
$blinky add_variable blink_off_t 20
$blinky add_variable delay       1000
$blinky	add_variable timerID     1
$blinky add_variable ledPin      27

################## start ####################

$blinky add_state start {}  { return delay }


################## delay ####################

set action { variable timerID; variable delay; timerTick $timerID $delay }
set transition { variable timerID; if [timerExpired $timerID] { return blink_on } }
$blinky add_state delay $action $transition


################# blink_on ##################

set action {
    variable ledPin
    variable timerID
    variable nblinks
    variable blink_count
    variable blink_on_t
    if { $nblinks < 0 || $blink_count < $nblinks } {
	incr blink_count; rpioSet [expr 1 << $ledPin]
	timerTick $timerID $blink_on_t
    }
}
set transition {
    variable timerID
    variable nblinks
    variable blink_count
    if { $nblinks >= 0 && [expr $blink_count >= $nblinks] } { return end }
    if [timerExpired $timerID] { return blink_off }
}
$blinky add_state blink_on $action $transition


################ blink_off ##################

set action {
    variable timerID
    variable blink_off_t
    variable ledPin
    rpioClear [expr 1 << $ledPin]
    timerTick $timerID $blink_off_t
}
set transition {
    variable timerID
    if [timerExpired $timerID] { return blink_on }
}
$blinky add_state blink_off $action $transition


################### end #####################

$blinky add_state end { variable ledPin; rpioClear [expr 1 << $ledPin] } {}

ess::set_system $blinky

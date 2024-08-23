#
# NAME
#  testgpiolatency.tcl
#
# DESCRIPTION
#  Find roundtrip latency for input line change to output line acknowledgment
#
# DETAILS
#  Open two line for output and one for input.  Use one of the output lines to trigger
# the input line (by connected by wire and setting up gpio_input to listen to that line).
# Attach a script to the input line variable which in turn sets the third output line.
# On a scope, track both output lines and determine the latency.
#

# line definitions
set in 13
set out0 16
set out1 21

# track current output pin value
set pinval 0

# initialize one input line and two output lines for this test
proc initialize_pins {} {
    if { [gpioLineRequestInput $::in] < 0 } { error "input $::in cannot be acquired" }
    if { [gpioLineRequestOutput $::out0] < 0 } { error "input $::out0 cannot be acquired" }
    if { [gpioLineRequestOutput $::out1] < 0 } { error "input $::out1 cannot be acquired" }
}

# toggle the initial output (out0) 
proc toggle { args } {
    set ::pinval [expr 1-$::pinval]
    gpioLineSetValue $::out0 $::pinval
}

# set acknowledge output (out1) to match other output (out0)
proc gpio_acknowledge { args } {
    gpioLineSetValue $::out1 $::pinval
}

# start a timer to toggle output line every interval ms
proc start_timer { interval } {
    timerSetScript 0 "toggle"
    timerTickInterval 0 $interval $interval
}

# intialize and attach script to input line to acknowledge
initialize_pins
dservAddExactMatch gpio/input/$::in
dpointSetScript gpio/input/$::in gpio_acknowledge
start_timer 10



proc timer_expired {} {
    set current_time [now]
    timerTick 1000
    print "timer 0: [expr {$current_time-$::last_time}]"
    set ::last_time $current_time
}

timerSetScript 0 timer_expired
timerTick 1000
set last_time [now]

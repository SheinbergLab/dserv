# loopback_rtt.tcl -- single-shot DO->DI loopback round-trip, measured inside dserv
# (no shell spawns, no sleeps). Source in dserv, then call loopback_rtt / loopback_stats.
#
# Wire DO<opin> -> DI<ipin> (e.g. GP1->GP2). Each call flips the output to force a
# fresh edge, waits for the box to echo state/di/<ipin>, returns elapsed us:
#   arrival[now] - command[dservTimestamp].
#
# NOTE: dpoint-script callbacks run in dserv's SERVER interp, not the interactive
# prompt's -- so we signal arrival through a scratch DATAPOINT (test/lb_arrival),
# not a Tcl variable, and poll it (Tcl-var vwait would never wake here).
#
#   source host/loopback_rtt.tcl
#   loopback_rtt   extio/rig1 1 2
#   loopback_stats extio/rig1 1 2 200

proc __lb_capture {args} { dservSet test/lb_arrival [now] }

proc loopback_rtt {dev {opin 1} {ipin 2} {timeout_ms 500}} {
    set di $dev/state/di/$ipin

    # flip the output relative to its last committed value -> guaranteed edge
    set cur 0
    catch { set cur [dservGet $dev/state/do/$opin] }
    set new [expr {$cur ? 0 : 1}]

    dservSet test/lb_arrival 0
    dservAddExactMatch $di
    dpointSetScript  $di __lb_capture

    dservSet $dev/cmd/do/$opin $new
    set t0 [dservTimestamp $dev/cmd/do/$opin]

    set deadline [expr {[clock milliseconds] + $timeout_ms}]
    set a 0
    while {[clock milliseconds] < $deadline} {
        update                                  ;# pump the event loop
        set a [dservGet test/lb_arrival]
        if {$a != 0} break
    }

    catch { dpointSetScript $di {} }            ;# teardown never fails a measurement
    catch { dservRemoveMatch $di }

    if {$a == 0} { return -code error "timeout: no $di within ${timeout_ms} ms" }
    return [expr {$a - $t0}]
}

proc loopback_stats {dev {opin 1} {ipin 2} {n 200}} {
    set v {}
    for {set i 0} {$i < $n} {incr i} {
        if {![catch { loopback_rtt $dev $opin $ipin } rt]} { lappend v $rt }
    }
    set v [lsort -integer $v]
    set c [llength $v]
    if {$c < 2} { return "insufficient samples (check wire / pin modes / debounce 0)" }
    set sum 0; foreach x $v { incr sum $x }
    return [format "n=%d  min %d  med %d  p90 %d  p99 %d  max %d  mean %d us" \
        $c [lindex $v 0] [lindex $v [expr {$c/2}]] \
        [lindex $v [expr {int($c*0.9)}]] [lindex $v [expr {int($c*0.99)}]] \
        [lindex $v end] [expr {$sum/$c}]]
}

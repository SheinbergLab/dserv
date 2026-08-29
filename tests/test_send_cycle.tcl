# test_send_cycle.tcl -- end-to-end regression for the send-cycle guard
# and send timeout (TclServer.cpp send_command, SendGuard.h).
#
# Run by ctest as: dserv -c tests/test_send_cycle.tcl (a CONFIG script --
# subprocess creation hangs in the --tscript phase) with
# DSERV_SEND_TIMEOUT_MS=1000.  Against a pre-guard dserv the cycle send
# below wedges the a and b interp threads forever and ctest's TIMEOUT
# kills the test -- that hang IS the regression being pinned (2026-08-29
# outage: ess -> configs -> ess).
#
# The script self-asserts and prints ONE verdict line, because listener
# bind-retry noise (when another dserv holds the fixed ports, e.g. a dev
# box with a live instance) interleaves with output and defeats a strict
# multi-line PASS regex.
#
# Error-propagation shape worth knowing: the cycle is detected in b's
# interp, so b's send returns TCL_ERROR there; a's process loop wraps it
# as an "!TCL_ERROR ..." reply STRING which propagates back as a normal
# result.  The timeout, by contrast, fires in THIS interp's send and is a
# real Tcl error here.

puts "Start send cycle test."

set failures {}
proc assert {ok what} {
    if {$ok} { puts "  ok: $what" } else {
        puts "  FAIL: $what"
        lappend ::failures $what
    }
}

subprocess a
subprocess b

# a -> b -> a cycle: detected at b, reported back as a reply string
set r [send a {send b {send a {expr 1}}}]
assert [string match {*send cycle detected*} $r] "cycle refused"
assert [string match {*b -> a -> b*} $r]        "chain named"

# benign sends still work, and the guard state was cleaned up
assert [expr {[send a {expr {6*7}}] == 42}]          "benign send"
assert [expr {[send a {send b {expr {6*7}}}] == 42}] "benign chained send"

# timeout: a is busy for 3s, our send gives up at 1s (env override)
set rc [catch {send a {after 3000}} msg]
assert [expr {$rc == 1}]                             "timeout errored"
assert [string match {*timed out*} $msg]      "timeout named"

# recovery: once a finishes, its late reply was discarded harmlessly and
# a fresh send works
after 3200
assert [expr {[send a {expr {1 + 1}}] == 2}]         "recovered after timeout"

if {[llength $failures] == 0} {
    puts "SEND CYCLE TEST: ALL PASS"
} else {
    puts "SEND CYCLE TEST: [llength $failures] FAILURE(S): $failures"
}
shutdown

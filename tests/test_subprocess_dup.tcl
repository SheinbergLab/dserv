# test_subprocess_dup.tcl -- the `subprocess` command's duplicate-name
# error must be loud, clean, and point at the re-source idiom.
#
# Run by ctest as: dserv -c tests/test_subprocess_dup.tcl (a CONFIG script,
# like test_send_cycle.tcl).  Pins two things:
#
#   1. A second `subprocess <name> <script>` is a Tcl ERROR (never a silent
#      re-run or skip -- dsconf relies on it), and the message tells the
#      caller how to re-source the live child instead.
#   2. The message is CLEAN.  The port/script probe used to hand the interp
#      to Tcl_GetIntFromObj, which left "expected integer but got ..." in
#      the result whenever the argument was a script; the real message was
#      then appended to that junk ("expected integer but got a
#      list_subprocess: child process ... already exists", 2026-09-10).
#
# One verdict line, same reason as test_send_cycle.tcl.

puts "Start subprocess dup test."

set failures {}
proc assert {ok what} {
    if {$ok} { puts "  ok: $what" } else {
        puts "  FAIL: $what"
        lappend ::failures $what
    }
}

# first spawn with a SCRIPT argument (not a port) -- the case that polluted
set n [subprocess dup_child {set ::marker first}]
assert [expr {$n eq "dup_child"}]                          "first spawn returns the name"
assert [expr {[send dup_child {set ::marker}] eq "first"}] "config script ran"

# second spawn of the same name: loud error, clean text, re-source hint
set rc [catch {subprocess dup_child {set ::marker second}} msg]
assert [expr {$rc == 1}]                                    "duplicate name is a Tcl error"
assert [string match {subprocess: child process "dup_child" already exists*} $msg] \
    "message starts with the real complaint (no leaked int-parse text)"
assert [expr {![string match {*expected integer*} $msg]}]   "no 'expected integer' junk"
assert [string match {*send dup_child {source <path>}*} $msg] "re-source hint names the child"
assert [expr {[send dup_child {set ::marker}] eq "first"}]  "the duplicate did NOT re-run the script"

# the port form is still accepted (int probe intact)
set n2 [subprocess dup_child_port 0 {set ::x 1}]
assert [expr {$n2 eq "dup_child_port"}]                      "port + script form spawns"

# and re-sourcing the live child by the hinted route works
send dup_child {set ::marker resourced}
assert [expr {[send dup_child {set ::marker}] eq "resourced"}] "re-source via send works"

if {[llength $failures] == 0} {
    puts "SUBPROCESS DUP TEST: ALL PASS"
} else {
    puts "SUBPROCESS DUP TEST: [llength $failures] FAILURE(S): $failures"
}
shutdown

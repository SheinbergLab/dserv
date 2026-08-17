#
# test_private.tcl
#
#  Exercise DSERV_DPOINT_PRIVATE_FLAG semantics from the trigger interp:
#  - dservSetPrivate publishes a point whose name/metadata are visible
#    (dservKeys, dservTimestamp) but whose payload is unreadable (dservGet)
#  - dservCopy of a private point stays private
#  - trigger scripts never fire for private points
#
#  Run as: dserv --tscript tests/test_private.tcl
#

puts "Start private test."

# public baseline: normal points still readable
dservSet test/public hello
puts "public get: [dservGet test/public]"

# private point: listed, timestamped, but payload denied
dservSetPrivate test/secret shh1
puts "keys has secret: [expr {[lsearch [dservKeys] test/secret] >= 0}]"
set rc [catch {dservGet test/secret} msg]
puts "get denied: $rc"
puts "deny message: $msg"
puts "timestamp visible: [expr {[dservTimestamp test/secret] > 0}]"

# a copy of a private point is itself private
dservCopy test/secret test/secret_copy
set rc [catch {dservGet test/secret_copy} msg]
puts "copy denied: $rc [expr {[string match {*is private*} $msg] ? "private" : "missing"}]"

# triggers: arm on both; only the public set may fire.
# handlers run in queue order after this script completes, so a
# (wrong) trigger for test/secret would print before the test/pub line
# and break the expected output.
proc test_handler { name data } {
    puts "TRIGGER $name = $data"
}
proc exit_when_done { name data } {
    puts "Private test done."
    exit
}
triggerRemoveAll
triggerAdd test/secret 1 test_handler
triggerAdd test/pub 1 test_handler
triggerAdd test/done 1 exit_when_done

dservSetPrivate test/secret shh2
dservSet test/pub world
dservSet test/done 1

puts "End private test."

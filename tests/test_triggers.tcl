puts "Start trigger test."

# This handler will be called by dserv when values "test/foo" and "test/bar" are set.
proc test_handler { name data } {
    # Confirm we see expected values at the time of set and at the time of handling.
    # TODO: there's an unexpeted, non-printing character at the end of $name.
    # shell ignores this but ctest treats this like newline, breaking regex tests.
    puts "Handle [string trim $name]: event = $data, latest = [dservGet $name]"
}

triggerRemoveAll
dservSet test/foo 0
dservSet test/bar 0

# Bind our handler script to the value names we chose, to alert on every set (every 1 call).
# TODO: can we register a handler with a wildcard?
# triggerAdd test/* 1 test_handler
triggerAdd test/foo 1 test_handler
triggerAdd test/bar 1 test_handler

# Expect sets 42 43 to trigger test_handler.
dservSet test/foo 42
dservSet test/bar 43

# Don't expect set 44 to trigger test_handler.
# However, the 44 will be visible to test_handler via dservGet.
triggerRemove test/bar
dservSet test/bar 44

puts "End trigger test."

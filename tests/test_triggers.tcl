puts "Start trigger test."

# This handler will be called by dserv when values "test/foo" and "test/bar" are set.
proc test_handler { name data } {
    # Print the value that was set as well as the current value at handling time.
    puts "Handle $name: event = $data, latest = [dservGet $name]"
}

triggerRemoveAll
dservSet test/foo 0
dservSet test/bar 0

# Bind our handler script to the value name test/foo and test/bar, to alert on "every 1" call ie always.
# TODO: something amiss when registering handler with wildcard, as in triggerAdd test/* 1 test_handler
triggerAdd test/foo 1 test_handler
triggerAdd test/bar 1 test_handler

# Expect sets 42 and 43 to trigger our test_handler.
dservSet test/foo 42
dservSet test/bar 43

# Don't expect set 44 to trigger test_handler.
# However, the 44 will be visible to test_handler via dservGet.
triggerRemove test/bar
dservSet test/bar 44

puts "End trigger test."

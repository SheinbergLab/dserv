puts "Start trigger test."

# This handler will be called by dserv when values "test/foo" and "test/bar" are set.
proc test_handler { name data } {
    # Print the value that was set as well as the current value at handling time.
    puts "Handle $name: event = $data, latest = [dservGet $name]"
}

# Don't expect our handler to be triggered when initializing.
triggerRemoveAll
dservSet test/foo 0
dservSet test/bar 0

# Register our handler script to match datapoint names like test/foo and test/bar.
# Register to alert on "every 1" change -- ie always.
triggerAdd test/* 1 test_handler

# Expect sets 42 and 43 to trigger our test_handler.
dservSet test/foo 42
dservSet test/bar 43

# Don't expect set 44 to trigger test_handler.
# However, the value 44 will be visible to test_handler via dservGet.
triggerRemove test/*
dservSet test/bar 44

puts "End trigger test."

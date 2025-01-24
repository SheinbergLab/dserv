puts "Start logger test."

# Create a log file, or truncate if it already exists.
set logFile dserv.log
dservLoggerOpen $logFile 1

# Only write events matching pattern "test/*".
dservLoggerAddMatch $logFile test/*

# Set datapoints with fixed, known timestamps and integer datatype (5).
# But this datapoint from before the logger is started should be ignored.
dservSetData test/foo 65 5 65

# Datapoints and logging commands flow through distinct queues and threads.
# To test commands in order, wait several ms between them.
set pause_ms 10

# Log subsequent test/* events to file.
after $pause_ms
dservLoggerStart $logFile
after $pause_ms
dservSetData test/foo 66 5 66

# Ignore events while logger is paused.
after $pause_ms
dservLoggerPause $logFile
after $pause_ms
dservSetData test/foo 67 5 67

# Log subsequent test/* events, again.
after $pause_ms
dservLoggerResume $logFile
after $pause_ms
dservSetData test/foo 68 5 68

# Ignore events that don't match the match pattern test/*.
after $pause_ms
dservSetData ignore/bar 69 5 69

# Ignore events after logger is closed.
after $pause_ms
dservLoggerClose $logFile
after $pause_ms
dservSetData test/foo 70 5 70

after $pause_ms
puts "End logger test."

# Load the Thread package
package require Thread

# Spawn threads and assign tasks
set threadCount 10
set threads {}
set results {}

for {set i 0} {$i < $threadCount} {incr i} {
    # Create a new thread
    set tid [thread::create {
	# Define a worker function for threads
	proc solveProblem {data} {
	    # Simulate processing (replace with actual logic)
	    return "Processed: $data"
	}
    }]
    puts "thread $tid created"
    lappend threads $tid

    # Send a task to the thread (example: pass data)
    thread::send -async $tid [list solveProblem "Task $i"] cb
}

# Callback function to handle results
proc cb {tid result} {
    global results
    set results($tid) $result
}

# Wait for all threads to complete
foreach tid $threads {
    thread::wait $tid
}

# Output results
foreach tid $threads {
    puts "Thread $tid Result: $results($tid)"
}

# Release threads
foreach tid $threads {
    thread::release $tid
}

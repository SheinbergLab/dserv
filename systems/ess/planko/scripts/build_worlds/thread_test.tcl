package require Thread

tsv::set result thread1 ""
tsv::set result thread2 ""

# Create threads
set thread1 [thread::create {thread::wait}]
set thread2 [thread::create {thread::wait}]

# Send commands asynchronously with a callback
thread::send -async $thread1 {
    proc thread_proc {thread_id main_thread} {
	after 1000
	set cmd [list set result_${thread_id} "Result from $thread_id"]
	tsv::set result ${thread_id} "Result from $thread_id"
    }
    thread_proc thread1 [thread::id]
}

thread::send -async $thread2 {
    proc thread_proc {thread_id main_thread} {
	after 1000
	set cmd [list set result_${thread_id} "Result from $thread_id"]
	tsv::set result $thread_id "Result from $thread_id"
    }
    thread_proc thread2 [thread::id]
}

# Wait for a while to allow threads to complete
after 2000

# Collect results from variables
puts [tsv::get result thread1]
puts [tsv::get result thread2]

# Release and join threads
thread::release $thread1
thread::release $thread2

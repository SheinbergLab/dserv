#
# mt_jitter.tcl
#

# Load the Thread package
package require Thread

set dlshlib [file join /usr/local/dlsh dlsh.zip]
set base [file join [zipfs root] dlsh]
if { ![file exists $base] && [file exists $dlshlib] } {
    zipfs mount $dlshlib $base
}
set ::auto_path [linsert $::auto_path [set auto_path 0] $base/lib]

package require dlsh

set num_threads 6
proc mt_jitter { n_jitter } {

    # Create multiple threads
    set threads {}

    tsv::set result count 0

    # called by each worker thread when it has finished
    # and sets the global done if all are complete
    # the main thread is waiting on this variable
    proc check_if_done {} {
	if { [tsv::get result count] == $::num_threads } {
	    set ::done 1
	}
    }

    for {set i 1} {$i <= $::num_threads} {incr i} {
	# Create a new thread
	set tid [thread::create {
	    source jitter_worlds.tcl

	    proc do_work { njitter from to } {
		
		# Load boards
		set worlds [dg_read worlds]
		set g [world_jitter $worlds $njitter $from $to]
		dg_delete $worlds
		
		# Add result to shared memory 
		set tid [thread::id]
		dg_toString $g result_$tid
		puts "thread $tid done"
		tsv::set result $tid [set result_$tid]
		tsv::incr result count
		upvar 1 main_tid main_tid
		thread::send $main_tid check_if_done
	    }
	    
	    # Get the thread ID
	    set tid [thread::id]

	    # now wait to do work
	    thread::wait
	}]
	
	lappend threads $tid
	tsv::set result $tid {}
    }
    
    # Start work in each thread and collect results
    set all_results [dict create]
    
    # Start all threads and collect results synchronously
    set thread_index -1
    foreach tid $threads {
	# Each thread does different work (number of iterations and delay)
	incr thread_index
	set n_per_thread 20
	set from [expr $thread_index*$n_per_thread]
	set to [expr $from+$n_per_thread]

	thread::send $tid [list set main_tid [thread::id]]
	
	# Execute the do_work proc in the thread and wait for the result
	thread::send -async $tid [list do_work $n_jitter $from $to]
    }

    vwait ::done
    
    set worlds {}
    foreach tid $threads {
	set g [dg_fromString [tsv::get result $tid] $tid]
	if { $worlds == {} } {
	    set worlds $g
	} else {
	    dg_append $worlds $g
	    dg_delete $g
	}
    }
    
    # Release the threads
    foreach tid $threads {
	thread::release $tid
    }
    
    return $worlds
}

set jitters [mt_jitter 250]
dg_rename $jitters jitters
dg_write jitters

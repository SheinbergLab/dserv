#
# build_worlds.tcl
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
proc create_worlds {} {

    # Create multiple threads
    set threads {}
    set nboards_per_thread 20
    set nplanks 10

    set t_args {}
    for { set t 0 } { $t < $::num_threads } { incr t } {
	for { set nhit 1 } { $nhit < 6 } { incr nhit } {
	    lappend t_args [list $nboards_per_thread $nplanks $nhit]
	}
    }

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
	    set dlshlib [file join /usr/local/dlsh dlsh.zip]
	    set base [file join [zipfs root] dlsh]
	    if { ![file exists $base] && [file exists $dlshlib] } {
		zipfs mount $dlshlib $base
	    }
	    set ::auto_path [linsert $::auto_path [set auto_path 0] $base/lib]
	    
	    tcl::tm::path add .
	    package require box2d
	    package require planko
	    
	    namespace eval planko {
		proc accept_board { g } {
		    variable params
		    set x [dl_last $g:x]
		    set y [dl_last $g:y]
		    set contacts [dl_tcllist $g:contact_bodies]
		    
		    # assumes both catchers are at same y value
		    set catcher_y $params(lcatcher_y)
		    
		    set upper [expr $catcher_y+0.01]
		    set lower [expr $catcher_y-0.01]
		    
		    if { [expr {$y < $upper && $y > $lower}] } {
			set result [expr {$x>0}]
		    } else {
			return "-1 0"
		    }
		    
		    set planks [lmap c $contacts \
				    { expr { [isPlank $c] ? [lindex [lindex $c 0] 0] : [continue] } }]
		    set planks [uniqueList $planks]
		    set nhit [llength $planks]
		    if { $nhit != $params(hitplanks) } { return -1 }
		    
		    return "$result $nhit"
		}
	    }

	    proc do_work { nboards nplanks nhit } {

		# Generate the boards
		set g [planko::generate_worlds $nboards [list nplanks $nplanks hitplanks $nhit]]

		# Add result to shared memory 
		set tid [thread::id]
		dg_toString $g result_$tid
		puts "thread $tid done"
		tsv::set result $tid [set result_$tid]
		tsv::incr result count
		upvar 1 main_tid main_tid
		thread::send $main_tid check_if_done
	    }
	    
	    set planko::params(hitplanks) 1
	    
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
	lassign [lindex $t_args [incr thread_index]] nboards nplanks nhit

	thread::send $tid [list set main_tid [thread::id]]
	
	# Execute the do_work proc in the thread and wait for the result
	thread::send -async $tid [list do_work $nboards $nplanks $nhit]
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

set worlds [create_worlds]
dg_rename $worlds worlds
dg_write worlds

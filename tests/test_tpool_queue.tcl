# test_tpool_queue.tcl -- end-to-end test of tpool_queue (TpoolMap.cpp),
# the pull-based worker pool, and a regression check on tpool_map, which
# shares its interpreter setup.
#
# Run by ctest as: dserv -c tests/test_tpool_queue.tcl. The script
# self-asserts and prints ONE verdict line (listener bind-retry noise from
# another dserv on the fixed ports interleaves with output).

puts "Start tpool_queue test."

set failures {}
proc assert {ok what} {
    if {$ok} { puts "  ok: $what" } else {
        puts "  FAIL: $what"
        lappend ::failures $what
    }
}
proc lseq0 {n} { set l {}; for {set i 0} {$i < $n} {incr i} { lappend l $i }; return $l }

# every task is done exactly once, by index
set r [tpool_queue [lseq0 40] {} {expr {$task*$task}} -threads 4]
assert [expr {[dict get $r made] == 40 && [dict get $r tried] == 40}] "40 tasks, 40 results"
set ok 1; set seen {}
foreach pair [dict get $r results] {
    lassign $pair i v
    lappend seen $i
    if { $v != $i*$i } { set ok 0 }
}
assert $ok "each result belongs to its task index"
assert [expr {[lsort -integer $seen] eq [lseq0 40]}] "no task skipped or repeated"
assert [expr {[llength [dict get $r errors]] == 0}] "no errors"
assert [expr {[dict get $r n_threads] == 4}] "four threads"

# the task value arrives intact (a list with spaces and braces), and
# -args reaches the work script
set tasks [list {a b} {c {d e}} "x\ty"]
set r [tpool_queue $tasks {} {return "[dict get $args_dict tag]:[llength $task]:$task"} -threads 2 -args {tag T}]
array unset got
foreach pair [dict get $r results] { lassign $pair i v; set got($i) $v }
assert [expr {$got(0) eq "T:2:a b" && $got(1) eq "T:2:c {d e}" && $got(2) eq "T:2:x\ty"}] "task values and -args intact"

# an empty result is "nothing made", not an error, and is not returned
set r [tpool_queue [lseq0 30] {} {if { $task % 3 } { return "" }; return $task} -threads 3]
assert [expr {[dict get $r made] == 10 && [dict get $r tried] == 30}] "empty results not counted (10 of 30)"
assert [expr {[llength [dict get $r errors]] == 0}] "...and not errors"

# -want: stop when the quota is met, with exactly that many results
set r [tpool_queue [lseq0 2000] {} {after 2; return $task} -threads 4 -want 25]
assert [expr {[dict get $r made] == 25 && [llength [dict get $r results]] == 25}] "quota: exactly 25 results"
assert [expr {[dict get $r tried] < 2000}] "quota: stopped early ([dict get $r tried] of 2000 tried)"

# a task that raises is recorded and the worker carries on
set r [tpool_queue [lseq0 12] {} {if { $task == 3 } { error boom }; return $task} -threads 2]
assert [expr {[dict get $r made] == 11}] "a failing task does not stop the rest"
assert [string match "*task 3: boom*" [dict get $r errors]] "the failure names its task"

# workers keep pulling: with slow tasks all four take a fair share
set r [tpool_queue [lseq0 40] {} {after 25; return $worker_id} -threads 4]
array unset n
foreach pair [dict get $r results] { incr n([lindex $pair 1]) }
set least 40
foreach w [array names n] { if { $n($w) < $least } { set least $n($w) } }
assert [expr {[array size n] == 4 && $least >= 5}] "all four workers busy throughout (least did $least of 40)"

# state set up once per worker persists across its tasks
set r [tpool_queue [lseq0 20] {set ::count 0} {incr ::count; return $::count} -threads 2]
set mx 0
foreach pair [dict get $r results] { if { [lindex $pair 1] > $mx } { set mx [lindex $pair 1] } }
assert [expr {$mx > 1}] "setup runs once per worker, not per task (a worker counted to $mx)"

# binary results survive (a NUL in the middle)
set r [tpool_queue {x} {} {binary format a2ca2 ab 0 cd} -threads 1]
assert [expr {[string length [lindex [dict get $r results] 0 1]] == 5}] "binary result intact"

# a setup that fails is reported per worker and nothing is made
set r [tpool_queue [lseq0 6] {package require no_such_package_xyz} {return $task} -threads 2]
assert [expr {[dict get $r made] == 0 && [llength [dict get $r errors]] == 2}] "setup failure reported by each worker"

# no tasks
set r [tpool_queue {} {} {return 1}]
assert [expr {[dict get $r made] == 0 && [dict get $r tried] == 0}] "empty task list"

# tpool_map is unchanged: an even split, one result per worker
set r [tpool_map 8 {} {return $n} -threads 4]
assert [expr {[dict get $r results] eq {2 2 2 2} && [dict get $r missing] == 0}] "tpool_map still splits evenly"
set r [tpool_map 4 {package require no_such_package_xyz} {return $n} -threads 2]
assert [expr {[dict get $r missing] == 4 && [llength [dict get $r errors]] == 2}] "tpool_map still reports setup failure"

if {[llength $failures] == 0} {
    puts "TPOOL QUEUE TEST: ALL PASS"
} else {
    puts "TPOOL QUEUE TEST: [llength $failures] FAILURE(S): $failures"
}
shutdown

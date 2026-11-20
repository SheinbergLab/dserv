# test_photopair.tcl -- tools/photopair/photopair.tcl's pairing, on
# records made up here (a real file needs dlsh and is tried by hand: see
# docs/photo_processor.md, "Pairing edges with frames").
#
#   tclsh tests/test_photopair.tcl
#
# Covers: the logged edge rows (two in one datapoint), the photopatch marks
# and their directions, the dserv-wall offset (logged, given, estimated
# from arrivals), cmd marks from stim/cmd JSON, and the pairing itself on a
# 120 Hz toggle whose latency is longer than a refresh, with a dropped
# edge, a spurious one, a direction that does not match, and a mark with no
# edge.

set here [file dirname [file normalize [info script]]]
source [file join [file dirname $here] tools photopair photopair.tcl]

set fails 0
proc check { got want what } {
    if { $got eq $want } { puts "  ok: $what" } else {
        puts "  FAIL: $what: got {$got} want {$want}"
        incr ::fails
    }
}
proc near { a b tol } { expr {abs($a - $b) <= $tol} }

# an edge row as the processor sends it: dir t50 t10 t90 old new settled seq t_arm t_done
proc erow { dir t50 seq } {
    list $dir $t50 [expr {$t50 - 300}] [expr {$t50 + 400}] 0 1000 1 $seq $t50 [expr {$t50 + 5000}]
}

puts "logged edges:"
set recs [dict create proc/photo/edge [list \
    [list 2000 [concat [erow -1 2000 2] [erow 1 1500 1]]] \
    [list 9000 [erow 1 9000 3]]]]
set E [photopair::edges_logged $recs]
check [llength $E] 3 "two rows in one datapoint are two edges"
check [lmap e $E { lindex $e 1 }] {1500 2000 9000} "sorted by t50"
check [lindex $E 0] {1 1500 1200 1900 0 1000 1 1} "dir t50 t10 t90 old new settled seq"

puts "offset:"
set off 37000000
set wall0 1795000000000000
set recs [dict create stim/frame [list \
    [list [expr {$wall0 + $off + 900}] [list 1 10 $wall0 0]] \
    [list [expr {$wall0 + 8333 + $off + 250}] [list 2 11 [expr {$wall0 + 8333}] 0]]]]
check [photopair::wall_offset $recs ""] [list [expr {$off + 250}] estimated] \
    "no logged offset: the least arrival - flip"
dict set recs ess/dserv_wall_us [list [list 0 $off]]
check [photopair::wall_offset $recs ""] [list $off logged] "the logged one first"
check [photopair::wall_offset $recs 5] {5 given} "-offset wins"
check [photopair::wall_offset [dict create] ""] {0 none} "nothing to go on"

puts "photopatch marks:"
# swapcount flip_wall level serial ntags why
set recs [dict create stim/photopatch [list \
    [list 0 [list 10 1000 0 1 0 1]] \
    [list 0 [list 11 2000 1 2 1 2]] \
    [list 0 [list 12 3000 0 3 1 2]] \
    [list 0 [list 13 -1 1 4 0 4]]]]
set M [photopair::marks_photopatch $recs 100]
check [lmap m $M { lrange $m 0 1 }] {{1100 0} {2100 1} {3100 -1}} \
    "on the dserv clock; light rises, dark falls, the patch appearing either; an unknown flip dropped"

puts "cmd marks:"
set recs [dict create stim/cmd [list \
    [list 0 [list {{"seq":1,"cmd":"!sample_on","flip":5000,"status":"ok"}}]] \
    [list 0 [list {{"seq":2,"cmd":"nexttrial 3","flip":null,"status":"ok"}}]] \
    [list 0 [list {{"seq":3,"cmd":"!sample_off","flip":7000,"status":"ok"}}]]]]
check [photopair::marks_cmd $recs !sample_* 10 0] {{5010 0 !sample_on} {7010 0 !sample_off}} \
    "matching commands at their flip; none without one"

puts "pairing a 120 Hz toggle, latency 11 ms (longer than a refresh):"
set P 8333.3
set marks {}
set edges {}
set seq 0
for { set i 0 } { $i < 40 } { incr i } {
    set t [expr {100000 + $i * $P}]
    set dir [expr {$i % 2 ? -1 : 1}]
    lappend marks [list $t $dir "m$i"]
    if { $i == 7 } continue                         ;# an edge the detector missed
    set jit [expr {($i % 5 - 2) * 40}]              ;# +-80 us
    lappend edges [lrange [erow $dir [expr {$t + 11000 + $jit}] [incr seq]] 0 7]
}
lappend edges [lrange [erow 1 [expr {100000 + 20.5 * $P}] 99] 0 7]   ;# spurious, between
set edges [lsort -real -index 1 $edges]
lassign [photopair::pair $marks $edges 100 4] L rows
check [near $L 11000 100] 1 "typical latency 11 ms ([expr {$L / 1000.0}])"
set paired [lmap r $rows { if { [lindex $r 1] < 0 } continue ; set r }]
check [llength $paired] 39 "39 of 40 paired"
check [lindex $rows 7 1] -1 "the mark whose edge was missed: none (not its neighbour's)"
set bad 0
foreach r $paired {
    if { ![near [lindex $r 3] 11000 80] } { incr bad }
    if { [lindex $edges [lindex $r 1] 0] != [lindex $r 0 1] } { incr bad }
}
check $bad 0 "every pair 11 ms +-80 us, direction matched"
check [lsearch -exact [lmap r $paired { lindex $edges [lindex $r 1] 7 }] 99] -1 "the spurious edge unpaired"
lassign [photopair::stats [lmap r $paired { lindex $r 3 }]] n med
check [list $n [near $med 11.0 0.1]] {39 1} "stats in ms"

puts "marks with no direction, rise 8.1 ms and fall 3.4 ms (an LCD):"
# cmd:/evt: marks: a patch toggling on each, so their edges alternate
set marks {}
set edges {}
set t 100000
for { set i 0 } { $i < 36 } { incr i } {
    set t [expr {$t + 200000 + ($i % 7) * 37000}]
    set dir [expr {$i % 2 ? -1 : 1}]
    lappend marks [list $t 0 "m$i"]
    set jit [expr {($i % 5 - 2) * 30}]
    set lat [expr {$dir > 0 ? 8100 : 3400}]
    lappend edges [lrange [erow $dir [expr {$t + $lat + $jit}] $i] 0 7]
}
set edges [lsort -real -index 1 $edges]
lassign [photopair::pair $marks $edges 100 4] L rows Ldir
check [list [near [dict get $Ldir 1] 8100 70] [near [dict get $Ldir -1] 3400 70]] {1 1} \
    "a latency for each direction ([dict get $Ldir 1], [dict get $Ldir -1])"
set paired [lmap r $rows { if { [lindex $r 1] < 0 } continue ; set r }]
check [llength $paired] 36 "all 36 paired (one L between them would leave half outside the window)"
set bad 0
foreach r $paired {
    set want [expr {[lindex $edges [lindex $r 1] 0] > 0 ? 8100 : 3400}]
    if { ![near [lindex $r 3] $want 70] } { incr bad }
}
check $bad 0 "each by its own direction's latency"

puts "a direction with too few first-pass edges uses the overall latency:"
# every edge rising at 8.1 ms, but one stray falling edge 2 ms after a mark
# comes first for it
set edges2 {}
foreach m $marks {
    lappend edges2 [lrange [erow 1 [expr {[lindex $m 0] + 8100}] 0] 0 7]
}
lappend edges2 [lrange [erow -1 [expr {[lindex $marks 4 0] + 2000}] 98] 0 7]
set edges2 [lsort -real -index 1 $edges2]
lassign [photopair::pair $marks $edges2 100 4] L rows Ldir
check [dict get $Ldir -1] $L "one falling latency is not a direction's own"
set stray [lsearch -exact -index 7 -real $edges2 98]
check [lsearch -exact -index 1 $rows $stray] -1 "the stray falling edge unpaired"
check [llength [lmap r $rows { if { [lindex $r 1] < 0 } continue ; set r }]] 36 "every mark has its rise"

puts "nothing within -max:"
lassign [photopair::pair {{0 1 a}} [list [lrange [erow 1 500000 1] 0 7]] 100 4] L rows
check [list $L [lindex $rows 0 1]] {{} -1} "no latency, no pair"

puts "a direction no edge has:"
lassign [photopair::pair {{0 -1 a}} [list [lrange [erow 1 11000 1] 0 7]] 100 4] L rows
check $L {} "a falling mark is not paired with a rising edge"

if { $fails } {
    puts "$fails FAILURE(S)"
    exit 1
}
puts "all checks passed"

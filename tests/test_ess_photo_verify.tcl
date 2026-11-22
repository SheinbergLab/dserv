# test_ess_photo_verify.tcl -- each photodiode patch change paired with
# its light, online (lib/ess-2.0.tm, the photo_verify block; docs/
# photo_processor.md, "Checking a system").
#
#   /usr/local/bin/dlsh tests/test_ess_photo_verify.tcl
#
# The block is read out of ess-2.0.tm and fed records the way stim2
# (stim/photopatch), the photo processor (proc/photo/edge) and the
# stim_cmd_log hook would, on a clock the test sets. dservSetData is a
# stand-in that keeps what was published.

set here [file dirname [file normalize [info script]]]
set root [file dirname $here]

set fails 0
proc check { got want what } {
    if { $got eq $want } { puts "  ok: $what" } else {
        puts "  FAIL: $what: got {$got} want {$want}"
        incr ::fails
    }
}
proc slurp { f } { set fd [open $f] ; set s [read $fd] ; close $fd ; return $s }

set ::clk 0
proc now {} { return $::clk }
set ::out {}
proc dservSetData { name ts type data } { lappend ::out [list $name $ts $data] }

set src [slurp [file join $root lib ess-2.0.tm]]
set b [string first "    # BEGIN photo_verify\n" $src]
set e [string first "    # END photo_verify\n" $src]
if { $b < 0 || $e < 0 } { error "no photo_verify block in ess-2.0.tm" }
namespace eval ::ess [string range $src $b $e]

# a JSON field, enough for these flat records
proc jget { j k } {
    if { [regexp "\"$k\":(\"\[^\"\]*\"|\\\[\[^\\\]\]*\\\]|\\\{\[^\\\}\]*\\\}|\[^,\\\}\]+)" $j -> v] } {
        return [string trim $v \"]
    }
    return ""
}
proc pairs {} { lmap r $::out { if { [lindex $r 0] ne "photo/pair" } continue ; lindex $r 2 } }
proc statuses {} { lmap j [pairs] { jget $j status } }

# stim2 at swap s, flip at wall w (us), changing the patch to level
set ::off 1000000000          ;# dserv clock = wall + off
set ::serial 0
proc change { swap wall level {why 2} {nacks 1} } {
    ::ess::pv_change [list $swap $wall $level [incr ::serial] 0 $why $nacks] [expr {$wall + $::off + 500}]
}
# an edge at t50 (dserv us), t10 a ms before it
set ::eseq 0
proc edge { dir t50 } {
    ::ess::pv_edge [list $dir $t50 [expr {$t50 - 1000}] [expr {$t50 + 1000}] 0 0 1 [incr ::eseq] 0 0]
}
# a session: every 100 ms a '!' command flips the patch; the light rises
# 8 ms after a rising change, falls 3.4 ms after a falling one
set rise_us 8000
set fall_us 3400
proc run { n {skip {}} {t0 1000000} {swap0 100} } {
    set level 0
    for { set i 0 } { $i < $n } { incr i } {
        set level [expr {!$level}]
        set wall [expr {$t0 + $i * 100000}]
        set swap [expr {$swap0 + $i * 6}]
        set t [expr {$wall + $::off}]
        change $swap $wall $level
        ::ess::pv_cmd [expr {$swap * 10}] "!flip $i" $swap [expr {$t + 1200}]
        if { $i ni $skip } {
            edge [expr {$level ? 1 : -1}] [expr {$t + ($level ? $::rise_us : $::fall_us)}]
        }
        set ::clk [expr {$t + 50000}]
        ::ess::pv_resolve $::clk
    }
}

puts "a clean session:"
::ess::pv_start marked $::off
run 20
set ::clk [expr {$::clk + 1000000}]
::ess::pv_resolve $::clk
check [lsort -unique [statuses]] lit "every change lit"
check [dict get $::ess::pv_n changes] 20 "20 changes"
check [dict get $::ess::pv_n cmds] 20 "20 '!' commands"
check [dict get $::ess::pv_n cmds_lit] 20 "  every one on a lit change"
check [::ess::pv_verdict] pass "verdict pass"
set j [lindex [pairs] 1]
check [list [jget $j status] [jget $j level] [jget $j lat50] [jget $j lat10] [jget $j seqs]] \
    {lit 0 3400 2400 {[1060]}} "a falling change: lat50 3.4 ms, lat10 2.4, its command's seq"
set j [lindex [pairs] 2]
check [list [jget $j lat50] [jget $j nacks] [jget $j edge]] {8000 1 3} "a rising one: 8 ms, its edge's seq"
check [lindex $::out 0 1] [expr {$::off + 1000000}] "photo/pair is stamped at the flip"
set v [::ess::pv_json]
check [list [jget $v verdict] [jget $v lit] [jget $v dark] [jget $v extra] [jget $v pending]] \
    {pass 20 0 0 0} "photo/verify: pass, 20 lit"
check [regexp {"rise":\{"n":10,"median":8.000,"sd":0.000,"min":8.000,"max":8.000\}} $v] 1 \
    "  rise: 10 at 8 ms"
check [regexp {"fall":\{"n":10,"median":3.400,} $v] 1 "  fall: 10 at 3.4 ms"
check [::ess::pv_summary] "pass: 20 of 20 changes lit; rise 8.000 ms (sd 0.000, max 8.000); fall 3.400 ms (sd 0.000, max 3.400)" \
    "the summary line"

puts "a change is judged only when nothing more can come for it:"
::ess::pv_start marked $::off
set ::out {}
change 1 1 1
set ::clk [expr {$::off + 200000}]
::ess::pv_resolve $::clk
check [llength [pairs]] 0 "200 ms on: not yet"
check [jget [::ess::pv_json] pending] 1 "  pending 1"
edge 1 [expr {$::off + 8000}]      ;# late in arriving: blocks are batched
set ::clk [expr {$::off + 301000}]
::ess::pv_resolve $::clk
check [statuses] lit "301 ms on: lit, by an edge that arrived late"

puts "an edge that arrives before its change's record:"
::ess::pv_start marked $::off
set ::out {}
edge -1 [expr {$::off + 3400}]
change 1 1 0
set ::clk [expr {$::off + 400000}]
::ess::pv_resolve $::clk
check [statuses] lit "lit"
check [dict get $::ess::pv_n extra] 0 "  no extra edge"

puts "missed frames:"
::ess::pv_start marked $::off
set ::out {}
run 20 {7 12}
set ::clk [expr {$::clk + 1000000}]
::ess::pv_resolve $::clk
check [lindex [statuses] 7] dark "the 8th change gave no light: dark"
check [lindex [statuses] 8] lit "  the next is still lit"
check [list [dict get $::ess::pv_n lit] [dict get $::ess::pv_n dark] [dict get $::ess::pv_n cmds_dark]] {18 2 2} \
    "18 lit, 2 dark, their 2 commands dark"
check [::ess::pv_verdict] fail "verdict fail"
check [string match "fail: 18 of 20 changes lit, 2 dark;*" [::ess::pv_summary]] 1 "  said so"

puts "a frame the panel never showed, on an LCD at 60 Hz:"
# up (not shown), down (so no edge either), up: only the third's light
::ess::pv_start marked $::off
set ::out {}
run 10                                         ;# learn 8 / 3.4 ms
set ::clk [expr {$::clk + 1000000}]
::ess::pv_resolve $::clk
set ::out {}
set t [expr {3000000}]
change 500 $t 1
change 501 [expr {$t + 16667}] 0
change 502 [expr {$t + 33333}] 1
edge 1 [expr {$t + 33333 + $::off + 8000}]
set ::clk [expr {$t + $::off + 2000000}]
::ess::pv_resolve $::clk
check [statuses] {dark dark lit} "the first rise is not given the third's light"
check [dict get $::ess::pv_n extra] 0 "  and no edge is left over"

puts "a light at another latency: late"
::ess::pv_start marked $::off
set ::out {}
run 10                                         ;# learn 8 / 3.4 ms
set ::clk [expr {$::clk + 1000000}]
::ess::pv_resolve $::clk
set ::out {}
set t 4000000
change 600 $t 1
::ess::pv_cmd 6000 "!late" 600 [expr {$t + $::off + 1200}]
edge 1 [expr {$t + $::off + 28000}]              ;# 20 ms later than its 8
set ::clk [expr {$t + $::off + 1000000}]
::ess::pv_resolve $::clk
set j [lindex [pairs] 0]
check [list [jget $j status] [jget $j lat50] [jget $j expect] [jget $j light]] \
    [list late 28000 8000 [expr {$t + $::off + 28000}]] "late: its light, its latency and the expected one"
check [list [dict get $::ess::pv_n late] [dict get $::ess::pv_n extra] [dict get $::ess::pv_n cmds_late]] {1 0 1} \
    "  counted late, the edge not extra, its command late"
check [::ess::pv_verdict] fail "  verdict fail"
check [string match "fail: 10 of 11 changes lit, 1 late;*" [::ess::pv_summary]] 1 "  said so"

puts "the latency steps up 20 ms mid-session:"
::ess::pv_start marked $::off
set ::out {}
run 20                                         ;# learn 8 / 3.4 ms
set ::rise_us 28000
set ::fall_us 23400
run 12 {} 4000000 400
set ::clk [expr {$::clk + 1000000}]
::ess::pv_resolve $::clk
set ::rise_us 8000
set ::fall_us 3400
check [lrange [statuses] 20 end] {late late late late late late lit lit lit lit lit lit} \
    "three late in each direction, then lit at the new latency"
check [list [dict get $::ess::pv_n lit] [dict get $::ess::pv_n late] [dict get $::ess::pv_n dark] [dict get $::ess::pv_n extra]] \
    {26 6 0 0} "  none dark, no edge extra"

puts "an edge no change explains:"
::ess::pv_start marked $::off
set ::out {}
run 5
edge 1 [expr {$::clk + 20000}]
set ::clk [expr {$::clk + 1000000}]
::ess::pv_resolve $::clk
check [dict get $::ess::pv_n extra] 1 "one extra edge"
check [::ess::pv_verdict] fail "verdict fail"

puts "the patch appearing:"
::ess::pv_start marked $::off
set ::out {}
change 1 1 0 1
set ::clk [expr {$::off + 1000000}]
::ess::pv_resolve $::clk
check [statuses] appear "with no edge: appear, not dark"
check [list [dict get $::ess::pv_n changes] [dict get $::ess::pv_n dark] [::ess::pv_verdict]] {0 0 none} \
    "  not a change: verdict none"
::ess::pv_start marked $::off
set ::out {}
change 1 1 1 1
edge 1 [expr {$::off + 8000}]
set ::clk [expr {$::off + 1000000}]
::ess::pv_resolve $::clk
check [list [statuses] [dict get $::ess::pv_n extra]] {appear 0} "with one: appear, the edge taken"

puts "'!' commands:"
::ess::pv_start marked $::off
set ::out {}
run 3
::ess::pv_cmd 9001 "!redraw" 77 [expr {$::clk + 1000}]     ;# no change on swap 77
::ess::pv_cmd 9002 "set x 1" 78 [expr {$::clk + 1000}]     ;# not a '!' command
set ::clk [expr {$::clk + 1000000}]
::ess::pv_resolve $::clk
check [list [dict get $::ess::pv_n cmds] [dict get $::ess::pv_n unmarked]] {4 1} \
    "a '!' on a swap with no change: unmarked; a plain one ignored"
check [::ess::pv_verdict] fail "  marked mode: fail"
::ess::pv_start manual $::off
run 3
::ess::pv_cmd 9001 "!redraw" 77 [expr {$::clk + 1000}]
set ::clk [expr {$::clk + 1000000}]
::ess::pv_resolve $::clk
check [list [dict get $::ess::pv_n unmarked] [::ess::pv_verdict]] {0 pass} "  manual mode: not counted"
::ess::pv_start marked $::off
set ::out {}
change 40 1 1 2 2
::ess::pv_cmd 401 "!a" 40 [expr {$::off + 900}]
::ess::pv_cmd 402 "!b" 40 [expr {$::off + 950}]
edge 1 [expr {$::off + 8000}]
set ::clk [expr {$::off + 1000000}]
::ess::pv_resolve $::clk
check [list [jget [lindex [pairs] 0] seqs] [dict get $::ess::pv_n cmds_lit]] {{[401,402]} 2} \
    "two '!' on one swap: one change, both its commands"

puts "a new data file:"
::ess::pv_new_count [expr {$::off + 5}]
check [list [dict get $::ess::pv_n lit] [::ess::pv_verdict] $::ess::pv_off] [list 0 none [expr {$::off + 5}]] \
    "counts from zero, the new offset"
check [expr {[llength [dict get $::ess::pv_lats 1]] > 0}] 1 "  the latencies learned are kept"

puts "off:"
::ess::pv_stop
set ::out {}
::ess::pv_change_dp stim/photopatch {1 0 1 1 0 2 1}
::ess::pv_edge_dp proc/photo/edge {1 8000 7000 9000 0 0 1 1 0 0}
check [list [llength $::ess::pv_changes] [llength $::ess::pv_edges]] {0 0} "records ignored"

puts [expr {$fails ? "$fails FAILED" : "all checks passed"}]
exit [expr {$fails > 0}]

#
# test_df_display_times.tcl
#
#  df::add_display_times (lib/df-2.0.tm): the step after every system's
#  extractor that adds, for each event ESS linked to a '!' command
#  (ess/evt_display), the frame's flip and the photodiode's light as
#  <event>_flip / <event>_light, ms from obs onset, and display_ok.
#
#  On a SYNTHETIC obs DG shaped like readESS output. The lights
#  (photo/pair) are judged 300 ms after their flip, so one here sits in the
#  NEXT obs and one between obs (<session>): they are joined by swap.
#
#  Needs dlsh: `dlsh tests/test_df_display_times.tcl`.
#
set repo [file normalize [file join [file dirname [info script]] ..]]
tcl::tm::path add $repo/lib
package require df 2.0

set n 0; set fails 0
proc check {what got want} {
    incr ::n
    if {$got ne $want} { incr ::fails; puts "FAIL $what\n  got:  $got\n  want: $want" } else { puts "ok   $what" }
}

proc ev {type sub t seq swap flip} {
    return "{\"type\":\"$type\",\"subtype\":\"$sub\",\"t\":$t,\"seq\":$seq,\"swap\":$swap,\"flip\":$flip}"
}
proc pair {swap status {light ""}} {
    set j "{\"serial\":1,\"swap\":$swap,\"level\":1,\"why\":2,\"nacks\":1,\"seqs\":\[1\],\"status\":\"$status\""
    if {$light ne ""} { append j ",\"edge\":3,\"lat10\":1,\"lat50\":2,\"light\":$light" }
    return "$j}"
}

proc make_obs {{with_pairs 1}} {
    set g [dg_create]
    # obs 0: SAMPLE ON at 100 ms (flip 2 ms before), CHOICES ON at 500 ms
    # obs 1: SAMPLE ON (its frame dark), CHOICES ON (no patch change on its
    #        swap), SAMPLE ON again
    # obs 2: SAMPLE ON, stim2 with no flip time
    dl_set $g:<ds>ess/evt_display [dl_llist \
        [dl_slist [ev SAMPLE ON 5000000 1 10 4998000] [ev CHOICES ON 5400000 2 20 5398500]] \
        [dl_slist [ev SAMPLE ON 8000000 3 30 7999000] [ev CHOICES ON 8250000 4 40 8249000] \
                  [ev SAMPLE ON 8350000 5 31 8349000]] \
        [dl_slist [ev SAMPLE ON 9000000 6 50 null]]]
    dl_set $g:<dst>ess/evt_display [dl_llist \
        [dl_dlist 100.0 500.0] [dl_dlist 50.0 300.0 400.0] [dl_dlist 20.0]]
    if {$with_pairs} {
        dl_set $g:<ds>photo/pair [dl_llist \
            [dl_slist [pair 10 lit 5006000]] \
            [dl_slist [pair 20 lit 5405500]] \
            [dl_slist [pair 50 lit 9008000] [pair 31 lit 8358000]]]
        dl_set $g:<session>photo/pair [dl_llist [dl_slist [pair 30 dark]]]
    }
    return $g
}
proc trials {} {
    set t [dg_create]
    dl_set $t:obsid [dl_ilist 2 0 1]
    return $t
}
proc col {t c} { dl_tcllist $t:$c }

puts "with the photodiode:"
set g [make_obs]
set t [df::add_display_times [trials] $g]
check "columns" [lsort [dg_tclListnames $t]] \
    {choices_on_flip choices_on_light display_ok obsid sample_on_flip sample_on_light}
check "sample_on_flip: rows by obsid, NaN with no flip time" [col $t sample_on_flip] {NaN 98.0 49.0}
check "sample_on_light: a dark frame is NaN" [col $t sample_on_light] {28.0 106.0 NaN}
check "choices_on_flip: NaN where it did not occur" [col $t choices_on_flip] {NaN 498.5 299.0}
check "choices_on_light: joined from the next obs; unmarked is NaN" [col $t choices_on_light] {NaN 505.5 NaN}
check "display_ok: obs 1 had a dark and an unmarked frame" [col $t display_ok] {1 1 0}
dg_delete $t

puts "a late light:"
set g [make_obs]
dl_set $g:<ds>photo/pair [dl_llist \
    [dl_slist [pair 10 late 5026000]] \
    [dl_slist [pair 20 lit 5405500]] \
    [dl_slist [pair 50 lit 9008000] [pair 31 lit 8358000]]]
set t [df::add_display_times [trials] $g]
check "its light kept" [col $t sample_on_light] {28.0 126.0 NaN}
check "  display_ok 0 for its trial" [col $t display_ok] {1 0 0}
dg_delete $t
dg_delete $g
set g [make_obs]

puts "a column the extractor wrote:"
set t [trials]
dl_set $t:sample_on_light [dl_flist 1 2 3]
set t [df::add_display_times $t $g]
check "kept" [col $t sample_on_light] {1.0 2.0 3.0}
check "  the others added" [col $t sample_on_flip] {NaN 98.0 49.0}
dg_delete $t
dg_delete $g

puts "no photodiode:"
set g [make_obs 0]
set t [df::add_display_times [trials] $g]
check "flips still there" [col $t sample_on_flip] {NaN 98.0 49.0}
check "  lights NaN" [col $t sample_on_light] {NaN NaN NaN}
check "  display_ok -1: nothing to judge" [col $t display_ok] {-1 -1 -1}
dg_delete $t
dg_delete $g

puts "a file with no linked events:"
set g [dg_create]
dl_set $g:<ds>photo/pair [dl_llist [dl_slist [pair 10 lit 5006000]]]
set t [df::add_display_times [trials] $g]
check "untouched" [dg_tclListnames $t] obsid

puts "through the step every extraction path calls:"
set g [make_obs]
set t [df::_add_recorded_streams_safely [trials] $g test]
check "display columns added" [col $t display_ok] {1 1 0}

puts [expr {$fails ? "$fails of $n FAILED" : "all checks passed ($n)"}]
exit [expr {$fails > 0}]

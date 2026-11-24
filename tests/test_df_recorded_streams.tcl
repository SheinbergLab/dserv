#
# test_df_recorded_streams.tcl
#
#  df::add_recorded_streams (lib/df-2.0.tm): the step after every system's
#  extractor that adds the streams a file's decode keys name -- analog
#  groups from ess/ain/recorded, the rig's DI lines and chord groups from
#  extio/record -- so no extractor has to know about them.
#
#  Built on a SYNTHETIC obs DG shaped like readESS output: <ds>/<dst>/<dsn>
#  columns per obs, the keys as <session> columns (a list of string lists,
#  as readESS writes them -- the first version of session_key missed that
#  wrapping and found no key at all). Analog blocks are real box_ain_group.h
#  wire bytes, so the decode is the shipping one.
#
#  Checks: rows follow obsid (not obs order); per-scan times = block <dst> +
#  k*interval; one column per channel named by label (ch<N> unlabelled);
#  DI edges per obs; a column the extractor already wrote is kept; a
#  declared stream with no data gets empty rows, so trials stay
#  rectangular; a file with no keys is untouched.
#
#  Needs dlsh (dl_* commands): run as `dlsh tests/test_df_recorded_streams.tcl`.
#
set repo [file normalize [file join [file dirname [info script]] ..]]
tcl::tm::path add $repo/lib
package require df 2.0

set n 0; set fails 0
proc check {what got want} {
    incr ::n
    if {$got ne $want} { incr ::fails; puts "FAIL $what\n  got:  $got\n  want: $want" } else { puts "ok   $what" }
}

# one analog block: ver mask nchan count interval_us flags pad, int16 scans
proc block {nchan interval samples} {
    set count [expr {[llength $samples] / $nchan}]
    binary format cucucucuiusux2s* 1 0 $nchan $count $interval 0 $samples
}
# bytes -> one obs's <ds> value list (DF_CHAR)
proc bytes_list {b} {
    dl_local l [dl_create char]
    if {[string length $b]} { dl_fromString $b $l }
    return [dl_tcllist $l]
}

# ---- the synthetic obs DG: 3 obs --------------------------------------------
set g [dg_create]
set eye extio/box01/state/ain/eye
set pd  extio/box01/state/ain/pd
set di  extio/box01/state/di/3

# eye: 2 channels, 2 blocks per obs of 2 scans at 1000 us
dl_set $g:<ds>$eye  [dl_llist]
dl_set $g:<dst>$eye [dl_llist]
dl_set $g:<dsn>$eye [dl_llist]
foreach o {0 1 2} {
    set b1 [block 2 1000 [list [expr {$o*100}] 1 [expr {$o*100+2}] 3]]
    set b2 [block 2 1000 [list 4 5 6 7]]
    dl_append $g:<ds>$eye  [dl_create char {*}[bytes_list $b1$b2]]
    dl_append $g:<dst>$eye [dl_dlist [expr {$o + 0.5}] [expr {$o + 2.5}]]
    dl_append $g:<dsn>$eye [dl_ilist [string length $b1] [string length $b2]]
}
# DI line: obs 1 has no edges
dl_set $g:<ds>$di  [dl_llist [dl_ilist 1 0] [dl_ilist] [dl_ilist 1]]
dl_set $g:<dst>$di [dl_llist [dl_dlist 10.25 20.5] [dl_dlist] [dl_dlist 7.0]]
dl_set $g:<dsn>$di [dl_llist [dl_ilist 1 1] [dl_ilist] [dl_ilist 1]]

set rec [dict create want eye recorded [dict create \
    box01/eye [dict create box box01 group eye dpoint $eye chans {0 1} labels {eye_x ch1} source param] \
    box01/pd  [dict create box box01 group pd  dpoint $pd  chans {2}   labels {photodiode} source rig]] \
    declined {} missing {} ambiguous {}]
set xrec [dict create declared {tlock lever} streams [dict create \
    tlock [dict create status ok kind di box box01 dpoint $di] \
    lever [dict create status ok kind di box box01 dpoint extio/box01/state/di/9] \
    gone  [dict create status missing kind "" box "" dpoint ""]]]
foreach {k v} [list ess/ain/recorded $rec extio/record $xrec] {
    dl_set $g:<session>$k [dl_llist [dl_slist $v]]
}

# ---- trials: obs 2 then 0 (order is the extractor's), eye_x already there --
set t [dg_create]
dl_set $t:obsid [dl_ilist 2 0]
dl_set $t:eye_x [dl_slist mine mine]
df::add_recorded_streams $t $g

check "session_key unwraps" [dict get [df::session_key $g ess/ain/recorded] want] eye
check "eye_t row0 = obs 2" [dl_tcllist [dl_get $t:eye_t 0]] {2.5 3.5 4.5 5.5}
check "eye_t row1 = obs 0" [dl_tcllist [dl_get $t:eye_t 1]] {0.5 1.5 2.5 3.5}
check "eye_ch1 (unlabelled) row0" [dl_tcllist [dl_get $t:eye_ch1 0]] {1 3 5 7}
check "eye_ch1 type" [dl_datatype [dl_get $t:eye_ch1 0]] short
check "extractor's eye_x kept" [dl_tcllist $t:eye_x] {mine mine}
check "pd declared, no data: empty rows" [list [dl_length $t:pd_t] [dl_length [dl_get $t:pd_photodiode 0]]] {2 0}
check "tlock_t by obsid" [dl_tcllist [dl_get $t:tlock_t 0]] 7.0
check "tlock values row1" [dl_tcllist [dl_get $t:tlock 1]] {1 0}
check "lever: no column in file -> empty rows" [list [dl_length $t:lever] [dl_length [dl_get $t:lever_t 0]]] {2 0}
check "missing rig name adds nothing" [dl_exists $t:gone] 0
check "rectangular" [df::validate_rectangular $t] 1

# ---- tlock/din: the Display++ trigger input -----------------------------------
# a separate line, by a group pattern: tlock_din columns, rows by obsid
set grp extio/box01/state/group/dpp
dl_set $g:<ds>$grp  [dl_llist [dl_ilist 1 0 1] [dl_ilist 1 0] [dl_ilist]]
dl_set $g:<dst>$grp [dl_llist [dl_dlist 5.5 2013.8 2022.1] [dl_dlist 9.25 2017.6] [dl_dlist]]
dl_set $g:<dsn>$grp [dl_llist [dl_ilist 1 1 1] [dl_ilist 1 1] [dl_ilist]]
proc set_tlock_din {g v} {
    if {[dl_exists $g:<session>tlock/din]} { dl_delete $g:<session>tlock/din }
    dl_set $g:<session>tlock/din [dl_llist [dl_slist $v]]
}
set_tlock_din $g [dict create status ok kind group dp extio/*/state/group/dpp detail "group dpp on box01"]
set t4 [dg_create]
dl_set $t4:obsid [dl_ilist 1 0 2]
df::add_recorded_streams $t4 $g
check "tlock_din_t row0 = obs 1" [dl_tcllist [dl_get $t4:tlock_din_t 0]] {9.25 2017.6}
check "tlock_din row1 = obs 0" [dl_tcllist [dl_get $t4:tlock_din 1]] {1 0 1}
check "tlock_din obs with no edges: empty row" [dl_length [dl_get $t4:tlock_din_t 2]] 0
check "rig DI still added beside it" [dl_tcllist [dl_get $t4:tlock_t 2]] 7.0

# the same line extio/record already added: not added twice
set_tlock_din $g [dict create status ok kind di dp $di detail "box01 pin 3, labelled tlock"]
set t5 [dg_create]
dl_set $t5:obsid [dl_ilist 0]
df::add_recorded_streams $t5 $g
check "same line as a rig DI: no tlock_din" [dl_exists $t5:tlock_din] 0

# declared but unresolved when the file opened: nothing
set_tlock_din $g [dict create status unresolved kind "" dp "" detail "no box"]
set t6 [dg_create]
dl_set $t6:obsid [dl_ilist 0]
df::add_recorded_streams $t6 $g
check "unresolved tlock/din: no column" [dl_exists $t6:tlock_din_t] 0

# ---- photo/din: the light comparator's line ----------------------------------
set pdi extio/photo-sync/state/di/0
dl_set $g:<ds>$pdi  [dl_llist [dl_ilist 1 0] [dl_ilist 0 1 0 1] [dl_ilist]]
dl_set $g:<dst>$pdi [dl_llist [dl_dlist 22.5 2033.8] [dl_dlist 20.4 2031.8 2034.8 2748.4] [dl_dlist]]
dl_set $g:<dsn>$pdi [dl_llist [dl_ilist 1 1] [dl_ilist 1 1 1 1] [dl_ilist]]
dl_set $g:<session>photo/din [dl_llist [dl_slist \
    [dict create status ok kind di dp $pdi detail "photo-sync pin 0, labelled photopatch"]]]
set t7 [dg_create]
dl_set $t7:obsid [dl_ilist 1 0 2]
df::add_recorded_streams $t7 $g
check "photo_din_t row0 = obs 1" [dl_tcllist [dl_get $t7:photo_din_t 0]] {20.4 2031.8 2034.8 2748.4}
check "photo_din row1 = obs 0" [dl_tcllist [dl_get $t7:photo_din 1]] {1 0}
check "photo_din beside tlock/din's line" [dl_exists $t7:tlock_din] 0
dl_delete $g:<session>photo/din

# ---- a file with no keys: untouched ------------------------------------------
set g2 [dg_create]
set t2 [dg_create]
dl_set $t2:obsid [dl_ilist 0]
df::add_recorded_streams $t2 $g2
check "no keys: only obsid" [dg_tclListnames $t2] obsid

# ---- no obsid: untouched, not an error -------------------------------------------
set t3 [dg_create]
dl_set $t3:x [dl_ilist 1]
df::add_recorded_streams $t3 $g
check "no obsid: untouched" [dg_tclListnames $t3] x

if {$fails} { puts "\n$n checks, $fails FAILED"; exit 1 }
puts "\n$n checks: all checks passed"

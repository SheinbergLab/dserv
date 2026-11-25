# test_ess_photo_test.tcl -- the photodiode placement test (lib/ess_photo-1.0.tm,
# the photo_test block, with photo_verify's pv_median/pv_stats).
#
#   tclsh tests/test_ess_photo_test.tcl
#
# A pretend stim2 patch and diode stand behind the block's pt_* hooks:
# `photopatch flip` changes the patch at once, and the diode reads the
# dark or light level of the scenario, reporting an edge when the step is
# at least min_step. dservAfter only queues; `run` fires the queue, the
# clock moving one gap per tick, so each tick sees the flip before it as
# settled.

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

set ::clk 1000000000
proc now {} { return $::clk }
set ::pub {}
proc dservSetData { name ts type data } {
    if { $name eq "photo/test" } { lappend ::pub $data }
}
set ::timers [dict create]
set ::tid 0
proc dservAfter { ms script } {
    dict set ::timers [incr ::tid] [list $ms $script]
    return $::tid
}
proc dservAfterCancel { id } { dict unset ::timers $id }
# fire timers until none is left (or a limit), the clock moving each time
proc run { {limit 2000} } {
    while { [dict size $::timers] && [incr limit -1] > 0 } {
        set id [lindex [dict keys $::timers] 0]
        lassign [dict get $::timers $id] ms s
        dict unset ::timers $id
        set ::clk [expr {$::clk + $ms * 1000}]
        uplevel #0 $s
    }
}

set src [slurp [file join $root lib ess_photo-1.0.tm]]
foreach blk {photo_verify photo_test} {
    set b [string first "    # BEGIN $blk\n" $src]
    set e [string first "    # END $blk\n" $src]
    if { $b < 0 || $e < 0 } { error "no $blk block in lib" }
    namespace eval ::ess [string range $src $b $e]
}

# ---- the pretend rig ----
# sc: dark light min_step noise rise_us fall_us lose (a direction whose
# edges never come: 1, -1 or 0); busy; on mode level changes; sent
proc rig { args } {
    unset -nocomplain ::sc
    array set ::sc {dark 30 light 900 min_step 100 noise 1.5 rise_us 12000
        fall_us 9000 lose 0 slow 0 enabled 1 age 4000 stim ok}
    array set ::sc $args
    set ::busy ""
    set ::pp [dict create on 1 mode marked level 0 changes 0 wall -1]
    if { [info exists ::sc(on)] } { dict set ::pp on $::sc(on) }
    set ::edges 0
    set ::edge {}
    set ::din_last {}
    set ::sent {}
    set ::pub {}
    set ::timers [dict create]
}
proc reading {} {
    expr {[dict get $::pp on] && [dict get $::pp level] ? $::sc(light) : $::sc(dark)}
}
proc ::ess::pt_param { p } {
    switch -- $p {
        level    { return [reading] }
        n_edges  { return $::edges }
        noise    { return $::sc(noise) }
        min_step { return $::sc(min_step) }
        invert   { return [expr {[info exists ::sc(invert)] ? $::sc(invert) : 0}] }
    }
}
proc ::ess::pt_enabled {} { return $::sc(enabled) }
proc ::ess::pt_stream_age {} { return $::sc(age) }
proc ::ess::pt_patch_opts {} {
    dict create -mode marked -corner tr -size 60 -inset 0 -dark 0
}
proc ::ess::pt_wall_off {} { return 0 }
proc ::ess::pt_busy {} { return $::busy }
proc ::ess::pt_get { dp } { return $::edge }
# a light line (photo din): sc(din) its label, sc(din_bad) why it cannot be
# read, sc(din_rev) the line wired the wrong way round
proc ::ess::pt_din_line {} { expr {[info exists ::sc(din)] ? $::sc(din) : ""} }
proc ::ess::pt_din_check {} { expr {[info exists ::sc(din_bad)] ? $::sc(din_bad) : ""} }
proc ::ess::pt_din_count {} { set ::edges }
proc ::ess::pt_din_last {} { set ::din_last }
set ::done_hooks 0
proc ::ess::pt_done_hook {} { incr ::done_hooks }
proc ::ess::pt_stim { cmd } {
    lappend ::sent $cmd
    if { $::sc(stim) eq "none" } { return "" }
    if { $::sc(stim) eq "nomodule" } {
        return "!TCL_ERROR invalid command name \"photopatch\""
    }
    switch -glob -- $cmd {
        "photopatch status" {
            return [list on [dict get $::pp on] mode [dict get $::pp mode] \
                        level [dict get $::pp level] changes [dict get $::pp changes] \
                        last_change_flip_wall_us [dict get $::pp wall]]
        }
        "photopatch on*" {
            dict set ::pp on 1
            dict set ::pp level 0
            if { [llength $cmd] > 2 } { dict set ::pp mode [lindex $cmd 2] }
            return 1
        }
        "photopatch off" { dict set ::pp on 0 ; return 0 }
        "photopatch set*" {
            set i [lsearch $cmd -mode]
            if { $i >= 0 } { dict set ::pp mode [lindex $cmd $i+1] }
            return ""
        }
        "photopatch flip *" {
            if { ![dict get $::pp on] } { return "!TCL_ERROR photopatch flip: the patch is off" }
            set lv [lindex $cmd 2]
            set old [reading]
            dict set ::pp level $lv
            dict incr ::pp changes
            set wall [expr {$::clk + 8000}]
            dict set ::pp wall $wall
            set new [reading]
            set step [expr {$new - $old}]
            set dir [expr {$step > 0 ? 1 : -1}]
            # the processor negates an inverted detector: dir is "brighter"
            if { [info exists ::sc(invert)] && $::sc(invert) } { set dir [expr {-$dir}] }
            # ...and so does an active-low pin on the box (1 = light)
            if { [info exists ::sc(active_low)] } { set dir [expr {-$dir}] }
            if { abs($step) >= $::sc(min_step) && $dir != $::sc(lose) && !$::sc(slow) } {
                incr ::edges
                set lat [expr {$lv ? $::sc(rise_us) : $::sc(fall_us)}]
                set ::edge [list $dir [expr {$wall + $lat}] 0 0 $old $new 1 $::edges 0 0]
                set ::din_last [list [expr {($dir > 0) ^ [info exists ::sc(din_rev)]}] \
                                    [expr {$wall + $lat}]]
            }
            return $lv
        }
    }
    return ""
}

proc last {} { lindex $::pub end }
proc jget { key {json ""} } {
    if { $json eq "" } { set json [last] }
    if { [regexp "\"$key\":\"(\[^\"\]*)\"" $json -> v] } { return $v }
    if { [regexp "\"$key\":(\[^,\}\]*)" $json -> v] } { return $v }
    return "?"
}

puts "over the patch, bright:"
rig
set id [::ess::photo_test]
check [jget state] running "running at once"
run
check [list [jget state] [jget verdict]] {done aligned} "done: aligned"
check [list [jget flipped] [jget answered] [jget reversed]] {10 10 0} "  10 of 10 answered"
check [list [jget dark] [jget light] [jget step] [jget min_step]] {30.0 900.0 870.0 100.0} "  dark, light, step"
check [regexp {"rise":\{"n":5,"median":12.000} [last]] 1 "  rise 12 ms"
check [regexp {"fall":\{"n":5,"median":9.000} [last]] 1 "  fall 9 ms"
check [string match "aligned: 10 of 10 changes answered; step 870 counts*rise 12.000 ms*" [jget summary]] 1 "  the summary"
check [dict get $::pp mode] marked "  the patch's mode put back"
check [dict get $::pp on] 1 "  and left on (the gear had it on)"
check [lsearch -inline -all $::sent "photopatch set*"] {{photopatch set -mode manual} {photopatch set -mode marked}} "  manual for the test, then back"
regexp {"flips":\[(.*)\]\}$} [last] -> fl
check [llength [regexp -all -inline {\[[^]]*\]} $fl]] 10 "  ten flips listed"
check [lindex [regexp -all -inline {\[[^]]*\]} $fl] 0] {[1,1,870.0,12.000]} "  each {level edges step lat_ms}: lat from the flip"
check $::done_hooks 1 "  the health light looked at after"
check [expr {$id > 0}] 1 "  photo_test returns the id"

puts "the gear's patch off:"
rig on 0
::ess::photo_test run 4
check [lindex $::sent 1] {photopatch set -mode marked -corner tr -size 60 -inset 0 -dark 0} "drawn with the rig's settings"
check [lindex $::sent 2] {photopatch on manual} "  on, in manual mode"
run
check [jget verdict] aligned "  aligned"
check [dict get $::pp on] 0 "  taken away after"
check [lrange $::sent end-1 end] {{photopatch off} {photopatch set -mode marked}} "  off, its mode back"

puts "a step under 2x min_step:"
rig light 200
::ess::photo_test
run
check [jget verdict] small "small"
check [string match "small: every change answered, but the step 170 counts*" [jget summary]] 1 "  says the step"

puts "a step under min_step:"
rig light 90
::ess::photo_test
run
check [list [jget verdict] [jget answered]] {below_min_step 0} "below_min_step, nothing answered"
check [string match "*min_step 100 is above it*set min_step near 20" [jget summary]] 1 "  suggests a third of the step"

puts "over min_step, but too slow to be an edge:"
# the Pi 5 rig, 2026-11-22: step 146 over min_step 100, a ~10 ms rise
rig dark 50 light 197 noise 1.3 slow 1
::ess::photo_test
run
check [list [jget verdict] [jget answered] [jget step]] {no_edges 0 147.0} "no_edges"
check [string match "no edges: the diode sees the patch*above min_step 100*set min_step near 49" [jget summary]] 1 "  says why, suggests a min_step"

puts "not over the patch:"
rig light 31
::ess::photo_test
run
check [jget verdict] none "none"
check [string match "none: no change under the diode*(tr, 60 px)" [jget summary]] 1 "  names the corner"

puts "an inverted amplifier:"
rig dark 900 light 30
::ess::photo_test
run
check [list [jget verdict] [jget reversed] [jget answered]] {reversed 10 0} "reversed"
check [string match "*turn on photo invert" [jget summary]] 1 "  says to turn on invert"

puts "an inverted detector, invert on:"
rig dark 900 light 30 invert 1
::ess::photo_test
run
check [list [jget verdict] [jget answered] [jget reversed]] {aligned 10 0} "aligned"
check [string match "*step 870 counts (dark 900, light 30)*" [jget summary]] 1 "  a positive step, the box's counts"

puts "invert on, but the detector is not inverted:"
rig invert 1
::ess::photo_test
run
check [jget verdict] reversed "reversed"
check [string match "*turn it off" [jget summary]] 1 "  says to turn invert off"

puts "a light line (photo din), over the patch:"
rig din photopatch
::ess::photo_test
run
check [list [jget verdict] [jget answered] [jget reversed] [jget source]] {aligned 10 0 din} "aligned"
check [string match "aligned: 10 of 10 changes answered on line photopatch; rise *" [jget summary]] 1 "  names the line, with latencies"
check [list [jget dark] [jget step]] {null null} "  no levels for a line"

puts "a light line wired the wrong way round:"
rig din photopatch din_rev 1
::ess::photo_test
run
check [jget verdict] reversed "reversed"
check [string match "*active-low*" [jget summary]] 1 "  says active-low"

puts "a light line, not over the patch:"
rig din photopatch light 31
::ess::photo_test
run
check [jget verdict] none "none"
check [string match "*pot between the patch's dark and light*" [jget summary]] 1 "  says the pot"

puts "a light line that does not resolve:"
rig din photopatch din_bad "photo din: no box announces a line or a group labelled photopatch"
check [catch { ::ess::photo_test } err] 1 "refused"
check [string match "*no box announces*" $err] 1 "  says why"

# A line from the box's own comparator (pin mode cmp): sc(cmp) set. The box
# reports each settled level in counts (state/cmp/level, pt_cmp_level) and
# its state/cmp; sc(thr) is the locked threshold (counts), sc(cmp_st) its
# state. A relearn sets sc(relearned). sc(active_low) an inverted pin.
proc ::ess::pt_cmp_line {} {
    if { ![info exists ::sc(cmp)] } { return {} }
    list box 15 [expr {[info exists ::sc(active_low)] ? -1 : 1}]
}
proc ::ess::pt_cmp_level { box } { reading }
proc ::ess::pt_cmp_state { box } {
    set st [expr {[info exists ::sc(cmp_st)] ? $::sc(cmp_st) : "armed"}]
    set thr [expr {[info exists ::sc(thr)] ? $::sc(thr) : 464}]
    list state $st lo $::sc(dark) hi $::sc(light) dac [expr {$thr / 16 - 1}] drift 0 mismatch 0
}
proc ::ess::pt_cmp_relearn { box } { incr ::sc(relearned) ; set ::sc(cmp_st) armed }

puts "a cmp line, over the patch:"
rig din photopatch cmp 1
::ess::photo_test
run
check [list [jget verdict] [jget answered] [jget source]] {aligned 10 cmp} "aligned, source cmp"
check [list [jget dark] [jget light] [jget step] [jget min_step]] {30.0 900.0 870.0 100.0} "  the box's levels, its min step"
check [string match "aligned: 10 of 10 changes answered on line photopatch; step 870 counts*threshold 464*" [jget summary]] 1 "  levels and threshold"
check [info exists ::sc(relearned)] 0 "  a run does not relearn"

puts "a cmp line, the threshold outside the levels:"
rig din photopatch cmp 1 thr 1200
set ::sc(min_step) 100000   ;# the pretend comparator reports no edge
::ess::photo_test
run
set ::sc(min_step) 100
check [list [jget verdict] [jget answered]] {no_edges 0} "no_edges"
check [string match "*threshold (1200 counts) is not between*relearn*" [jget summary]] 1 "  says the threshold is off, relearn"

puts "a cmp line, a step the box cannot learn from:"
rig din photopatch cmp 1 light 90
::ess::photo_test
run
check [jget verdict] below_min_step "below_min_step"
check [string match "*box needs a step of 100*" [jget summary]] 1 "  the box's minimum"

puts "a cmp line, still learning:"
rig din photopatch cmp 1 cmp_st learning
set ::sc(min_step) 100000
::ess::photo_test
run
set ::sc(min_step) 100
check [string match "no_edges*comparator is learning*" [jget summary]] 1 "says it is learning"

puts "a cmp line, an active-low pin (light reads lower):"
rig din photopatch cmp 1 dark 4050 light 3200 active_low 1
::ess::photo_test
run
check [list [jget verdict] [jget answered]] {aligned 10} "aligned"
check [string match "*step 850 counts (dark 4050, light 3200)*" [jget summary]] 1 "  a positive step"

puts "a cmp line, aim ends by relearning:"
rig din photopatch cmp 1 cmp_st learning
::ess::photo_test aim 1
run
check [list [jget state] [jget flipped]] {done 4} "done after the 4 aim flips"
check [info exists ::sc(relearned)] 1 "  relearned"
check [llength [lsearch -all $::sent "photopatch flip *"]] 7 "  then 3 relock flips"
check [jget verdict] aligned "  judged by the relock: aligned"
check [string match "aimed: step 870 counts*relearned and locked at 464 counts, between them, and the change after it answered" [jget summary]] 1 "  says where it locked"

puts "a cmp line, aim with early flips missed (positioning), then a good relock:"
rig din photopatch cmp 1
set ::sc(min_step) 100000      ;# while aiming: no edges
::ess::photo_test aim 1
for { set i 0 } { $i < 5 } { incr i } {
    set id [lindex [dict keys $::timers] 0]
    lassign [dict get $::timers $id] ms s
    dict unset ::timers $id
    set ::clk [expr {$::clk + $ms * 1000}]
    uplevel #0 $s
}
set ::sc(min_step) 100         ;# the relock: edges again
run
check [list [jget verdict] [jget answered]] {aligned 0} "aligned though no aim flip answered"
check [string match "aimed: *(0 of 4 changes answered while aiming)*" [jget summary]] 1 "  the aim's count said, not judged"

puts "a cmp line, aim whose relock lands outside the levels:"
rig din photopatch cmp 1 thr 2000
::ess::photo_test aim 1
run
check [jget verdict] partial "partial"
check [string match "*relearned at 2000 counts, which is not between*" [jget summary]] 1 "  says the detector moved"

puts "a cmp line, running summaries are signed (active-low):"
rig din photopatch cmp 1 dark 4050 light 3200 active_low 1
::ess::photo_test run 4
for { set i 0 } { $i < 3 } { incr i } {
    set id [lindex [dict keys $::timers] 0]
    lassign [dict get $::timers $id] ms s
    dict unset ::timers $id
    set ::clk [expr {$::clk + $ms * 1000}]
    uplevel #0 $s
}
check [string match "flip * of 4: step 850 counts*" [jget summary]] 1 "  + step while running"
run

puts "a cmp line, aim stopped: relearns first:"
rig din photopatch cmp 1
::ess::photo_test aim 10
set id [lindex [dict keys $::timers] 0]
lassign [dict get $::timers $id] ms s
dict unset ::timers $id
uplevel #0 $s
check [string match "stopping: relearning*" [::ess::photo_test stop]] 1 "stop says it relearns"
run
check [list [jget state] [info exists ::sc(relearned)]] {done 1} "  done, relearned"
check [llength [lsearch -all $::sent "photopatch flip *"]] 4 "  1 aim flip + 3 relock flips"

puts "only the rises come back:"
rig lose -1
::ess::photo_test
run
check [list [jget verdict] [jget answered]] {partial 5} "partial, 5 of 10"

puts "not now:"
rig
set ::busy "a data file is open"
check [catch { ::ess::photo_test } err] 1 "refused while a data file is open"
check [jget state] error "  photo/test says so"
check [string match "not now: a data file is open*" [jget summary]] 1 "  and why"
check $::sent {} "  stim2 not touched"

puts "ESS starts during the test:"
rig
::ess::photo_test
for { set i 0 } { $i < 4 } { incr i } {
    set id [lindex [dict keys $::timers] 0]
    lassign [dict get $::timers $id] ms s
    dict unset ::timers $id
    set ::clk [expr {$::clk + $ms * 1000}]
    uplevel #0 $s
}
set ::busy "ESS is running"
run
check [jget state] stopped "stopped"
check [string match "stopped: ESS is running;*" [jget summary]] 1 "  says why, then what it had"
check [dict get $::pp mode] marked "  the mode put back"

puts "stop:"
rig
::ess::photo_test aim 10
set id [lindex [dict keys $::timers] 0]
lassign [dict get $::timers $id] ms s
dict unset ::timers $id
uplevel #0 $s
check [jget mode] aim "aim mode"
::ess::photo_test stop
check [list [jget state] [dict size $::timers]] {stopped 0} "  stopped, its timer cancelled"
check [::ess::photo_test stop] "no test running" "  a second stop"

puts "aim:"
rig
::ess::photo_test aim 1
check [jget n] 4 "1 s at 300 ms: 4 flips"
run
check [list [jget state] [jget flipped]] {done 4} "  done after 4"
set ::sc(light) 400
::ess::photo_test aim 1
run
check [jget step] 370.0 "  the step from the last flips"

puts "one at a time:"
rig
::ess::photo_test
check [catch { ::ess::photo_test } err] 1 "a second test refused"
check [string match "*already running*" $err] 1 "  saying so"
run

puts "no stim2, no module, no stream, not enabled:"
rig stim none
check [catch { ::ess::photo_test }] 1 "stim2 not answering: refused"
check [string match "*did not answer*" [jget summary]] 1 "  says so"
rig stim nomodule
check [catch { ::ess::photo_test }] 1 "no photopatch module: refused"
check [jget summary] "stim2 has no photopatch module" "  says so"
rig age -1
check [catch { ::ess::photo_test }] 1 "no blocks: refused"
rig enabled 0
check [catch { ::ess::photo_test }] 1 "photo not enabled: refused"
check [string match "photo is not enabled*" [jget summary]] 1 "  says so"

puts "the patch goes away mid-test:"
rig
::ess::photo_test
set id [lindex [dict keys $::timers] 0]
lassign [dict get $::timers $id] ms s
dict unset ::timers $id
uplevel #0 $s
dict set ::pp on 0
run
check [jget state] error "error"
check [string match "*patch went away*" [jget summary]] 1 "  says so"

puts "bad arguments:"
rig
check [catch { ::ess::photo_test run 0 }] 1 "0 flips"
check [catch { ::ess::photo_test run 10 20 }] 1 "a 20 ms gap"
check [catch { ::ess::photo_test aim -1 }] 1 "aim -1 s"
check [catch { ::ess::photo_test what }] 1 "an unknown subcommand"

puts [expr {$fails ? "$fails FAILED" : "all checks passed"}]
exit [expr {$fails > 0}]

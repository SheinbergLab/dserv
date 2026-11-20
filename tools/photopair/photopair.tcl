# photopair.tcl -- pair a session's photodiode light edges with the frames
# (or events) that caused them, and say how long the light took
#
#   dlsh tools/photopair/photopair.tcl ?options? FILE.ess
#
# The edges are dserv's `photo` processor's (proc/photo/edge, logged while
# the gear's `photo enabled` is on) or, when the file has none or with
# -replay, the same detector run here over the diode's recorded raw group
# (photo_replay, built with dserv: build/processors/photo_replay).
#
# The marks are what each edge is paired with:
#   photopatch  stim2's patch changes (stim/photopatch, the default when
#               the file has them): each flip, put on the dserv clock with
#               the logged dserv-minus-wall offset (ess/dserv_wall_us), and
#               the direction the change should give (light: rising).
#   tlock       the Display++ trigger's edges on the input `tlock din`
#               names (tlock/din), rising: on the box clock, like the
#               diode's samples.
#   di:VARNAME  rising edges of any digital datapoint.
#   evt:T:S     events of that type and subtype (SAMPLE ON is evt:30:1).
#   cmd:GLOB    stim2 commands whose name matches (stim/cmd, e.g.
#               cmd:!sample_on), at the flip stim2 reported for them.
#
# Pairing: first each mark with the next edge of its direction, which gives
# a typical latency for rising edges and one for falling; then each mark
# with the unused edge nearest to its time plus that edge direction's
# latency, within -window ms. An edge with no mark (the
# stimulus changing the light by itself, noise) stays unpaired, and a mark
# with no edge (light that never changed under the diode) gets none.
#
# Options:
#   -marks M        photopatch | tlock | di:VARNAME | evt:T:S | cmd:GLOB
#   -dir D          rise | fall | any: the edge a mark expects (photopatch
#                   marks say for themselves; default any)
#   -replay         detect again from the raw group even if edges are logged
#   -group G        the raw group: its label, box/label, or a varname
#   -p KEY=VALUE    a detector parameter for -replay (repeatable), e.g.
#                   -p strobe=1 -p period=0.0083333
#   -max MS         the longest latency the first pass accepts (100)
#   -window MS      half-width around the typical latency (4)
#   -offset US      dserv minus wall clock, to use instead of the logged one
#   -csv FILE       one row per mark
#   -bin PATH       photo_replay
#
# Run with /usr/local/bin/dlsh (needs dslog). docs/photo_processor.md,
# "Pairing edges with frames".

namespace eval photopair {
    variable opt
    array set opt {
        marks "" dir any replay 0 group "" params {} max 100 window 4
        offset "" csv "" bin ""
    }

    # ---- the file ---------------------------------------------------------

    # the records this tool reads, by name: a list of {timestamp vals}
    proc read_file { path } {
        package require dlsh
        package require dslog
        dslog::wideTypes 1
        set g [dslog::read $path -absolute]
        set recs [dict create]
        foreach n [dl_tcllist $g:varname] t [dl_tcllist $g:timestamp] \
            v [dl_tcllist $g:vals] {
            if { [string match evt:* $n] || [string match extio/*/state/* $n] ||
                 $n in {proc/photo/edge stim/photopatch stim/frame stim/cmd
                     ess/dserv_wall_us tlock/din ess/ain/recorded} } {
                dict lappend recs $n [list $t $v]
            }
        }
        dg_delete $g
        return $recs
    }

    proc recs { recs name } {
        expr {[dict exists $recs $name] ? [dict get $recs $name] : {}}
    }

    # ---- edges --------------------------------------------------------------

    # an edge: {dir t50 t10 t90 old new settled seq}
    proc edges_logged { recs } {
        set out {}
        foreach r [recs $recs proc/photo/edge] {
            set v [lindex $r 1]
            for { set i 0 } { $i + 10 <= [llength $v] } { incr i 10 } {
                lappend out [lrange $v $i [expr {$i + 7}]]
            }
        }
        lsort -real -index 1 $out
    }

    # the raw analog groups in the file, by varname
    proc raw_groups { recs } {
        lsort [lmap n [dict keys $recs] {
            if { ![string match extio/*/state/ain/* $n] } continue
            set n
        }]
    }

    proc pick_group { recs group } {
        set all [raw_groups $recs]
        if { $group eq "" } {
            if { [llength $all] == 1 } { return [lindex $all 0] }
            if { ![llength $all] } { error "no recorded analog group in this file" }
            error "several analog groups ([join $all {, }]): name one with -group"
        }
        foreach n $all {
            set parts [split $n /]          ;# extio box state ain label
            set label [lindex $parts end]
            set box [lindex $parts 1]
            if { $group eq $n || $group eq $label || $group eq "$box/$label" } { return $n }
        }
        error "no analog group $group here ([join $all {, }])"
    }

    proc find_bin { given } {
        if { $given ne "" } { return $given }
        set here [file dirname [file normalize [info script]]]
        foreach c [list [file join $here .. .. build processors photo_replay] \
                       /usr/local/dserv/bin/photo_replay] {
            if { [file executable $c] } { return $c }
        }
        error "photo_replay not found: build dserv (build/processors/photo_replay) or give -bin"
    }

    # the detector over a recorded group: {edges counters}
    proc edges_replay { recs varname params bin } {
        set lines {}
        foreach r [recs $recs $varname] {
            lassign $r t v
            lappend lines "$varname $t [binary encode hex [binary format c* $v]]"
        }
        set out [exec $bin {*}$params << [join $lines \n] 2>@ stderr]
        set edges {}
        set counters [dict create]
        foreach line [split $out \n] {
            switch -- [lindex $line 0] {
                edge  { lappend edges [lrange $line 1 8] }
                param { dict set counters [lindex $line 1] [lindex $line 2] }
            }
        }
        list [lsort -real -index 1 $edges] $counters
    }

    # ---- marks --------------------------------------------------------------

    # dserv clock minus the wall clock stim2 stamps flips with: the logged
    # value, else an estimate from stim2's own records -- each arrives
    # after its flip, so (receipt - flip) is the offset plus a delivery
    # time, and its least value is the offset plus the shortest one: an
    # upper bound, which makes every latency read a little short.
    proc wall_offset { recs given } {
        if { $given ne "" } { return [list $given given] }
        set logged [recs $recs ess/dserv_wall_us]
        if { [llength $logged] } {
            return [list [lindex $logged end 1] logged]
        }
        set d {}
        foreach r [recs $recs stim/frame] {
            lassign $r t v
            set w [lindex $v 2]           ;# seq swapcount flip_us ...
            if { $w > 0 } { lappend d [expr {$t - $w}] }
        }
        foreach r [recs $recs stim/photopatch] {
            lassign $r t v
            set w [lindex $v 1]           ;# swapcount flip_wall ...
            if { $w > 0 } { lappend d [expr {$t - $w}] }
        }
        if { ![llength $d] } { return [list 0 none] }
        list [tcl::mathfunc::min {*}$d] estimated
    }

    # a mark: {t dir label}; dir 1, -1, or 0 for either
    proc dir_of { d } {
        switch -- $d { rise { return 1 } fall { return -1 } any { return 0 } }
        error "-dir is rise, fall or any"
    }

    proc marks_photopatch { recs off } {
        set out {}
        foreach r [recs $recs stim/photopatch] {
            lassign [lindex $r 1] swap wall level serial ntags why
            if { $wall <= 0 } continue
            # the patch appearing changes the light from the scene's: either way
            set dir [expr {$why == 1 ? 0 : ($level ? 1 : -1)}]
            lappend out [list [expr {$wall + $off}] $dir "swap $swap change $serial level $level why $why"]
        }
        return $out
    }

    proc marks_di { recs varname dir } {
        set out {}
        foreach r [recs $recs $varname] {
            lassign $r t v
            if { $v == 1 } { lappend out [list $t $dir "$varname 1"] }
        }
        return $out
    }

    # the line `tlock din` named, as the file has it (tlock/din:
    # {status ok dp NAME ...}); a pattern is matched against the file
    proc tlock_varname { recs } {
        set r [lindex [recs $recs tlock/din] end 1 0]   ;# a string: one element
        if { $r eq "" || [catch { dict get $r dp } dp] } {
            error "no tlock/din in this file (setting `tlock din`): use -marks di:VARNAME"
        }
        if { [string match {*[*?]*} $dp] } {
            foreach n [lsort [dict keys $recs]] {
                if { [string match $dp $n] } { return $n }
            }
        }
        return $dp
    }

    proc marks_evt { recs name dir } {
        lmap r [recs $recs $name] { list [lindex $r 0] $dir $name }
    }

    proc marks_cmd { recs glob off dir } {
        set out {}
        foreach r [recs $recs stim/cmd] {
            set j [lindex $r 1 0]          ;# a string: one element
            if { ![regexp {"cmd":"((?:[^"\\]|\\.)*)"} $j -> cmd] } continue
            if { ![string match $glob $cmd] } continue
            if { ![regexp {"flip":(-?[0-9]+)} $j -> flip] || $flip <= 0 } continue
            lappend out [list [expr {$flip + $off}] $dir $cmd]
        }
        return $out
    }

    # ---- pairing ------------------------------------------------------------

    proc compatible { mdir edir } { expr {$mdir == 0 || $mdir == $edir} }

    proc median { l } {
        set l [lsort -real $l]
        set n [llength $l]
        if { !$n } { return {} }
        if { $n % 2 } { return [lindex $l [expr {$n / 2}]] }
        expr {([lindex $l [expr {$n / 2 - 1}]] + [lindex $l [expr {$n / 2}]]) / 2.0}
    }

    # A direction needs this many first-pass latencies for one of its own;
    # below it (a stray edge or two of the other direction) it uses L.
    variable min_dir_n 3

    # marks and edges (each sorted by time) -> {L rows Ldir}: L the typical
    # latency in us ({} if nothing paired), a row per mark
    #   {mark edge_index lat10 lat50 lat90}   (us; edge -1 and lats {} unpaired)
    # and Ldir {1 L_rising -1 L_falling}, what each edge direction was judged
    # by. A display rises and falls at different speeds (an LCD here: 8.1 ms
    # up, 3.4 down), so a mark that does not say its direction (cmd:, evt:)
    # is paired with each candidate edge by that edge's own latency: one L
    # between the two would put half the marks outside -window.
    proc pair { marks edges max_ms window_ms } {
        variable min_dir_n
        set marks [lsort -real -index 0 $marks]
        set t50 [lmap e $edges { lindex $e 1 }]
        set ne [llength $edges]

        # 1: each mark's next edge of its direction, each on its own (not
        # in turn: one edge missed would shift every later pair by one)
        set lats {}
        set bydir [dict create 1 {} -1 {}]
        foreach m $marks {
            lassign $m t dir
            set k [expr {[lsearch -bisect -real $t50 $t] + 1}]
            for { } { $k < $ne } { incr k } {
                if { [lindex $t50 $k] - $t > $max_ms * 1000 } break
                if { [lindex $t50 $k] <= $t } continue
                set edir [expr {int([lindex $edges $k 0])}]
                if { [compatible $dir $edir] } {
                    set lat [expr {[lindex $t50 $k] - $t}]
                    lappend lats $lat
                    dict lappend bydir $edir $lat
                    break
                }
            }
        }
        set L [median $lats]
        if { $L eq "" } {
            return [list {} [lmap m $marks { list $m -1 {} {} {} }] {}]
        }
        set Ldir [dict create]
        foreach d {1 -1} {
            set l [dict get $bydir $d]
            dict set Ldir $d [expr {[llength $l] >= $min_dir_n ? [median $l] : $L}]
        }
        set lo [tcl::mathfunc::min {*}[dict values $Ldir]]
        set hi [tcl::mathfunc::max {*}[dict values $Ldir]]

        # 2: the unused edge nearest the mark plus its direction's latency
        set w [expr {$window_ms * 1000.0}]
        set used [lrepeat $ne 0]
        set rows {}
        foreach m $marks {
            lassign $m t dir
            set k [lsearch -bisect -real $t50 [expr {$t + $lo - $w}]]
            if { $k < 0 } { set k 0 }
            set best -1
            set bd {}
            for { } { $k < $ne && [lindex $t50 $k] <= $t + $hi + $w } { incr k } {
                if { [lindex $used $k] } continue
                set edir [expr {int([lindex $edges $k 0])}]
                if { ![compatible $dir $edir] } continue
                set d [expr {abs([lindex $t50 $k] - $t - [dict get $Ldir $edir])}]
                if { $d > $w } continue
                if { $best < 0 || $d < $bd } { set best $k ; set bd $d }
            }
            if { $best < 0 } {
                lappend rows [list $m -1 {} {} {}]
                continue
            }
            lset used $best 1
            lassign [lindex $edges $best] edir e50 e10 e90
            lappend rows [list $m $best [expr {$e10 - $t}] [expr {$e50 - $t}] [expr {$e90 - $t}]]
        }
        list $L $rows $Ldir
    }

    # n median mean sd min max, ms, of a list of us
    proc stats { l } {
        set n [llength $l]
        if { !$n } { return [list 0 {} {} {} {} {}] }
        set ms [lmap x $l { expr {$x / 1000.0} }]
        set mean [expr {[tcl::mathop::+ {*}$ms] / $n}]
        set ss 0.0
        foreach x $ms { set ss [expr {$ss + ($x - $mean) ** 2}] }
        set sd [expr {$n > 1 ? sqrt($ss / ($n - 1)) : 0.0}]
        list $n [median $ms] $mean $sd [tcl::mathfunc::min {*}$ms] [tcl::mathfunc::max {*}$ms]
    }

    proc fmt_stats { label l } {
        lassign [stats $l] n med mean sd lo hi
        if { !$n } { return [format "  %-12s %5d" $label 0] }
        format "  %-12s %5d %9.3f %9.3f %7.3f %9.3f %9.3f" $label $n $med $mean $sd $lo $hi
    }

    # ---- main ----------------------------------------------------------------

    proc usage { {msg ""} } {
        if { $msg ne "" } { puts stderr "photopair: $msg" }
        puts stderr "usage: photopair.tcl ?-marks M? ?-dir rise|fall|any? ?-replay? ?-group G?\
 ?-p KEY=VALUE ...? ?-max MS? ?-window MS? ?-offset US? ?-csv FILE? ?-bin PATH? FILE.ess"
        exit 2
    }

    proc main { argv } {
        variable opt
        set file ""
        for { set i 0 } { $i < [llength $argv] } { incr i } {
            set a [lindex $argv $i]
            switch -- $a {
                -replay { set opt(replay) 1 }
                -p { lappend opt(params) [lindex $argv [incr i]] }
                -marks - -dir - -group - -max - -window - -offset - -csv - -bin {
                    if { $i + 1 >= [llength $argv] } { usage "$a needs a value" }
                    set opt([string range $a 1 end]) [lindex $argv [incr i]]
                }
                default {
                    if { [string match -* $a] } { usage "unknown option $a" }
                    if { $file ne "" } { usage "one file" }
                    set file $a
                }
            }
        }
        if { $file eq "" } usage
        set recs [read_file $file]
        puts "[file tail $file]"

        # edges
        set counters {}
        if { $opt(replay) || ![llength [recs $recs proc/photo/edge]] } {
            set vn [pick_group $recs $opt(group)]
            lassign [edges_replay $recs $vn $opt(params) [find_bin $opt(bin)]] edges counters
            set how "replayed from $vn[expr {[llength $opt(params)] ? " ([join $opt(params)])" : ""}]"
        } else {
            set edges [edges_logged $recs]
            set how "logged (proc/photo/edge)"
        }
        set nr [llength [lsearch -all -exact -index 0 -real $edges 1.0]]
        puts "edges: [llength $edges], $nr rising, [expr {[llength $edges] - $nr}] falling; $how"
        if { [dict size $counters] } {
            set c {}
            foreach k {n_samples dt_est n_unsettled n_gaps n_pulses n_missing period_est noise} {
                if { [dict exists $counters $k] } { lappend c "$k [dict get $counters $k]" }
            }
            puts "  detector: [join $c {, }]"
        }

        # marks
        set m $opt(marks)
        if { $m eq "" } {
            set m [expr {[llength [recs $recs stim/photopatch]] ? "photopatch" : ""}]
            if { $m eq "" } { usage "no stim/photopatch here: name the marks with -marks" }
        }
        set dir [dir_of $opt(dir)]
        set offnote ""
        if { $m eq "photopatch" || [string match cmd:* $m] } {
            lassign [wall_offset $recs $opt(offset)] off src
            set offnote " (flip + dserv-wall offset $off us, $src)"
            if { $src eq "estimated" } {
                append offnote "\n  the offset is estimated from stim2's records' arrival: latencies read short by the shortest delivery"
            }
        }
        switch -glob -- $m {
            photopatch { set marks [marks_photopatch $recs $off] }
            tlock      { set marks [marks_di $recs [tlock_varname $recs] $dir] }
            di:*       { set marks [marks_di $recs [string range $m 3 end] $dir] }
            evt:*      { set marks [marks_evt $recs $m $dir] }
            cmd:*      { set marks [marks_cmd $recs [string range $m 4 end] $off $dir] }
            default    { usage "-marks: photopatch, tlock, di:VARNAME, evt:T:S or cmd:GLOB" }
        }
        puts "marks: [llength $marks] $m$offnote"

        lassign [pair $marks $edges $opt(max) $opt(window)] L rows Ldir
        if { $L eq "" } {
            puts "nothing paired: no edge of the right direction within $opt(max) ms of any mark"
            exit 1
        }
        set paired [lmap r $rows { if { [lindex $r 1] < 0 } continue ; set r }]
        set Lr [dict get $Ldir 1]
        set Lf [dict get $Ldir -1]
        set Lnote [expr {$Lr == $Lf ? "" : [format " (rising %.3f, falling %.3f)" \
                                              [expr {$Lr / 1000.0}] [expr {$Lf / 1000.0}]]}]
        puts [format "paired: %d of %d marks, %d edges unpaired; typical latency %.3f ms%s, window +-%s ms" \
                  [llength $paired] [llength $rows] [expr {[llength $edges] - [llength $paired]}] \
                  [expr {$L / 1000.0}] $Lnote $opt(window)]
        puts [format "  %-12s %5s %9s %9s %7s %9s %9s" "mark->light" n median mean sd min max]
        foreach {lab ix} {t10 2 t50 3 t90 4} {
            puts [fmt_stats $lab [lmap r $paired { lindex $r $ix }]]
        }
        foreach {lab d} {"t50 rising" 1 "t50 falling" -1} {
            set l [lmap r $paired {
                if { [lindex $edges [lindex $r 1] 0] != $d } continue
                lindex $r 3
            }]
            if { [llength $l] } { puts [fmt_stats $lab $l] }
        }

        if { $opt(csv) ne "" } {
            set f [open $opt(csv) w]
            puts $f "mark_t,mark_dir,mark,edge_seq,edge_dir,lat10_ms,lat50_ms,lat90_ms,old,new,settled"
            foreach r $rows {
                lassign $r mk k l10 l50 l90
                lassign $mk t d lab
                if { $k < 0 } {
                    puts $f "$t,$d,\"$lab\",,,,,,,,"
                    continue
                }
                lassign [lindex $edges $k] edir e50 e10 e90 old new settled seq
                puts $f [format "%s,%s,\"%s\",%s,%s,%.3f,%.3f,%.3f,%s,%s,%s" $t $d $lab \
                             [expr {int($seq)}] [expr {int($edir)}] \
                             [expr {$l10 / 1000.0}] [expr {$l50 / 1000.0}] [expr {$l90 / 1000.0}] \
                             $old $new [expr {int($settled)}]]
            }
            close $f
            puts "wrote $opt(csv)"
        }
    }
}

if { [info exists argv0] && [file normalize $argv0] eq [file normalize [info script]] } {
    if { [catch { photopair::main $argv } err] } {
        puts stderr "photopair: $err"
        exit 1
    }
}

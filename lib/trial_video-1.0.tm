# trial_video-1.0.tm — play one observation from a stim/inbox tape
#
# trial_video_make passes the .ess and a trial index. The player sources
# the stim script stored in the .ess, or the file named there when the
# block has no copy, and feeds that trial's cmd and ds records back in
# seq order. StimTimeF is pinned from the tape (stimPlayback / stimTimeSet),
# one capture per original swap. The mp4 repeats those pictures at 60 fps.
# There is no per-protocol plugin.
package provide trial_video 1.0

namespace eval trial_video {
    variable frame_ms [expr {1000.0 / 60.0}]
    variable tape_ready 1
    variable touch_hold 0.2
    variable touch_png [file normalize [file join [file dirname [info script]] touch.png]]
    variable eye_png [file normalize [file join [file dirname [info script]] eye.png]]
    variable eye_trail_py [file normalize [file join [file dirname [info script]] eye_trail.py]]
    # One dot per video frame, every third frame, newest at 50% and each
    # older dot 0.82 of the one in front of it. Fifteen dots reach back
    # about 0.7 s and the last one is near 3%. The download paints that
    # whole trail into one frame and overlays it once.
    variable eye_trail_n 15
    variable eye_trail_gap 3
    variable eye_trail_fade 0.82

    proc stim {script} {
        if {[catch {exec essctrl -s stim -c $script} out]} {
            return -code error "stim: $out"
        }
        set out [string trim $out]
        if {[string match "!TCL_ERROR *" $out]} {
            return -code error "stim: [string range $out 11 end]"
        }
        return $out
    }

    proc stim_file {path} {
        stim [list source $path]
    }

    # One stim script for a whole trial. essctrl closes the read after 30s,
    # which is shorter than a long capture, so this talks to stim's message
    # port directly and waits until the script returns.
    proc stim_run {path} {
        set payload [list source $path]
        set n [string length $payload]
        set sock [socket 127.0.0.1 4612]
        # Tcl 9 dropped the "binary" encoding. iso8859-1 keeps every byte.
        fconfigure $sock -translation lf -encoding iso8859-1 -blocking 1 -buffering full
        puts -nonewline $sock [binary format I $n]
        puts -nonewline $sock $payload
        flush $sock
        set hdr [read $sock 4]
        if {[string length $hdr] != 4} {
            close $sock
            return -code error "stim: short reply header"
        }
        binary scan $hdr I rn
        set resp ""
        if {$rn > 0} { set resp [read $sock $rn] }
        close $sock
        if {[string match "!TCL_ERROR *" $resp]} {
            return -code error "stim: [string range $resp 11 end]"
        }
        return $resp
    }

    proc point_value {g vals_cmd i} {
        set v [dl_get $g:vals $i]
        if {[dl_datatype $v] eq "string" || [dl_datatype $v] eq "list"} {
            return [lindex [dl_tcllist $v] 0]
        }
        return [dl_tcllist $v]
    }

    proc parse_inbox {rec} {
        set nl [string first \n $rec]
        if {$nl < 0} {
            return -code error "stim/inbox record has no body"
        }
        set hdr [string range $rec 0 [expr {$nl - 1}]]
        if {[llength $hdr] < 6} {
            return -code error "bad stim/inbox header: $hdr"
        }
        lassign $hdr seq kind ticks timef swap nbytes
        if {![string is integer -strict $nbytes] || $nbytes < 0} {
            return -code error "bad stim/inbox nbytes: $hdr"
        }
        if {$nbytes == 0} {
            set body ""
        } else {
            set body [string range $rec [expr {$nl + 1}] [expr {$nl + $nbytes}]]
        }
        return [dict create seq $seq kind $kind ticks $ticks timef $timef \
                    swap $swap nbytes $nbytes body $body]
    }

    proc write_stimdg {g idx path} {
        set vals [dl_get $g:vals $idx]
        if {[dl_datatype $vals] ne "char"} {
            return -code error "stimdg is [dl_datatype $vals], expected char"
        }
        set bytes [binary format cu* [dl_tcllist $vals]]
        set fd [open $path wb]
        fconfigure $fd -translation binary
        puts -nonewline $fd $bytes
        close $fd
        return [file size $path]
    }

    # Source the stim file the way configure_stim does (dservhost set, so
    # the file's subscribe succeeds and fills ::dsCmds), then drop the
    # server matches without clearing the handlers.
    proc install_stim {host stimfile work} {
        set setup [file join $work setup.tcl]
        set fd [open $setup w]
        puts $fd [list stimPlayback 1]
        puts $fd [list set ::dservhost $host]
        puts $fd {
package require qpcs
catch { qpcs::dsStimUnregister $::dservhost }
catch { array unset ::dsCmds }
qpcs::dsStimRegister $::dservhost
}
        puts $fd [list source $stimfile]
        puts $fd {
qpcs::dsStimUnregister $::dservhost
proc _tape_ds {raw} {
    set name [lindex $raw 0]
    if {![info exists ::dsCmds($name)]} {
        error "no handler for $name"
    }
    eval $::dsCmds($name) $raw
}
proc _tape_seen {path T pin} {
    set f [open $path a]
    puts $f [format "%.6f %.6f %.6f %d" $T $pin $::StimTimeF $::tape_mark]
    close $f
}
if {![array exists ::dsCmds]} { return "" }
return [lsort [array names ::dsCmds]]
}
        close $fd
        set handlers [stim_file $setup]
        file delete -force $setup
        return $handlers
    }

    proc load_stimdg {bin} {
        set load [file join [file dirname $bin] load_stimdg.tcl]
        set fd [open $load w]
        puts $fd "set f \[open [list $bin] rb\]"
        puts $fd {fconfigure $f -translation binary}
        puts $fd {set data [read $f]}
        puts $fd {close $f}
        puts $fd {dg_fromString $data stimdg}
        close $fd
        stim_file $load
        file delete -force $load
    }

    proc restore_live {host} {
        set path /tmp/trial_video_restore.tcl
        set fd [open $path w]
        puts $fd [list set ::dservhost $host]
        puts $fd {
catch { stimPlayback 0 }
if {[info exists ::dservhost] && $::dservhost ne ""} {
    package require qpcs
    qpcs::dsStimRegister $::dservhost
    if {![info exists ::dsCmds(stimdg)]} {
        proc readdg {args} { dg_fromString64 [lindex $args 4] }
        set ::dsCmds(stimdg) readdg
    }
    foreach name [array names ::dsCmds] {
        catch { qpcs::dsStimAddMatch $::dservhost $name }
    }
}
}
        close $fd
        set err ""
        catch {stim_file $path} err
        file delete -force $path
        return $err
    }

    proc emit_record {fd rec} {
        set kind [dict get $rec kind]
        set body [dict get $rec body]
        if {$kind eq "cmd"} {
            if {[string match "omitted *" $body]} { return 0 }
            puts $fd $body
            if {[lindex $body 0] eq "show_response"} { return 2 }
            return 1
        }
        if {$kind eq "ds"} {
            if {$body eq "stimdg" || [string match "stimdg *" $body]} {
                return 0
            }
            puts $fd [list _tape_ds $body]
            return 1
        }
        return 0
    }

    proc check_clock {seenfile} {
        variable frame_ms
        set fd [open $seenfile r]
        set rows {}
        while {[gets $fd line] >= 0} {
            if {$line eq ""} continue
            lappend rows $line
        }
        close $fd
        set held 0
        set wiped 0
        set show {}
        foreach line $rows {
            lassign $line T pin seen mark
            if {$mark} {
                set show [dict create T $T pin $pin seen $seen]
            }
            if {abs($seen - $pin) < 0.05} {
                incr held
            } elseif {abs($seen) < 0.5} {
                incr wiped
            } else {
                return -code error \
                    "StimTimeF $seen did not hold tape pin $pin at StimTicksF $T"
            }
        }
        if {[llength $rows] == 0} {
            return -code error "playback produced no frames"
        }
        if {$wiped > 4} {
            return -code error \
                "StimTimeF was cleared on $wiped frames; the tape pin is not sticking"
        }
        if {$held < 1} {
            return -code error "StimTimeF never held the tape pin"
        }
        if {[dict exists $show seen]} {
            set pin [dict get $show pin]
            set seen [dict get $show seen]
            if {abs($seen - $pin) >= 0.05} {
                return -code error \
                    "show_response frame saw StimTimeF $seen, tape pin $pin"
            }
        }
        return [dict create frames [llength $rows] clock_held $held \
                    reset_frames $wiped show $show]
    }

    # Display period of one observation, in milliseconds. The median gap
    # between tape times that looks like a single swap. A 60 Hz grid is
    # the fallback when the tape has no such gap.
    proc swap_period {recs} {
        set prev ""
        set gaps {}
        foreach rec $recs {
            set tk [dict get $rec ticks]
            if {$prev ne ""} {
                set d [expr {$tk - $prev}]
                if {$d >= 8.0 && $d <= 40.0} { lappend gaps $d }
            }
            if {$prev eq "" || $tk > $prev} { set prev $tk }
        }
        if {[llength $gaps] == 0} { return [expr {1000.0 / 60.0}] }
        set gaps [lsort -real $gaps]
        return [lindex $gaps [expr {[llength $gaps] / 2}]]
    }

    # "setBackground r g b" from one inbox command, or "" when it is not one.
    proc background_rgb {kind body} {
        if {$kind ne "cmd" || [llength $body] != 4} { return "" }
        if {[lindex $body 0] ne "setBackground"} { return "" }
        foreach c [lrange $body 1 3] {
            if {![string is integer -strict $c] || $c < 0 || $c > 255} {
                return ""
            }
        }
        return [lrange $body 1 3]
    }

    # "r g b" from one ess/stim_background value, or "" when it is not one.
    # A string point comes back as one element holding all three numbers.
    proc background_stored {raw} {
        if {[llength $raw] == 1} { set raw [lindex $raw 0] }
        if {[llength $raw] != 3} { return "" }
        foreach c $raw {
            if {![string is integer -strict $c] || $c < 0 || $c > 255} {
                return ""
            }
        }
        return $raw
    }

    # Last setBackground at or before trial start. Trial start is BEGINOBS.
    # A command from earlier in the file counts. With no BEGINOBS, the first
    # inbox time of the trial stands in for the start. If the inbox has none,
    # the color stim was holding when the file opened counts. "" when neither
    # was stored.
    proc background_before {g names ts trial} {
        set ends {}
        set begins {}
        set i 0
        foreach name $names t $ts {
            if {[string match {evt:19:*} $name]} {
                lappend begins $t
            } elseif {[string match {evt:20:*} $name]} {
                lappend ends $t
            }
            incr i
        }
        if {$trial >= [llength $ends]} {
            return -code error \
                "trial_video: trial $trial outside 0..[expr {[llength $ends] - 1}]"
        }
        set start -1e300
        if {$trial > 0} { set start [lindex $ends [expr {$trial - 1}]] }
        set end [lindex $ends $trial]
        set begin_t [begin_logger $begins $start $end]

        set first_t ""
        set i 0
        foreach name $names t $ts {
            if {$name eq "stim/inbox" && $t > $start && $t <= $end} {
                if {$first_t eq "" || $t < $first_t} { set first_t $t }
            }
            incr i
        }
        if {$first_t eq ""} {
            return -code error "trial_video: trial $trial has no stim/inbox records"
        }
        set limit $begin_t
        if {$limit eq ""} { set limit $first_t }

        set bg ""
        set bg_t -1e300
        set i 0
        foreach name $names t $ts {
            if {$name eq "stim/inbox" && $t <= $limit} {
                set rec [parse_inbox [lindex [dl_tcllist [dl_get $g:vals $i]] 0]]
                set rgb [background_rgb [dict get $rec kind] [dict get $rec body]]
                if {$rgb ne "" && $t >= $bg_t} {
                    set bg $rgb
                    set bg_t $t
                }
            }
            incr i
        }
        if {$bg ne ""} { return $bg }
        set i 0
        foreach name $names t $ts {
            if {$name eq "ess/stim_background" && $t <= $limit && $t >= $bg_t} {
                set rgb [background_stored [dl_tcllist [dl_get $g:vals $i]]]
                if {$rgb ne ""} {
                    set bg $rgb
                    set bg_t $t
                }
            }
            incr i
        }
        return $bg
    }

    # missing 1 when neither the inbox nor the file-open color is stored.
    proc background {ess_path trial} {
        if {![file exists $ess_path]} {
            return -code error "trial_video: no such file $ess_path"
        }
        if {![string is integer -strict $trial] || $trial < 0} {
            return -code error "trial_video: trial must be a non-negative integer"
        }
        catch {dg_delete $ess_path}
        set g [dslog::read $ess_path]
        set names [dl_tcllist $g:varname]
        set ts [dl_tcllist $g:timestamp]
        if {[lsearch -exact $names stim/inbox] < 0} {
            catch {dg_delete $g}
            return -code error "trial_video: no stim/inbox tape in [file tail $ess_path]"
        }
        if {[catch {set rgb [background_before $g $names $ts $trial]} err]} {
            catch {dg_delete $g}
            return -code error $err
        }
        catch {dg_delete $g}
        if {$rgb eq ""} {
            return [dict create missing 1]
        }
        return [dict create missing 0 color $rgb]
    }

    proc render {ess_path trial out_mp4} {
        variable frame_ms
        if {![file exists $ess_path]} {
            return -code error "trial_video: no such file $ess_path"
        }
        if {![string is integer -strict $trial] || $trial < 0} {
            return -code error "trial_video: trial must be a non-negative integer"
        }

        catch {dg_delete $ess_path}
        set g [dslog::read $ess_path]
        set names [dl_tcllist $g:varname]
        set ts [dl_tcllist $g:timestamp]
        if {[lsearch -exact $names stim/inbox] < 0} {
            catch {dg_delete $g}
            return -code error "trial_video: no stim/inbox tape in [file tail $ess_path]"
        }

        set ends {}
        set begins {}
        set stimdg_i -1
        set stimdg_t -1e300
        set i 0
        foreach name $names t $ts {
            # ENDOBS is evt type 20. logger:endobs is only a copy of that
            # timestamp, and only when the file has an obs-limited stream.
            # BEGINOBS is evt type 19, and it is time zero for the movie.
            if {[string match {evt:19:*} $name]} {
                lappend begins $t
            } elseif {[string match {evt:20:*} $name]} {
                lappend ends $t
            } elseif {$name eq "stimdg" && $t >= $stimdg_t} {
                set stimdg_i $i
                set stimdg_t $t
            }
            incr i
        }
        if {$trial >= [llength $ends]} {
            catch {dg_delete $g}
            return -code error \
                "trial_video: trial $trial outside 0..[expr {[llength $ends] - 1}]"
        }

        set start -1e300
        if {$trial > 0} { set start [lindex $ends [expr {$trial - 1}]] }
        set end [lindex $ends $trial]
        set begin_t [begin_logger $begins $start $end]

        # The logged body is the script that was sourced. A block from
        # before that point has only a path, and plays whatever file is
        # there now.
        set path ""
        set path_t -1e300
        set script_body ""
        set body_t -1e300
        set i 0
        foreach name $names t $ts {
            if {$name eq "ess/stim_script_body" && $t <= $end && $t >= $body_t} {
                set script_body [lindex [dl_tcllist [dl_get $g:vals $i]] 0]
                set body_t $t
            } elseif {$name eq "ess/stim_script_path" && $t <= $end && $t >= $path_t} {
                set path [lindex [dl_tcllist [dl_get $g:vals $i]] 0]
                set path_t $t
            }
            incr i
        }
        if {$script_body eq ""} {
            if {$path eq ""} {
                catch {dg_delete $g}
                return -code error "trial_video: tape has no stim script"
            }
            if {![file exists $path]} {
                catch {dg_delete $g}
                return -code error "trial_video: stim script not found: $path"
            }
        }

        set recs {}
        set i 0
        foreach name $names t $ts {
            if {$name eq "stim/inbox" && $t > $start && $t <= $end} {
                set rec [parse_inbox [lindex [dl_tcllist [dl_get $g:vals $i]] 0]]
                dict set rec t $t
                lappend recs $rec
            }
            incr i
        }
        if {[llength $recs] == 0} {
            catch {dg_delete $g}
            return -code error "trial_video: trial $trial has no stim/inbox records"
        }
        set recs [lsort -command {apply {{a b} {
            set d [expr {[dict get $a seq] - [dict get $b seq]}]
            expr {$d < 0 ? -1 : $d > 0 ? 1 : 0}
        }}} $recs]

        set next_t $end
        foreach rec $recs {
            if {[dict get $rec kind] eq "cmd" &&
                [lindex [dict get $rec body] 0] eq "nexttrial"} {
                set next_t [dict get $rec t]
                break
            }
        }
        if {$stimdg_i < 0 || $stimdg_t > $next_t} {
            set stimdg_i -1
            set stimdg_t -1e300
            set i 0
            foreach name $names t $ts {
                if {$name eq "stimdg" && $t <= $next_t && $t >= $stimdg_t} {
                    set stimdg_i $i
                    set stimdg_t $t
                }
                incr i
            }
        }
        if {$stimdg_i < 0} {
            catch {dg_delete $g}
            return -code error "trial_video: no stimdg at or before trial $trial"
        }

        set need {}
        foreach rec $recs {
            if {[dict get $rec kind] ne "ds"} continue
            set body [dict get $rec body]
            if {$body eq "stimdg" || [string match "stimdg *" $body]} continue
            set pname [lindex $body 0]
            if {$pname ni $need} { lappend need $pname }
        }

        if {[catch {set bg_rgb [background_before $g $names $ts $trial]} err]} {
            catch {dg_delete $g}
            return -code error $err
        }
        set bg_missing [expr {$bg_rgb eq ""}]

        set outdir [file dirname $out_mp4]
        file mkdir $outdir
        set work [file join $outdir _play]
        file delete -force $work
        file mkdir $work
        set dgbin [file join $work stimdg.bin]
        write_stimdg $g $stimdg_i $dgbin
        catch {dg_delete $g}
        set g ""

        if {[catch {set host [dservGet ess/ipaddr]}]} {
            set host [string trim [exec essctrl -s ess -c {dservGet ess/ipaddr}]]
        }
        if {$host eq ""} {
            file delete -force $work
            return -code error "trial_video: ess/ipaddr is empty"
        }

        set info ""
        set failed 0
        set err ""
        set ff ""
        set hold ""
        if {[catch {
            set stimfile $path
            if {$script_body ne ""} {
                set stimfile [file join $work stim.tcl]
                set fd [open $stimfile w]
                puts -nonewline $fd $script_body
                close $fd
            }
            set handlers [install_stim $host $stimfile $work]
            foreach pname $need {
                if {$pname ni $handlers} {
                    return -code error \
                        "trial_video: stim file registered no handler for $pname"
                }
            }
            load_stimdg $dgbin

            set w [expr {int([stim {return [screen_set WinWidth]}])}]
            set h [expr {int([stim {return [screen_set WinHeight]}])}]

            set t_first [dict get [lindex $recs 0] ticks]
            set t_swap [dict get [lindex $recs end] ticks]
            # ENDOBS is later than the last swap when the display sits still.
            # Hold that last picture until the observation ends. Sample times
            # stay on the swaps, so stim time is not run past the tape.
            # Time zero is BEGINOBS. nexttrial is earlier, and is only a seed.
            set pairs [clock_pairs $recs]
            set t_end [ticks_at $pairs $end]
            if {$t_end < $t_swap} { set t_end $t_swap }
            set t0 [origin_tick $pairs $begin_t $t_first $t_swap]
            set t1 $t_swap
            # One redraw per original swap. A longer quiet stretch is split
            # into steps of that same period so the clock never jumps far
            # enough for an object to throw the step away.
            set step_ms [swap_period $recs]
            set times [list $t0]
            set T $t0
            while {1} {
                set next [expr {$T + $step_ms}]
                if {$next >= $t1 - 0.5} {
                    if {$t1 > $T + 0.01} { lappend times $t1 }
                    break
                }
                lappend times $next
                set T $next
            }
            set n_native [llength $times]
            # The file stays 60 fps. Each captured picture is repeated across
            # the output slots it covers, so a 50 Hz swap still plays in the
            # viewer without a second object update. The last picture also
            # fills every slot through ENDOBS.
            set n_out [expr {int(ceil(($t_end - $t0) / $frame_ms)) + 1}]
            if {$n_out < 1} { set n_out 1 }
            set reps {}
            set out_k 0
            for {set i 0} {$i < $n_native} {incr i} {
                if {$i + 1 < $n_native} {
                    set limit [lindex $times [expr {$i + 1}]]
                } else {
                    set limit [expr {$t1 + $frame_ms}]
                }
                set nrep 0
                while {$out_k < $n_out} {
                    set tk [expr {$t0 + $out_k * $frame_ms}]
                    if {$i + 1 < $n_native && $tk >= $limit} break
                    incr nrep
                    incr out_k
                }
                lappend reps $nrep
            }

            set seenfile [file join $work clock.txt]
            set ff_log [file join $work ffmpeg.log]
            # One script, one fifo. The hold keeps both ends from blocking
            # before the other arrives, and closing it is what tells ffmpeg
            # the pictures are done. Same encoder flags as before: a keyframe
            # every second, no B-frames, no edit list, timescale matching
            # the frames. Firefox stalls without those.
            set fifo [file join $work frames.fifo]
            exec mkfifo $fifo
            set hold [open $fifo RDWR]
            fconfigure $hold -translation binary -blocking 1
            forget_layered $out_mp4
            set ff [open [list |ffmpeg -y -loglevel error \
                -f rawvideo -pixel_format rgba -video_size ${w}x${h} \
                -framerate 60 -i $fifo \
                -c:v libx264 -pix_fmt yuv420p \
                -bf 0 -g 60 -keyint_min 60 -sc_threshold 0 \
                -use_editlist 0 -movie_timescale 15360 \
                -movflags +faststart $out_mp4 2> $ff_log] w]
            fconfigure $ff -translation binary -blocking 1

            set rec0 [lindex $recs 0]
            set base [dict get $rec0 timef]
            set base_ticks [dict get $rec0 ticks]
            if {[dict get $rec0 kind] eq "reset"} { set base 0.0 }
            set show_cmd_time ""
            set show_pin ""
            set idx 0
            set nrec [llength $recs]
            set play_path [file join $work play.tcl]
            set play_fd [open $play_path w]
            puts $play_fd "stimPlayback 1"
            # The color is the last one the tape set by trial start. Black is
            # stim's own start, used only when the tape never set one.
            if {$bg_missing} {
                puts $play_fd [list setBackground 0 0 0]
            } else {
                puts $play_fd [list setBackground {*}$bg_rgb]
            }

            # Inbox records before BEGINOBS set the scene (background, the
            # hidden object list). Step the stim clock across that gap so a
            # long interblock is not one jump, and do not capture it.
            set stepped ""
            while {$idx < $nrec} {
                set rec [lindex $recs $idx]
                if {[dict get $rec ticks] >= $t0} break
                set rt [dict get $rec ticks]
                if {$stepped ne "" && $rt > $stepped + 0.01} {
                    write_clock_steps $play_fd $base $base_ticks \
                        $stepped $rt $step_ms
                }
                set base_ticks $rt
                if {[dict get $rec kind] eq "reset"} {
                    set base 0.0
                } else {
                    set base [dict get $rec timef]
                }
                emit_record $play_fd $rec
                set pin [expr {$base + ($rt - $base_ticks)}]
                puts $play_fd [list stimTimeSet $pin]
                set stepped $rt
                incr idx
            }
            if {$stepped ne "" && $t0 > $stepped + 0.01} {
                write_clock_steps $play_fd $base $base_ticks \
                    $stepped $t0 $step_ms
            }

            for {set fi 0} {$fi < $n_native} {incr fi} {
                set T [lindex $times $fi]
                set marked 0
                while {$idx < $nrec} {
                    set rec [lindex $recs $idx]
                    if {[dict get $rec ticks] > $T} break
                    set base_ticks [dict get $rec ticks]
                    if {[dict get $rec kind] eq "reset"} {
                        set base 0.0
                    } else {
                        set base [dict get $rec timef]
                    }
                    if {[emit_record $play_fd $rec] == 2} {
                        set marked 1
                        set show_cmd_time [dict get $rec timef]
                    }
                    incr idx
                }
                set pin [expr {$base + ($T - $base_ticks)}]
                if {$marked} { set show_pin $pin }
                set nrep [lindex $reps $fi]
                puts $play_fd [list set ::tape_mark $marked]
                puts $play_fd [list stimTimeSet $pin]
                puts $play_fd "diagShow off"
                puts $play_fd [list captureFrame $fifo $nrep]
                puts $play_fd [list _tape_seen $seenfile $T $pin]
            }
            close $play_fd
            stim_run $play_path
            # The fifo's last writer has to go before ffmpeg will finish.
            close $hold
            set hold ""
            close $ff
            set ff ""
            set info [check_clock $seenfile]
            if {$show_cmd_time ne ""} {
                if {abs($show_pin - $show_cmd_time) > $step_ms + 1.0} {
                    return -code error \
                        "show_response pin $show_pin is more than a frame from tape time $show_cmd_time"
                }
                dict set info show_pin $show_pin
                dict set info show_tape $show_cmd_time
            }
            dict set info width $w
            dict set info height $h
            dict set info background_missing $bg_missing
            dict set info out $out_mp4
            dict set info step_ms $step_ms
            dict set info output_frames $n_out
        } err]} {
            set failed 1
        }
        # A fast failure closes the fifo's writer before ffmpeg has opened
        # it, and ffmpeg then waits forever. Stop it instead of waiting.
        if {$failed && $ff ne ""} {
            catch {exec kill -9 [pid $ff]}
        }
        catch {close $hold}
        catch {close $ff}
        catch {restore_live $host}
        file delete -force $work
        if {$failed} {
            catch {file delete -force $out_mp4}
            return -code error $err
        }
        return $info
    }

    # Drop layered copies of one clean trial so a fresh render is not served
    # from the previous bake.
    proc forget_layered {out_mp4} {
        set dir [file dirname $out_mp4]
        set stem [file rootname [file tail $out_mp4]]
        foreach f [glob -nocomplain -directory $dir ${stem}.*.mp4] {
            file delete -force $f
        }
    }

    # Same visibility as the viewer's marksAt. A press lasts until its
    # release. A press with no release, or one replaced by another press,
    # lasts touch_hold seconds.
    proc touch_intervals {events} {
        variable touch_hold
        set marks {}
        set open ""
        foreach ev $events {
            set kind [dict get $ev kind]
            if {$kind eq "press"} {
                if {$open ne ""} {
                    lappend marks [dict create \
                        t0 [dict get $open t] \
                        t1 [expr {[dict get $open t] + $touch_hold}] \
                        x [dict get $open x] \
                        y [dict get $open y]]
                }
                set open $ev
            } elseif {$kind eq "release" && $open ne ""} {
                set t0 [dict get $open t]
                set t1 [dict get $ev t]
                if {$t1 < $t0} {
                    set t1 [expr {$t0 + $touch_hold}]
                }
                lappend marks [dict create t0 $t0 t1 $t1 \
                    x [dict get $open x] y [dict get $open y]]
                set open ""
            }
        }
        if {$open ne ""} {
            lappend marks [dict create \
                t0 [dict get $open t] \
                t1 [expr {[dict get $open t] + $touch_hold}] \
                x [dict get $open x] \
                y [dict get $open y]]
        }
        return $marks
    }

    proc video_size {path} {
        if {[catch {exec ffprobe -v error -select_streams v:0 \
                -show_entries stream=width,height -of csv=p=0 $path} out]} {
            return -code error "trial_video: ffprobe: $out"
        }
        set out [string trim $out]
        if {![regexp {^(\d+),(\d+)$} $out -> w h]} {
            return -code error "trial_video: bad video size '$out'"
        }
        return [list $w $h]
    }

    proc write_touch_filter {path marks size {input "1:v"} {tail "out"} {head "0:v"}} {
        set n [llength $marks]
        set parts {}
        if {$n == 1} {
            lappend parts "\[${input}\]scale=${size}:${size}:flags=lanczos,format=rgba\[m0\]"
        } else {
            set outs {}
            for {set i 0} {$i < $n} {incr i} {
                lappend outs "\[m$i\]"
            }
            lappend parts "\[${input}\]scale=${size}:${size}:flags=lanczos,format=rgba,split=${n}[join $outs ""]"
        }
        set prev $head
        for {set i 0} {$i < $n} {incr i} {
            set m [lindex $marks $i]
            set x [format %.2f [dict get $m x]]
            set y [format %.2f [dict get $m y]]
            set t0 [format %.6f [dict get $m t0]]
            set t1 [format %.6f [dict get $m t1]]
            set dest [expr {$i + 1 == $n ? $tail : "v$i"}]
            lappend parts "\[$prev\]\[m$i\]overlay=x='${x}-overlay_w/2':y='${y}-overlay_h/2':enable='between(t,${t0},${t1})':eof_action=repeat\[$dest\]"
            set prev "v$i"
        }
        set fd [open $path w]
        puts -nonewline $fd [join $parts ";"]
        close $fd
    }

    # One center per video frame. "-" is a frame that has no sample yet.
    # The painter turns this into a single transparent frame per step.
    proc write_eye_moves {path moves} {
        set fd [open $path w]
        foreach m $moves {
            lassign $m _ x y
            if {$x eq ""} {
                puts $fd -
            } else {
                puts $fd "$x $y"
            }
        }
        close $fd
    }

    # The trail is already painted into input, full frame, so one overlay.
    proc write_eye_filter {path input head tail} {
        set parts [list \
            "\[${head}\]\[${input}\]overlay=x=0:y=0:format=auto:eof_action=pass\[${tail}\]"]
        set prefix ""
        if {[file exists $path] && [file size $path] > 0} {
            set prefix ";"
        }
        set fd [open $path a]
        puts -nonewline $fd $prefix
        puts -nonewline $fd [join $parts ";"]
        close $fd
    }

    proc video_duration {path} {
        if {[catch {exec ffprobe -v error -show_entries format=duration \
                -of default=nw=1:nk=1 $path} out]} {
            return -code error "trial_video: ffprobe: $out"
        }
        set out [string trim $out]
        if {![string is double -strict $out] || $out <= 0} {
            return -code error "trial_video: bad duration '$out'"
        }
        return $out
    }

    # Latest sample at each video frame. x and y are already video pixels.
    proc eye_frame_moves {samples duration} {
        variable frame_ms
        set step [expr {$frame_ms / 1000.0}]
        set n [llength $samples]
        set nframes [expr {int(ceil($duration / $step - 1e-9))}]
        if {$nframes < 1} { set nframes 1 }
        set moves {}
        set idx -1
        for {set f 0} {$f < $nframes} {incr f} {
            set t [expr {$f * $step}]
            while {$idx + 1 < $n && [lindex $samples $idx+1 0] <= $t + 1e-6} {
                incr idx
            }
            if {$idx < 0} {
                lappend moves [list $t "" ""]
            } else {
                set s [lindex $samples $idx]
                lappend moves [list $t [lindex $s 1] [lindex $s 2]]
            }
        }
        return $moves
    }

    # Paint the requested layers onto a copy of an existing stim mp4.
    # layers is touch, eye, or both. A layer with nothing to draw is left
    # out of the filename. Nothing to draw leaves the clean file untouched.
    proc burn {ess_path trial clean_mp4 {layers touch}} {
        variable touch_png
        variable eye_trail_py
        variable eye_trail_fade
        variable eye_trail_gap
        variable eye_trail_n
        variable frame_ms
        if {![file exists $ess_path]} {
            return -code error "trial_video: no such file $ess_path"
        }
        if {![string is integer -strict $trial] || $trial < 0} {
            return -code error "trial_video: trial must be a non-negative integer"
        }
        if {![file exists $clean_mp4]} {
            return -code error "trial_video: no video for trial $trial"
        }
        set want_touch [expr {"touch" in $layers}]
        set want_eye [expr {"eye" in $layers}]
        if {!$want_touch && !$want_eye} {
            return [dict create out $clean_mp4 layered 0]
        }
        if {$want_touch && ![file exists $touch_png]} {
            return -code error "trial_video: missing touch mark $touch_png"
        }
        if {$want_eye && ![file exists $eye_trail_py]} {
            return -code error "trial_video: missing eye painter $eye_trail_py"
        }

        set marks {}
        set space_w 1280
        set space_h 800
        if {$want_touch} {
            set info [touches $ess_path $trial]
            set marks [touch_intervals [dict get $info events]]
            set space_w [dict get $info width]
            set space_h [dict get $info height]
        }
        set eye_samples {}
        if {$want_eye} {
            set einfo [eyes $ess_path $trial]
            set eye_samples [dict get $einfo samples]
            if {!$want_touch} {
                set space_w [dict get $einfo width]
                set space_h [dict get $einfo height]
            }
        }
        set paint_touch [expr {[llength $marks] > 0}]
        set paint_eye [expr {[llength $eye_samples] > 0}]
        if {!$paint_touch && !$paint_eye} {
            return [dict create out $clean_mp4 layered 0]
        }

        set suffix ""
        if {$paint_touch} { append suffix ".touch" }
        if {$paint_eye} { append suffix ".eye" }
        set final "[file rootname $clean_mp4]$suffix.mp4"
        set fresh [file mtime $clean_mp4]
        if {$paint_touch} {
            set mark_time [file mtime $touch_png]
            if {$mark_time > $fresh} { set fresh $mark_time }
        }
        if {$paint_eye} {
            set mark_time [file mtime $eye_trail_py]
            if {$mark_time > $fresh} { set fresh $mark_time }
            set code_time [file mtime [file join [file dirname $eye_trail_py] trial_video-1.0.tm]]
            if {$code_time > $fresh} { set fresh $code_time }
        }
        if {[file exists $final] && [file mtime $final] >= $fresh} {
            return [dict create out $final layered 1]
        }

        lassign [video_size $clean_mp4] vw vh
        if {$space_w <= 0} { set space_w $vw }
        if {$space_h <= 0} { set space_h $vh }
        set sx [expr {double($vw) / double($space_w)}]
        set sy [expr {double($vh) / double($space_h)}]

        set stem [file rootname $clean_mp4]
        set part "${stem}${suffix}.part.mp4"
        set script "${stem}${suffix}.filter"
        set movesf "${stem}${suffix}.moves"
        file delete -force $part $script $movesf

        set inputs [list -i $clean_mp4]
        set next_in 1
        set head "0:v"
        if {$paint_touch} {
            set touch_in $next_in
            incr next_in
            lappend inputs -i $touch_png
            set tsize [expr {max(1, int(round(56.0 * $sx)))}]
            set scaled {}
            foreach m $marks {
                lappend scaled [dict create \
                    t0 [dict get $m t0] t1 [dict get $m t1] \
                    x [expr {[dict get $m x] * $sx}] \
                    y [expr {[dict get $m y] * $sy}]]
            }
            set tail [expr {$paint_eye ? "base" : "out"}]
            write_touch_filter $script $scaled $tsize "${touch_in}:v" $tail
            set head "base"
        }
        if {$paint_eye} {
            set esize [expr {max(1, int(round(8.0 * $sx)))}]
            set scaled {}
            foreach s $eye_samples {
                lappend scaled [list [dict get $s t] \
                    [expr {[dict get $s x] * $sx}] \
                    [expr {[dict get $s y] * $sy}]]
            }
            set duration [video_duration $clean_mp4]
            write_eye_moves $movesf [eye_frame_moves $scaled $duration]
            write_eye_filter $script "${next_in}:v" $head out
        }

        set ff [list ffmpeg -y -loglevel error {*}$inputs]
        if {$paint_eye} {
            set rate [format %.6f [expr {1000.0 / $frame_ms}]]
            lappend ff -f rawvideo -pix_fmt rgba -s ${vw}x${vh} -r $rate -i -
        }
        lappend ff -filter_complex_script $script \
            -map {[out]} \
            -c:v libx264 -pix_fmt yuv420p \
            -bf 0 -g 60 -keyint_min 60 -sc_threshold 0 \
            -use_editlist 0 -movie_timescale 15360 \
            -movflags +faststart $part
        if {$paint_eye} {
            set failed [catch {exec python3 -u $eye_trail_py $vw $vh $esize \
                $eye_trail_fade $eye_trail_gap $eye_trail_n < $movesf | {*}$ff} ff_err]
        } else {
            set failed [catch {exec {*}$ff} ff_err]
        }
        file delete -force $script $movesf
        if {$failed} {
            file delete -force $part
            return -code error "trial_video: ffmpeg: $ff_err"
        }
        file rename -force $part $final
        return [dict create out $final layered 1]
    }

    # Last BEGINOBS logger time in (start, end]. Empty when the window has none.
    proc begin_logger {begins start end} {
        set found ""
        foreach t $begins {
            if {$t > $start && $t <= $end} { set found $t }
        }
        return $found
    }

    # {logger_seconds ticks_ms} for one observation, sorted by logger time.
    proc clock_pairs {recs} {
        set pairs {}
        foreach rec $recs {
            lappend pairs [list [dict get $rec t] [dict get $rec ticks]]
        }
        return [lsort -command {apply {{a b} {
            set d [expr {[lindex $a 0] - [lindex $b 0]}]
            expr {$d < 0 ? -1 : $d > 0 ? 1 : 0}
        }}} $pairs]
    }

    # Stim tick of time zero. BEGINOBS when that tick is before the last
    # swap; the first inbox tick when this window has no BEGINOBS.
    proc origin_tick {pairs begin_t first_tick last_tick} {
        if {$begin_t eq ""} { return $first_tick }
        set t_begin [ticks_at $pairs $begin_t]
        if {$t_begin >= $last_tick} { return $first_tick }
        return $t_begin
    }

    # stimTimeSet from just after `from` through `to`, in steps of step_ms.
    # No picture is captured. A single jump across an interblock is what
    # the clock check rejects, so the seed walks the same period as playback.
    proc write_clock_steps {fd base base_ticks from to step_ms} {
        set T $from
        while {1} {
            set next [expr {$T + $step_ms}]
            if {$next >= $to - 0.5} {
                if {$to > $T + 0.01} {
                    set pin [expr {$base + ($to - $base_ticks)}]
                    puts $fd [list stimTimeSet $pin]
                }
                return
            }
            set pin [expr {$base + ($next - $base_ticks)}]
            puts $fd [list stimTimeSet $pin]
            set T $next
        }
    }

    # StimTicksF at a logger time, from the inbox records of one observation.
    # pairs are {logger_seconds ticks_ms}, sorted by logger time.
    proc ticks_at {pairs logger_t} {
        set n [llength $pairs]
        if {$n == 0} {
            return -code error "trial_video: no clock"
        }
        if {$n == 1} {
            return [expr {[lindex $pairs 0 1] + ($logger_t - [lindex $pairs 0 0]) * 1000.0}]
        }
        set lo 0
        for {set i 1} {$i < $n} {incr i} {
            if {[lindex $pairs $i 0] <= $logger_t} {
                set lo $i
            } else {
                break
            }
        }
        if {$lo == 0 && $logger_t < [lindex $pairs 0 0]} {
            set a 0
            set b 1
        } elseif {$lo >= $n - 1} {
            set a [expr {$n - 2}]
            set b [expr {$n - 1}]
        } else {
            set a $lo
            set b [expr {$lo + 1}]
        }
        set ta [lindex $pairs $a 0]
        set tb [lindex $pairs $b 0]
        set ka [lindex $pairs $a 1]
        set kb [lindex $pairs $b 1]
        set dt [expr {$tb - $ta}]
        if {abs($dt) < 1e-12} {
            return $ka
        }
        return [expr {$ka + ($logger_t - $ta) * ($kb - $ka) / $dt}]
    }

    # Presses and releases for one observation, in video seconds and frame pixels.
    # The framebuffer is 1280x800. Touch pixels are in ess/screen_w by ess/screen_h.
    proc touches {ess_path trial} {
        if {![file exists $ess_path]} {
            return -code error "trial_video: no such file $ess_path"
        }
        if {![string is integer -strict $trial] || $trial < 0} {
            return -code error "trial_video: trial must be a non-negative integer"
        }

        catch {dg_delete $ess_path}
        set g [dslog::read $ess_path]
        set names [dl_tcllist $g:varname]
        set ts [dl_tcllist $g:timestamp]
        if {[lsearch -exact $names stim/inbox] < 0} {
            catch {dg_delete $g}
            return -code error "trial_video: no stim/inbox tape in [file tail $ess_path]"
        }

        set ends {}
        set begins {}
        set i 0
        foreach name $names t $ts {
            if {[string match {evt:19:*} $name]} {
                lappend begins $t
            } elseif {[string match {evt:20:*} $name]} {
                lappend ends $t
            }
            incr i
        }
        if {$trial >= [llength $ends]} {
            catch {dg_delete $g}
            return -code error \
                "trial_video: trial $trial outside 0..[expr {[llength $ends] - 1}]"
        }
        set start -1e300
        if {$trial > 0} { set start [lindex $ends [expr {$trial - 1}]] }
        set end [lindex $ends $trial]
        set begin_t [begin_logger $begins $start $end]

        set frame_w 1280.0
        set frame_h 800.0
        set screen_w 0.0
        set screen_h 0.0
        set screen_wt -1e300
        set screen_ht -1e300
        set recs {}
        set raw_events {}
        set i 0
        foreach name $names t $ts {
            if {$name eq "stim/inbox" && $t > $start && $t <= $end} {
                set rec [parse_inbox [lindex [dl_tcllist [dl_get $g:vals $i]] 0]]
                dict set rec t $t
                lappend recs $rec
            } elseif {($name eq "evt:48:0" || $name eq "evt:48:1") && $t > $start && $t <= $end} {
                set xy [dl_tcllist [dl_get $g:vals $i]]
                lappend raw_events [list $t $name [lindex $xy 0] [lindex $xy 1]]
            } elseif {$name eq "ess/screen_w" && $t <= $end && $t >= $screen_wt} {
                set raw [lindex [dl_tcllist [dl_get $g:vals $i]] 0]
                if {[string is double -strict $raw] && $raw > 0} {
                    set screen_w $raw
                    set screen_wt $t
                }
            } elseif {$name eq "ess/screen_h" && $t <= $end && $t >= $screen_ht} {
                set raw [lindex [dl_tcllist [dl_get $g:vals $i]] 0]
                if {[string is double -strict $raw] && $raw > 0} {
                    set screen_h $raw
                    set screen_ht $t
                }
            }
            incr i
        }
        catch {dg_delete $g}
        if {[llength $recs] == 0} {
            return -code error "trial_video: trial $trial has no stim/inbox records"
        }
        if {$screen_w <= 0} { set screen_w $frame_w }
        if {$screen_h <= 0} { set screen_h $frame_h }

        set recs [lsort -command {apply {{a b} {
            set d [expr {[dict get $a seq] - [dict get $b seq]}]
            expr {$d < 0 ? -1 : $d > 0 ? 1 : 0}
        }}} $recs]
        set pairs [clock_pairs $recs]
        set t0 [origin_tick $pairs $begin_t \
                    [dict get [lindex $recs 0] ticks] \
                    [dict get [lindex $recs end] ticks]]

        set events {}
        foreach ev $raw_events {
            lassign $ev logger_t name x y
            if {![string is double -strict $x] || ![string is double -strict $y]} {
                continue
            }
            set ticks [ticks_at $pairs $logger_t]
            set kind [expr {$name eq "evt:48:0" ? "press" : "release"}]
            lappend events [dict create \
                kind $kind \
                t [expr {($ticks - $t0) / 1000.0}] \
                x [expr {$x * $frame_w / $screen_w}] \
                y [expr {$y * $frame_h / $screen_h}]]
        }
        set events [lsort -command {apply {{a b} {
            set d [expr {[dict get $a t] - [dict get $b t]}]
            if {abs($d) > 1e-9} {
                return [expr {$d < 0 ? -1 : 1}]
            }
            set ka [dict get $a kind]
            set kb [dict get $b kind]
            if {$ka eq $kb} { return 0 }
            if {$ka eq "press"} { return -1 }
            return 1
        }}} $events]

        return [dict create width [expr {int($frame_w)}] height [expr {int($frame_h)}] \
                    events $events]
    }

    # deg = c0 + c1*x + c2*y + c3*x^2 + c4*y^2 + c5*xy + c6*x^2*y + c7*x*y^2 + c8*x^2*y^2
    proc eye_biquad {x y coeffs} {
        lassign $coeffs c0 c1 c2 c3 c4 c5 c6 c7 c8
        set x2 [expr {$x * $x}]
        set y2 [expr {$y * $y}]
        set xy [expr {$x * $y}]
        return [expr {$c0 + $c1*$x + $c2*$y + $c3*$x2 + $c4*$y2 + $c5*$xy + \
                      $c6*$x2*$y + $c7*$x*$y2 + $c8*$x2*$y2}]
    }

    proc eye_coeffs_real {coeffs} {
        if {[llength $coeffs] != 9} { return 0 }
        foreach c $coeffs {
            if {![string is double -strict $c]} { return 0 }
            if {$c != 0} { return 1 }
        }
        return 0
    }

    # A logged string may come back as one element, or already as a dict list.
    proc logged_dict {vals} {
        if {[llength $vals] == 1} {
            set vals [lindex $vals 0]
        }
        if {[catch {dict size $vals}]} { return "" }
        return $vals
    }

    proc logged_word {vals} {
        if {[llength $vals] == 0} { return "" }
        return [lindex $vals 0]
    }

    # Last value whose time is at or before t. records are {time value}, sorted.
    proc value_at {records t} {
        set found ""
        foreach rec $records {
            if {[lindex $rec 0] <= $t} {
                set found [lindex $rec 1]
            } else {
                break
            }
        }
        return $found
    }

    # Camera raw is already the inverted P1-P4 difference. Analog raw is
    # post-swap ADC counts, and invert is applied after the linear scale.
    proc eye_to_deg {source settings h v} {
        if {$source eq "virtual"} {
            return [list $h $v]
        }
        set bq 0
        if {[dict exists $settings use_biquadratic]} {
            set bq [dict get $settings use_biquadratic]
        }
        set bqh {}
        set bqv {}
        if {[dict exists $settings bq_h_coeffs]} {
            set bqh [dict get $settings bq_h_coeffs]
        }
        if {[dict exists $settings bq_v_coeffs]} {
            set bqv [dict get $settings bq_v_coeffs]
        }
        if {$bq && [eye_coeffs_real $bqh] && [eye_coeffs_real $bqv]} {
            return [list [eye_biquad $h $v $bqh] [eye_biquad $h $v $bqv]]
        }
        set sh 1.0
        set sv 1.0
        set ch 0.0
        set cv 0.0
        set ih 0
        set iv 0
        foreach {key var} {scale_h sh scale_v sv raw_center_h ch raw_center_v cv \
                           invert_h ih invert_v iv} {
            if {[dict exists $settings $key]} {
                set $var [dict get $settings $key]
            }
        }
        set hd [expr {$sh * ($h - $ch)}]
        set vd [expr {$sv * ($v - $cv)}]
        if {$source eq "analog"} {
            if {$ih} { set hd [expr {-$hd}] }
            if {$iv} { set vd [expr {-$vd}] }
        }
        return [list $hd $vd]
    }

    # A raw log row is a batch of pairs stamped with the first sample.
    # Space the pairs up to the next row. The last row reuses the previous gap.
    proc expand_eye_batches {batches} {
        set n [llength $batches]
        set samples {}
        for {set i 0} {$i < $n} {incr i} {
            lassign [lindex $batches $i] t floats
            set pairs [expr {[llength $floats] / 2}]
            if {$pairs < 1} continue
            set dt 0.0
            if {$i + 1 < $n} {
                set next_t [lindex $batches $i+1 0]
                if {$next_t > $t} {
                    set dt [expr {($next_t - $t) / double($pairs)}]
                }
            } elseif {$i > 0} {
                lassign [lindex $batches $i-1] prev_t prev_floats
                set prev_pairs [expr {[llength $prev_floats] / 2}]
                if {$prev_pairs > 0 && $t > $prev_t} {
                    set dt [expr {($t - $prev_t) / double($prev_pairs)}]
                }
            }
            for {set k 0} {$k < $pairs} {incr k} {
                lappend samples [list \
                    [expr {$t + $k * $dt}] \
                    [lindex $floats [expr {$k * 2}]] \
                    [lindex $floats [expr {$k * 2 + 1}]]]
            }
        }
        return $samples
    }

    proc eye_source_ok {name} {
        expr {$name eq "video" || $name eq "analog" || $name eq "virtual"}
    }

    # Eye samples for one observation, in video seconds and frame pixels.
    # A sample with no source label, or a label this reader does not know, is skipped.
    proc eyes {ess_path trial} {
        if {![file exists $ess_path]} {
            return -code error "trial_video: no such file $ess_path"
        }
        if {![string is integer -strict $trial] || $trial < 0} {
            return -code error "trial_video: trial must be a non-negative integer"
        }

        catch {dg_delete $ess_path}
        set g [dslog::read $ess_path]
        set names [dl_tcllist $g:varname]
        set ts [dl_tcllist $g:timestamp]
        if {[lsearch -exact $names stim/inbox] < 0} {
            catch {dg_delete $g}
            return -code error "trial_video: no stim/inbox tape in [file tail $ess_path]"
        }

        set ends {}
        set begins {}
        foreach name $names t $ts {
            if {[string match {evt:19:*} $name]} {
                lappend begins $t
            } elseif {[string match {evt:20:*} $name]} {
                lappend ends $t
            }
        }
        if {$trial >= [llength $ends]} {
            catch {dg_delete $g}
            return -code error \
                "trial_video: trial $trial outside 0..[expr {[llength $ends] - 1}]"
        }
        set start -1e300
        if {$trial > 0} { set start [lindex $ends [expr {$trial - 1}]] }
        set end [lindex $ends $trial]
        set begin_t [begin_logger $begins $start $end]

        set frame_w 1280.0
        set frame_h 800.0
        set screen_w 0.0
        set screen_h 0.0
        set screen_hx 0.0
        set screen_hy 0.0
        set screen_wt -1e300
        set screen_ht -1e300
        set screen_hxt -1e300
        set screen_hyt -1e300
        set recs {}
        set batches {}
        set later ""
        set sources {}
        set settings_at {}
        set i 0
        foreach name $names t $ts {
            set vals [dl_tcllist [dl_get $g:vals $i]]
            if {$name eq "stim/inbox" && $t > $start && $t <= $end} {
                set rec [parse_inbox [lindex $vals 0]]
                dict set rec t $t
                lappend recs $rec
            } elseif {$name eq "eyetracking/raw" && $t > $start && $t <= $end} {
                lappend batches [list $t $vals]
            } elseif {$name eq "eyetracking/raw" && $t > $end} {
                if {$later eq "" || $t < [lindex $later 0]} {
                    set later [list $t $vals]
                }
            } elseif {$name eq "em/source_active"} {
                set word [logged_word $vals]
                if {[eye_source_ok $word]} {
                    lappend sources [list $t $word]
                }
            } elseif {$name eq "em/settings"} {
                set spec [logged_dict $vals]
                if {$spec ne ""} {
                    lappend settings_at [list $t $spec]
                }
            } elseif {$name eq "ess/screen_w" && $t <= $end && $t >= $screen_wt} {
                set raw [lindex $vals 0]
                if {[string is double -strict $raw] && $raw > 0} {
                    set screen_w $raw
                    set screen_wt $t
                }
            } elseif {$name eq "ess/screen_h" && $t <= $end && $t >= $screen_ht} {
                set raw [lindex $vals 0]
                if {[string is double -strict $raw] && $raw > 0} {
                    set screen_h $raw
                    set screen_ht $t
                }
            } elseif {$name eq "ess/screen_halfx" && $t <= $end && $t >= $screen_hxt} {
                set raw [lindex $vals 0]
                if {[string is double -strict $raw] && $raw > 0} {
                    set screen_hx $raw
                    set screen_hxt $t
                }
            } elseif {$name eq "ess/screen_halfy" && $t <= $end && $t >= $screen_hyt} {
                set raw [lindex $vals 0]
                if {[string is double -strict $raw] && $raw > 0} {
                    set screen_hy $raw
                    set screen_hyt $t
                }
            }
            incr i
        }
        catch {dg_delete $g}
        if {[llength $recs] == 0} {
            return -code error "trial_video: trial $trial has no stim/inbox records"
        }
        if {$screen_w <= 0} { set screen_w $frame_w }
        if {$screen_h <= 0} { set screen_h $frame_h }
        if {$screen_hx <= 0} { set screen_hx 16.0 }
        if {$screen_hy <= 0} { set screen_hy 16.0 }

        set recs [lsort -command {apply {{a b} {
            set d [expr {[dict get $a seq] - [dict get $b seq]}]
            expr {$d < 0 ? -1 : $d > 0 ? 1 : 0}
        }}} $recs]
        set pairs [clock_pairs $recs]
        set t0 [origin_tick $pairs $begin_t \
                    [dict get [lindex $recs 0] ticks] \
                    [dict get [lindex $recs end] ticks]]

        if {$later ne ""} { lappend batches $later }
        set batches [lsort -command {apply {{a b} {
            expr {[lindex $a 0] < [lindex $b 0] ? -1 : [lindex $a 0] > [lindex $b 0] ? 1 : 0}
        }}} $batches]
        set sources [lsort -command {apply {{a b} {
            expr {[lindex $a 0] < [lindex $b 0] ? -1 : [lindex $a 0] > [lindex $b 0] ? 1 : 0}
        }}} $sources]
        set settings_at [lsort -command {apply {{a b} {
            expr {[lindex $a 0] < [lindex $b 0] ? -1 : [lindex $a 0] > [lindex $b 0] ? 1 : 0}
        }}} $settings_at]
        set raw_samples [expand_eye_batches $batches]

        set samples {}
        foreach s $raw_samples {
            lassign $s logger_t h v
            if {$logger_t <= $start || $logger_t > $end} continue
            if {![string is double -strict $h] || ![string is double -strict $v]} continue
            set source [value_at $sources $logger_t]
            if {![eye_source_ok $source]} continue
            set spec [value_at $settings_at $logger_t]
            if {$source ne "virtual" && $spec eq ""} continue
            if {$spec eq ""} { set spec [dict create] }
            lassign [eye_to_deg $source $spec $h $v] deg_h deg_v
            set ticks [ticks_at $pairs $logger_t]
            set px [expr {$screen_w / 2.0 + $deg_h / $screen_hx * ($screen_w / 2.0)}]
            set py [expr {$screen_h / 2.0 - $deg_v / $screen_hy * ($screen_h / 2.0)}]
            lappend samples [dict create \
                t [expr {($ticks - $t0) / 1000.0}] \
                x [expr {$px * $frame_w / $screen_w}] \
                y [expr {$py * $frame_h / $screen_h}]]
        }
        set samples [lsort -command {apply {{a b} {
            set d [expr {[dict get $a t] - [dict get $b t]}]
            expr {$d < 0 ? -1 : $d > 0 ? 1 : 0}
        }}} $samples]
        return [dict create width [expr {int($frame_w)}] height [expr {int($frame_h)}] \
                    samples $samples]
    }

    # True when some raw sample has a video, analog, or virtual label at or before it.
    proc eye_covered {g names ts} {
        set sources {}
        set raws {}
        set i 0
        foreach name $names t $ts {
            if {$name eq "em/source_active"} {
                set word [logged_word [dl_tcllist [dl_get $g:vals $i]]]
                if {[eye_source_ok $word]} {
                    lappend sources [list $t $word]
                }
            } elseif {$name eq "eyetracking/raw"} {
                lappend raws $t
            }
            incr i
        }
        if {[llength $raws] == 0 || [llength $sources] == 0} { return 0 }
        set sources [lsort -command {apply {{a b} {
            expr {[lindex $a 0] < [lindex $b 0] ? -1 : [lindex $a 0] > [lindex $b 0] ? 1 : 0}
        }}} $sources]
        foreach t $raws {
            if {[eye_source_ok [value_at $sources $t]]} { return 1 }
        }
        return 0
    }

    # Layer names this file can actually draw. Touch is the press/release
    # events, whichever device produced them.
    proc layers {ess_path} {
        if {![file exists $ess_path]} {
            return -code error "trial_video: no such file $ess_path"
        }
        catch {dg_delete $ess_path}
        set g [dslog::read $ess_path]
        set names [dl_tcllist $g:varname]
        set ts [dl_tcllist $g:timestamp]
        set eye [eye_covered $g $names $ts]
        catch {dg_delete $g}
        set layers {}
        if {[lsearch -exact $names evt:48:0] >= 0 ||
            [lsearch -exact $names evt:48:1] >= 0} {
            lappend layers touch
        }
        if {$eye} {
            lappend layers eye
        }
        return $layers
    }
}

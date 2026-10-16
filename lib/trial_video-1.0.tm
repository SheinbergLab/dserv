# trial_video-1.0.tm — play one observation from a stim/inbox tape
#
# trial_video_make passes the .ess and a trial index. The player sources
# the stim file named in the tape, checks its hash, and feeds that trial's
# cmd and ds records back in seq order. StimTimeF is pinned from the tape
# (stimPlayback / stimTimeSet). There is no per-protocol plugin.
package provide trial_video 1.0

namespace eval trial_video {
    variable frame_ms [expr {1000.0 / 60.0}]
    variable tape_ready 1
    # dumpRaw keeps a single filename slot, filled when the display thread
    # later runs the request. A second dumpRaw in the same script overwrites
    # that slot before the first file is written, so each script dumps once.
    variable batch_frames 1

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

    proc render {ess_path trial out_mp4} {
        variable frame_ms
        variable batch_frames
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
        set path ""
        set hash ""
        set path_t -1e300
        set hash_t -1e300
        set stimdg_i -1
        set stimdg_t -1e300
        set i 0
        foreach name $names t $ts {
            # ENDOBS is evt type 20. logger:endobs is only a copy of that
            # timestamp, and only when the file has an obs-limited stream.
            if {[string match {evt:20:*} $name]} {
                lappend ends $t
            } elseif {$name eq "ess/stim_script_path" && $t >= $path_t} {
                set path [lindex [dl_tcllist [dl_get $g:vals $i]] 0]
                set path_t $t
            } elseif {$name eq "ess/stim_script_sha256" && $t >= $hash_t} {
                set hash [lindex [dl_tcllist [dl_get $g:vals $i]] 0]
                set hash_t $t
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
        if {$path eq "" || $hash eq ""} {
            catch {dg_delete $g}
            return -code error "trial_video: tape has no stim script path or hash"
        }
        if {![file exists $path]} {
            catch {dg_delete $g}
            return -code error "trial_video: stim script not found: $path"
        }
        set got [lindex [exec sha256sum $path] 0]
        if {$got ne $hash} {
            catch {dg_delete $g}
            return -code error \
                "trial_video: stim script hash differs: file $got tape $hash"
        }

        set start -1e300
        if {$trial > 0} { set start [lindex $ends [expr {$trial - 1}]] }
        set end [lindex $ends $trial]

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
        if {[catch {
            set handlers [install_stim $host $path $work]
            foreach pname $need {
                if {$pname ni $handlers} {
                    return -code error \
                        "trial_video: stim file registered no handler for $pname"
                }
            }
            load_stimdg $dgbin

            set w [expr {int([stim {return [screen_set WinWidth]}])}]
            set h [expr {int([stim {return [screen_set WinHeight]}])}]
            set expect [expr {20 + $w * $h * 4}]

            set t0 [dict get [lindex $recs 0] ticks]
            set t1 [dict get [lindex $recs end] ticks]
            set nframes [expr {int(ceil(($t1 - $t0) / $frame_ms)) + 1}]
            if {$nframes < 1} { set nframes 1 }

            set seenfile [file join $work clock.txt]
            set ff_log [file join $work ffmpeg.log]
            set ff [open [list |ffmpeg -y -loglevel error \
                -f rawvideo -pixel_format rgba -video_size ${w}x${h} \
                -framerate 60 -i - \
                -c:v libx264 -pix_fmt yuv420p $out_mp4 2> $ff_log] w]
            fconfigure $ff -translation binary -blocking 1 \
                -buffering full -buffersize 1048576

            set base 0.0
            set base_ticks $t0
            set show_cmd_time ""
            set show_pin ""
            set idx 0
            set nrec [llength $recs]
            set batch_fd ""
            set batch_n 0
            set batch_raws {}
            set batch_path [file join $work batch.tcl]

            proc flush_batch {path raws expect ff} {
                if {[llength $raws] == 0} return
                stim_file $path
                foreach raw $raws {
                    set deadline [expr {[clock milliseconds] + 3000}]
                    set size 0
                    while {[clock milliseconds] < $deadline} {
                        if {[file exists $raw]} { set size [file size $raw] }
                        if {$size >= $expect} break
                    }
                    if {$size < $expect} {
                        return -code error \
                            "dumpRaw incomplete for $raw (got $size, want $expect)"
                    }
                    set in [open $raw rb]
                    fconfigure $in -translation binary
                    read $in 20
                    fcopy $in $ff
                    close $in
                    file delete -force $raw
                }
            }

            for {set fi 0} {$fi < $nframes} {incr fi} {
                set T [expr {$t0 + $fi * $frame_ms}]
                if {$T > $t1} { set T $t1 }
                if {$batch_fd eq ""} {
                    set batch_fd [open $batch_path w]
                    puts $batch_fd "stimPlayback 1"
                    set batch_raws {}
                    set batch_n 0
                }
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
                    if {[emit_record $batch_fd $rec] == 2} {
                        set marked 1
                        set show_cmd_time [dict get $rec timef]
                    }
                    incr idx
                }
                set pin [expr {$base + ($T - $base_ticks)}]
                if {$marked} { set show_pin $pin }
                set raw [file join $work [format frame_%04d.raw $fi]]
                puts $batch_fd [list set ::tape_mark $marked]
                puts $batch_fd [list stimTimeSet $pin]
                puts $batch_fd "diagShow off"
                puts $batch_fd "redraw"
                puts $batch_fd [list dumpRaw $raw]
                puts $batch_fd [list _tape_seen $seenfile $T $pin]
                lappend batch_raws $raw
                incr batch_n
                if {$batch_n >= $batch_frames || $fi == $nframes - 1} {
                    close $batch_fd
                    set batch_fd ""
                    flush_batch $batch_path $batch_raws $expect $ff
                }
            }
            close $ff
            set info [check_clock $seenfile]
            if {$show_cmd_time ne ""} {
                if {abs($show_pin - $show_cmd_time) > $frame_ms + 1.0} {
                    return -code error \
                        "show_response pin $show_pin is more than a frame from tape time $show_cmd_time"
                }
                dict set info show_pin $show_pin
                dict set info show_tape $show_cmd_time
            }
            dict set info width $w
            dict set info height $h
            dict set info out $out_mp4
        } err]} {
            set failed 1
        }
        catch {close $ff}
        catch {restore_live $host}
        file delete -force $work
        if {$failed} {
            catch {file delete -force $out_mp4}
            return -code error $err
        }
        return $info
    }
}

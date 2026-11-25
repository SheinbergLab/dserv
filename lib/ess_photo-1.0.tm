# -*- mode: tcl -*-
#
# ess_photo-1.0.tm
#
# The photodiode, ESS's side (docs/photo_processor.md): the gear's `photo`
# section pushed to dserv's `photo` processor (processors/photo.c) or read
# from a comparator's DI line (`photo din`); stim2's photopatch drawn at
# each system load; every patch change paired with its light as the
# session runs (photo/pair, photo/verify); the green light (photo/health);
# and the placement test (photo_test, photo/test).
#
# Extracted from ess-2.0.tm (2026-11-24), the ess_transports way: the same
# `namespace eval ess`, so every cross-reference resolves at call time;
# ess-2.0.tm requires this module at load, and this module never requires
# ess back. ess-2.0.tm calls in at four points: stim_photopatch_enable
# (configure_stim), photo_log_match (file open), photo_obs_end (end_obs),
# photo_file_close (file close); ess_stim-1.0.tm's stim_frame_log_match
# logs stim/photopatch.
# The line resolver din_resolve is in ess_stim-1.0.tm, beside tlock's.
#
# The three blocks keep their BEGIN/END markers: the tests evaluate them
# by text, with everything outside stood in (tests/test_ess_photo*.tcl,
# test_ess_obs_hold.tcl).

package provide ess_photo 1.0

namespace eval ess {

    # BEGIN photo
    #
    # The photodiode edge detector (dserv's `photo` processor,
    # processors/photo.c, docs/photo_processor.md), set from the gear's
    # `photo` section (essconf.tcl). The processor keeps nothing across a
    # dserv restart; these settings do, and each is pushed to it as it
    # changes and once at boot. The processor is attached in dserv's main
    # interp (triggers.tcl) to every analog group; `group` picks the one it
    # reads, so changing it never re-attaches anything.
    #
    # processSetParam answers -1 when there is no such processor (a dserv
    # from before it): the settings then do nothing, and turning it on says
    # so once.
    variable photo_enabled 0
    variable photo_warned 0
    proc photo_param { p v } {
        expr { [processSetParam photo $p $v] >= 0 }
    }
    proc photo_group_norm {v} {
        set v [string trim $v]
        if { $v ne "" &&
             ![regexp {^[^/\s{}\\\[\]"$;*]+(/[^/\s{}\\\[\]"$;*]+)?$} $v] } {
            error "photo group: the analog group's label (photodiode), or\
 <box>/<label> to read one box only; empty reads every analog block"
        }
        return $v
    }
    proc photo_nonneg_norm {v} {
        set v [string trim $v]
        if { ![string is double -strict $v] || $v < 0 } {
            error "a number, 0 or more"
        }
        return $v
    }
    proc set_photo_enabled {on} {
        variable photo_enabled [expr {$on ? 1 : 0}]
        variable photo_warned
        variable photo_din
        catch { photo_din_attach }
        if { ![photo_param active $photo_enabled] && $photo_enabled &&
             $photo_din eq "" && !$photo_warned } {
            set photo_warned 1
            ::ess::ess_warning "photo enabled, but this dserv has no photo\
 processor (processors/photo.c): no light edges are detected" "photo"
        }
        catch { photo_health }
    }
    proc set_photo_group {v} {
        photo_param group [photo_group_norm $v]
        catch { photo_health }
    }
    proc set_photo_strobe {on} { photo_param strobe [expr {$on ? 1 : 0}] }
    proc set_photo_invert {on} { photo_param invert [expr {$on ? 1 : 0}] }
    # ms in the gear, seconds in the processor
    proc set_photo_period_ms {v} {
        photo_param period [expr {[photo_nonneg_norm $v] / 1000.0}]
    }
    proc set_photo_smooth_ms {v} {
        photo_param smooth [expr {[photo_nonneg_norm $v] / 1000.0}]
    }
    proc set_photo_settle_ms {v} {
        photo_param settle_dur [expr {[photo_nonneg_norm $v] / 1000.0}]
    }
    proc set_photo_min_step {v} {
        photo_param min_step [photo_nonneg_norm $v]
    }



    # THE LIGHT FROM A DIGITAL LINE (setting `photo din`). A comparator
    # module -- a photodiode, a pot for the threshold, a digital out: the
    # LM393 boards -- on an extio DI line, labelled. Each edge on the line
    # IS a light edge, stamped by the box when the line changed (the first
    # edge of a debounce window, so a debounce delays the report, never the
    # time). 1 is light: set the line active-low on the box where the
    # module pulls low in light, as the LM393 boards do.
    #
    # With it set, the pairing (photo_verify), the health light and
    # photo_test read the line, not the processor. The processor still runs
    # on its analog group if one streams, and proc/photo/edge is still
    # logged: a validation session can check the comparator against the
    # analog signal. The line's edges, the box's clock records and where
    # the label resolved (photo/din) go in every data file.
    #
    # Comparator edges are a fixed threshold on the light: steady to
    # ~0.05 ms while the light levels are, but a change of room light moves
    # the threshold's place on the transition, and the edge with it (0.5 ms
    # on an LCD, 2026-11-24). Shield the detector.
    variable photo_din ""
    variable photo_din_dp ""          ;# the datapoint subscribed to; "" none
    variable photo_din_n 0            ;# edges since dserv started
    variable photo_din_seq 0
    variable photo_din_last {}        ;# {value t} of the newest edge
    proc set_photo_din {v} {
        variable photo_din [string trim $v]
        photo_din_attach
        catch { photo_health }
    }
    # A chord group is no light line: the box settles its members into one
    # event, and edges a frame apart may merge
    proc photo_din_resolve {} {
        variable photo_din
        set r [din_resolve $photo_din]
        if { [dict get $r status] eq "ok" && [dict get $r kind] eq "group" } {
            return [dict create status unresolved kind group dp "" \
                        detail "$photo_din is a chord group, not a line:\
 label the comparator's DI pin itself"]
        }
        return $r
    }
    # Subscribe to the line while photo is enabled and it resolves.
    # Idempotent; called at each setting, system load and data file open,
    # as boxes come and go. Returns the datapoint, "" none.
    proc photo_din_attach {} {
        variable photo_enabled
        variable photo_din
        variable photo_din_dp
        set dp ""
        if { $photo_enabled && $photo_din ne "" } {
            set r [photo_din_resolve]
            if { [dict get $r status] eq "ok" } { set dp [dict get $r dp] }
        }
        if { $dp eq $photo_din_dp } { return $dp }
        if { $photo_din_dp ne "" } { catch { dservRemoveMatch $photo_din_dp } }
        set photo_din_dp $dp
        if { $dp ne "" } {
            dservAddExactMatch $dp
            dpointSetScript $dp ::ess::photo_din_edge
        }
        return $dp
    }
    proc photo_din_edge { dpoint data } {
        variable photo_din_n
        variable photo_din_last
        variable pv_on
        set t [now]
        catch { set t [dservTimestamp $dpoint] }
        incr photo_din_n
        set photo_din_last [list $data $t]
        if { $pv_on } {
            pv_edge [photo_din_row $data $t]
            pv_resolve [now]
        }
    }
    # a line's edge as a proc/photo/edge row: dir t50 t10 t90 old new
    # settled seq t_arm t_done -- one time, the box's stamp, for all three
    proc photo_din_row { v t } {
        variable photo_din_seq
        set v [expr {$v ? 1 : 0}]
        list [expr {$v ? 1 : -1}] $t $t $t [expr {1 - $v}] $v 1 \
            [incr photo_din_seq] $t $t
    }
    # photo/din, and the datafile matches: the line's edges, every one,
    # unbuffered, the box clock's records (see stim_tlock_din_match), and
    # the resolution (re-emitted once the logger runs: open_datafile's
    # dservTouch). Returns 1 when the edges were matched.
    proc photo_din_log_match { filename } {
        variable photo_din
        if { $photo_din eq "" } { return 0 }
        set r [photo_din_resolve]
        dservSet photo/din $r
        dservLoggerAddMatch $filename photo/din 0 0 1
        if { [dict get $r status] ne "ok" } {
            ::ess::ess_warning "photo din: [dict get $r detail] -- no light\
 edges in this data file" "photo"
            return 0
        }
        photo_din_attach
        set dp [dict get $r dp]
        dservLoggerAddMatch $filename $dp 0 0 1
        if { [regexp {^(.+/state)/di/[^/]+$} $dp -> base] } {
            dservLoggerAddMatch $filename $base/sync/* 0 0 1
            dservLoggerAddMatch $filename $base/in_obs 0 0 1
            # a line from the box's own comparator (pin mode cmp): its
            # learned levels and threshold, on change, and each settled
            # level of the light in counts (about one per patch change: the
            # analog's summary, with no stream). Absent on other boxes.
            # (re-emitted at open with photo/din: ess-2.0.tm open_datafile)
            dservLoggerAddMatch $filename $base/cmp 0 0 1
            dservLoggerAddMatch $filename $base/cmp/level 0 0 1
        }
        return 1
    }

    # The datafile match for proc/photo/edge (file_open), while the rig has
    # it on: not obs-limited, so an edge reported after its obs ended (the
    # block that finished it arrived later) is still kept -- its timestamp
    # (t50) puts it in its obs; unbuffered, so each edge datapoint keeps its
    # own timestamp and its rows stay rows. Returns 1 when added.
    proc photo_log_match { filename } {
        variable photo_enabled
        if { !$photo_enabled } { return 0 }
        dservLoggerAddMatch $filename proc/photo/edge 0 0 1
        # the offset that puts stim2's flip times (stim/photopatch,
        # stim/frame: CLOCK_REALTIME) on the dserv clock the edges are on
        dservLoggerAddMatch $filename ess/dserv_wall_us 0 0 1
        set off [dserv_wall_offset]
        dservSet ess/dserv_wall_us $off
        # each patch change paired with its light, and the verdict
        # (photo_verify): counted afresh for each file
        dservLoggerAddMatch $filename photo/pair 0 0 1
        dservLoggerAddMatch $filename photo/verify 0 0 1
        # the comparator's line, where the light comes from one
        photo_din_log_match $filename
        variable pv_on
        if { $pv_on } { pv_new_count $off }
        catch { photo_health }
        return 1
    }

    # dserv's clock minus this host's wall clock, in us: [now] and [clock
    # microseconds] read back to back, the tightest of a few brackets.
    # stim_evt_time maps a flip the same way, live; this is the value an
    # offline reader needs (tools/photopair). Steady over a session: both
    # clocks are slewed together, only a step changes it.
    proc dserv_wall_offset {} {
        set best {}
        for { set i 0 } { $i < 5 } { incr i } {
            set a [now]
            set w [clock microseconds]
            set b [now]
            if { $best eq "" || $b - $a < [lindex $best 0] } {
                set best [list [expr {$b - $a}] [expr {($a + $b) / 2 - $w}]]
            }
        }
        return [lindex $best 1]
    }

    # stim2's photopatch (stim2 docs/photopatch.md): a square in a corner
    # of the screen that changes between dark and light on chosen frames,
    # for the diode to sit over. Drawn only while the rig has both `photo
    # enabled` and `photo patch` on: a flashing corner distracts a subject,
    # so a rig without a diode over it never shows one. stim2's `postpass
    # reset` (every script load) turns it off, so like tlock it is turned on
    # again at each system load (configure_stim, stim_photopatch_enable),
    # and a changed setting takes effect at the next one.
    variable photo_patch 0
    variable photo_patch_opts [dict create -mode marked -corner tr -size 60 \
                                   -inset 0 -dark 0]
    proc set_photo_patch {on} {
        variable photo_patch [expr {$on ? 1 : 0}]
        catch { photo_health }
    }
    proc photo_patch_opt { opt v } {
        variable photo_patch_opts
        dict set photo_patch_opts $opt $v
    }
    proc photo_patch_int_norm { what lo v } {
        set v [string trim $v]
        if { ![string is integer -strict $v] || $v < $lo || $v > 4096 } {
            error "photo $what: pixels, $lo to 4096"
        }
        return [expr {int($v)}]
    }
    proc photo_patch_size_norm {v} { photo_patch_int_norm patch_size 1 $v }
    # px from both edges, or "x y": x from the corner's side edge, y from
    # its top or bottom (stim2 0.30.7 or later for two). One number when
    # they agree, so an older stim2 still takes it
    proc photo_patch_inset_norm {v} {
        set v [string trim $v]
        if { [catch { llength $v } n] || $n ni {1 2} } {
            error "photo patch_inset: pixels, or x y, each 0 to 4096"
        }
        set out [lmap c $v { photo_patch_int_norm patch_inset 0 $c }]
        if { [llength $out] == 2 && [lindex $out 0] == [lindex $out 1] } {
            set out [lindex $out 0]
        }
        return [join $out " "]
    }
    # one grey, or r g b, each 0 to 1
    proc photo_patch_dark_norm {v} {
        set v [string trim $v]
        if { [catch { llength $v } n] || $n ni {1 3} } {
            error "photo patch_dark: one grey or r g b, each 0 to 1"
        }
        foreach c $v {
            if { ![string is double -strict $c] || $c < 0 || $c > 1 } {
                error "photo patch_dark: one grey or r g b, each 0 to 1"
            }
        }
        return [join $v " "]
    }
    proc set_photo_patch_mode {v} { photo_patch_opt -mode $v }
    proc set_photo_patch_corner {v} { photo_patch_opt -corner $v }
    proc set_photo_patch_size {v} { photo_patch_opt -size [photo_patch_size_norm $v] }
    proc set_photo_patch_inset {v} { photo_patch_opt -inset [photo_patch_inset_norm $v] }
    proc set_photo_patch_dark {v} { photo_patch_opt -dark [photo_patch_dark_norm $v] }

    # Turn stim2's patch on, when the rig asks for it. Called by
    # configure_stim right after the prelude (whose `postpass reset` turned
    # it off). Sent with rmtSend, so the data file records it and a replay
    # draws the same square; guarded like tlock's, so a stim2 without the
    # module answers -1 and that is a warning. Returns 1 if it is on, 0 if
    # the rig does not ask for it, -1 if stim2 cannot.
    proc stim_photopatch_enable {} {
        variable photo_enabled
        variable photo_patch
        variable photo_patch_opts
        variable photo_patch_stim
        catch { photo_din_attach }
        if { !$photo_enabled || !$photo_patch } {
            set photo_patch_stim 0
            photo_verify_stop
            catch { photo_health }      ;# a system load: look again
            return 0
        }
        set r [rmtSend "if {\[llength \[info commands photopatch\]\]} {\
 [list photopatch set {*}$photo_patch_opts] ; photopatch on } else { expr {-1} }"]
        if { [string trim $r] eq "1" } {
            set photo_patch_stim 1
            # every change of it paired with its light (photo_verify)
            if { [catch { photo_verify_start } err] } {
                ::ess::ess_warning "photo: changes not paired with edges: $err" "photo"
            }
            catch { photo_health }
            return 1
        }
        photo_verify_stop
        set photo_patch_stim -1
        catch { photo_health }
        ::ess::ess_warning "photo patch is on, but stim2 did not draw it\
 ([expr {[string trim $r] eq "-1" ? "no photopatch module" : $r}])" "photo"
        return -1
    }

    # THE GREEN LIGHT (photo/health, the gear's dot on the photo section;
    # see settings-1.0.tm). The knobs say a diode is there; this says
    # whether the detector is hearing it:
    #
    #   off    not enabled on this rig
    #   error  no photo processor in this dserv; no block from the group
    #          (with other groups streaming: the group name is wrong), or
    #          none for 5 s (the stream stopped); stim2 cannot draw the
    #          patch; or the patch has changed and no edge came back within
    #          2 s of the last change (the diode is not over the patch, or
    #          min_step is above its step)
    #   ok     blocks arriving; with the patch on, its changes answered
    #
    # Like tlock's: worked out when a key changes, at a system load, at a
    # data file's open and at each end_obs, from the processor's counters
    # and three datapoint timestamps; published only when it changes. No
    # subscription to the stream or the edges.
    variable photo_patch_stim 0       ;# 1 stim2 took it, -1 it cannot
    variable photo_health_last "?"
    variable photo_health_stale_us 5000000
    variable photo_health_answer_us 2000000

    # the newest timestamp of any datapoint matching a pattern (0: none)
    proc photo_newest { pattern } {
        set keys {}
        if { [string match {*[*]*} $pattern] } {
            catch { set keys [dservKeys $pattern] }
        } else {
            set keys [list $pattern]
        }
        set newest 0
        foreach k $keys {
            set t 0
            catch { set t [dservTimestamp $k] }
            if { [string is wideinteger -strict $t] && $t > $newest } { set newest $t }
        }
        return $newest
    }
    # the blocks the processor reads, as a datapoint pattern
    proc photo_group_pattern { group } {
        if { $group eq "" } { return extio/*/state/ain/* }
        if { [string first / $group] >= 0 } {
            lassign [split $group /] box label
            return extio/$box/state/ain/$label
        }
        return extio/*/state/ain/$group
    }

    # The patch half of the light, for either source: src names it, n its
    # edges so far, t_edge its newest edge's time (0 none), hint what to
    # check when the changes go unanswered.
    proc photo_patch_health { src n t_edge hint } {
        variable photo_patch
        variable photo_patch_stim
        variable photo_health_answer_us
        if { $photo_patch && $photo_patch_stim < 0 } {
            return "error stim2 cannot draw the patch (no photopatch module)"
        }
        if { !$photo_patch } { return "ok reading $src, $n edges" }
        set t_pp [photo_newest stim/photopatch]
        # the patch appearing (why 1: swapcount wall level serial ntags
        # why) is no change over a scene as dark as it
        set why 0
        catch { set why [lindex [dservGet stim/photopatch] 5] }
        if { $t_pp == 0 || ($why == 1 && $t_edge == 0) } {
            return "ok reading $src, $n edges; the patch has not changed yet"
        }
        if { $t_edge == 0 } {
            return "error the patch has changed, but no edge ever came back: $hint"
        }
        if { $t_pp - $t_edge > $photo_health_answer_us } {
            return "error the patch changed, but the last edge is\
 [format %.1f [expr {($t_pp - $t_edge) / 1e6}]] s older than the last\
 change: [lindex [split $hint ?] 0]?"
        }
        # and how well, this file (photo_verify)
        set s ""
        catch { set s [pv_summary] }
        if { $s ne "" } { return "ok reading $src; $s" }
        return "ok reading $src, $n edges; the patch's changes answered"
    }

    # What to check when a din line's edges don't answer the patch. The
    # first sentence (to the "?") stands alone: photo_patch_health quotes
    # only that much when edges did come, just late. A module's comparator
    # has a pot; the box's own (pin mode cmp, docs/photo_processor.md "The
    # box's own comparator") learns its threshold and then locks it, so the
    # advice there is its state/cmp: still learning, or locked to levels
    # from before the detector was where it is now.
    proc photo_din_hint { dp } {
        set where "is the detector over the patch?"
        set st ""
        if { [regexp {^extio/([^/]+)/state/di/[^/]+$} $dp -> box] } {
            catch { set st [dict get [dservGet extio/$box/state/cmp] state] }
        }
        switch -- $st {
            armed {
                return "$where The box's comparator locked its threshold to the\
 levels it saw then: if the detector has moved since, relearn\
 (dservSet extio/$box/cmd/cmp/relearn 1) and flip the patch a few times"
            }
            learning {
                return "$where The box's comparator is still learning and reports\
 no edges until it has seen the patch dark, light and dark again: flip it a\
 few times (photo_test aim)"
            }
            no_samples {
                return "$where The box's comparator has no samples to learn from:\
 turn ain on (ain/enable 1; firmware before 0.4.0+110 also needs the line's\
 channel in an ain group)"
            }
            "" -
            off -
            unsupported {}
            default {
                return "$where The box's comparator reads '$st' (extio/$box/state/cmp)"
            }
        }
        return "is the detector over the patch, its pot between the patch's\
 dark and light?"
    }

    proc photo_health {} {
        variable photo_enabled
        variable photo_health_last
        variable photo_health_stale_us
        variable photo_din
        variable photo_din_n
        set h ""
        if { !$photo_enabled } {
            set h "off not enabled on this rig"
        } elseif { $photo_din ne "" } {
            # the light from a comparator's line: no stream to watch, only
            # whether its edges answer the patch
            set r [photo_din_resolve]
            if { [dict get $r status] ne "ok" } {
                set h "error photo din: [dict get $r detail]"
            } else {
                photo_din_attach
                set h [photo_patch_health "line $photo_din" $photo_din_n \
                           [photo_newest [dict get $r dp]] \
                           [photo_din_hint [dict get $r dp]]]
            }
        } else {
            set n_edges [processGetParam photo n_edges]
            if { $n_edges eq "" } {
                set h "error this dserv has no photo processor: nothing is detected"
            } else {
                set group [processGetParam photo group]
                set gname [expr {$group eq "" ? "every analog group" : "group $group"}]
                set t_blk [photo_newest [photo_group_pattern $group]]
                set now_us [now]
                if { $t_blk == 0 } {
                    if { [photo_newest extio/*/state/ain/*] } {
                        set h "error no block from $gname, though other analog\
 groups stream: is the group name right?"
                    } else {
                        set h "error no block from $gname: is the diode's group\
 streaming?"
                    }
                } elseif { $now_us - $t_blk > $photo_health_stale_us } {
                    set h "error the last block from $gname is\
 [expr {($now_us - $t_blk) / 1000000}] s old: the stream stopped"
                } else {
                    set h [photo_patch_health $gname $n_edges \
                               [photo_newest proc/photo/edge] \
                               "is the diode over the patch? min_step above its step?"]
                }
            }
        }
        if { $h ne $photo_health_last } {
            set photo_health_last $h
            dservSet photo/health $h
        }
        return $h
    }

    # The photo_verify block's three moments (below), with the processor's
    # settings: a system load drew the patch (stim_photopatch_enable), a
    # data file opened (photo_log_match), an obs ended, the file closes.
    proc photo_verify_start {} {
        variable photo_patch_opts
        variable stim_cmd_hook
        pv_start [dict get $photo_patch_opts -mode] [dserv_wall_offset]
        dservAddExactMatch stim/photopatch
        dpointSetScript stim/photopatch ::ess::pv_change_dp
        dservAddExactMatch proc/photo/edge
        dpointSetScript proc/photo/edge ::ess::pv_edge_dp
        if { "::ess::pv_cmd" ni $stim_cmd_hook } { lappend stim_cmd_hook ::ess::pv_cmd }
    }
    proc photo_verify_stop {} {
        variable stim_cmd_hook
        set stim_cmd_hook [lsearch -all -inline -not -exact $stim_cmd_hook ::ess::pv_cmd]
        pv_stop
    }
    proc photo_obs_end {} {
        variable pv_on
        if { $pv_on } { pv_resolve [now] ; pv_publish }
        photo_health
    }
    # before the logger closes: the file's verdict goes in it, and the log
    proc photo_file_close {} {
        variable pv_on
        if { !$pv_on } return
        pv_resolve [now]
        pv_publish
        set s [pv_summary]
        if { $s ne "" } { ess_info "photo: $s" "photo" }
    }
    # END photo

    # BEGIN photo_verify
    #
    # Each change of the photodiode's patch paired with its light, as the
    # session runs: the online half of tools/photopair (docs/
    # photo_processor.md, "Checking a system"). stim2 changes the patch on
    # every frame a '!' command (or a swap tag) lands on (marked mode) and
    # reports it as stim/photopatch {swap flip_wall level serial ntags why
    # nacks}; the photo processor reports each light edge as
    # proc/photo/edge (rows of ten: dir t50 t10 ...). Here:
    #
    #   change -> edge    as photopair pairs them: (1) each change's next
    #                     edge of its direction, within 100 ms, gives a
    #                     latency; the median of the last 50 of a direction
    #                     is its typical latency (until a direction has
    #                     3, its newest; with none, the other's; the
    #                     patch appearing gives none, its next edge may be
    #                     the scene's); (2) the change takes
    #                     the unused edge nearest its flip plus that
    #                     latency, within +-4 ms; (3) failing that, its first
    #                     unused edge of its direction within 100 ms, before
    #                     the next change of that direction could light: the
    #                     light came, at another latency (late). Three late
    #                     in a row in a direction: its typical latency starts
    #                     again from those three (a display that changed how
    #                     it draws mid-session)
    #   '!' command -> change    exactly, by swap: a '!' reply names the swap
    #                     that first showed the command, the swap
    #                     stim/photopatch names
    #
    # A change is judged once nothing more can arrive for it, 300 ms after
    # its flip (pv_hold_us), at the next record, end_obs or file close; an
    # edge no change took by then is extra. A flip is put on the dserv clock
    # with the offset taken at the start (pv_off); where stim2 has no flip
    # time (glfinish on a Mac) the record's arrival is used. A constant
    # error in either only moves the typical latency.
    #
    # Out:
    #   photo/pair     one per change, at its flip (JSON, in the data file):
    #                  serial swap level why nacks; seqs, the '!' commands
    #                  (stim/cmd seq) on its swap; status lit | late | dark
    #                  | appear (the patch's first frame: over a scene as
    #                  bright as the patch it gives no edge, so it is never
    #                  dark); and when an edge took it, edge (its seq), lat10
    #                  and lat50 (us from the flip), and light (its t50 on
    #                  the dserv clock: the light's own time, whatever the
    #                  flip's clock); late adds expect, the typical lat50
    #   photo/verify   the verdict (JSON, at each end_obs and file close):
    #                  verdict pass | fail | none; changes lit late dark
    #                  appear extra pending; cmds cmds_lit cmds_late
    #                  cmds_dark unmarked (a '!' command on a swap with no
    #                  change: counted in marked mode only); rise and fall
    #                  {n median sd min max}, ms of lat50, late ones
    #                  included. pass: every change lit, none late or dark,
    #                  no extra edge, no unmarked command. Counts restart with each data file
    #                  (pv_new_count); the typical latencies carry over.
    #
    # A change is judged after its obs may have ended, so its photo/pair can
    # land in the next obs or between obs in the file: a reader joins it by
    # swap (df::add_display_times), never by the obs it is in.
    #
    # Self-contained: tests/test_ess_photo_verify.tcl evaluates the text
    # between these markers in `namespace eval ess`, with now and
    # dservSetData stood in, and feeds it records.
    #
    variable pv_on 0
    variable pv_mode marked
    variable pv_off 0
    variable pv_hold_us 300000
    variable pv_cmd_hold_us 400000     ;# a change is judged before its commands
    variable pv_max_us 100000
    variable pv_window_us 4000
    variable pv_keep 50
    variable pv_min_dir_n 3
    variable pv_changes {}             ;# {t dir serial swap why nacks level}
    variable pv_edges {}               ;# {t50 dir t10 seq used}
    variable pv_cmds {}                ;# {t_ret swap seq}
    variable pv_swaps [dict create]    ;# swap -> {status t}, judged changes
    variable pv_lats [dict create 1 {} -1 {}]   ;# first-pass latencies, us
    variable pv_late_run [dict create 1 0 -1 0] ;# late in a row, by direction
    variable pv_lat50 [dict create 1 {} -1 {}]  ;# paired lat50s this file, us
    variable pv_n [dict create]

    proc pv_counts_zero {} {
        variable pv_n [dict create changes 0 lit 0 late 0 dark 0 appear 0 \
                           extra 0 cmds 0 cmds_lit 0 cmds_late 0 cmds_dark 0 \
                           unmarked 0]
        variable pv_lat50 [dict create 1 {} -1 {}]
    }
    pv_counts_zero

    # start pairing afresh: the patch's mode, and dserv's clock minus the
    # stim2 host's wall clock (us)
    proc pv_start { mode off } {
        variable pv_on 1
        variable pv_mode $mode
        variable pv_off $off
        variable pv_changes {}
        variable pv_edges {}
        variable pv_cmds {}
        variable pv_swaps [dict create]
        variable pv_lats [dict create 1 {} -1 {}]
        variable pv_late_run [dict create 1 0 -1 0]
        pv_counts_zero
    }
    proc pv_stop {} {
        variable pv_on 0
        variable pv_changes {}
        variable pv_edges {}
        variable pv_cmds {}
        variable pv_swaps [dict create]
    }
    # a new data file: its own counts, a fresh clock offset
    proc pv_new_count { off } {
        variable pv_off $off
        pv_counts_zero
    }

    # ---- input ----
    proc pv_change_dp { dpoint data } {
        variable pv_on
        if { !$pv_on } return
        set t_recv [now]
        catch { set t_recv [dservTimestamp $dpoint] }
        pv_change $data $t_recv
        pv_resolve [now]
    }
    proc pv_edge_dp { dpoint data } {
        variable pv_on
        variable photo_din
        # with a light line (photo din), its edges are the light
        if { !$pv_on || ([info exists photo_din] && $photo_din ne "") } return
        pv_edge $data
        pv_resolve [now]
    }
    # stim/photopatch's values, and when the record arrived
    proc pv_change { rec t_recv } {
        variable pv_changes
        variable pv_off
        lassign $rec swap wall level serial ntags why nacks
        if { $nacks eq "" } { set nacks 0 }
        set t [expr {$wall > 0 ? wide($wall) + $pv_off : wide($t_recv)}]
        set dir [expr {$why == 1 ? 0 : ($level ? 1 : -1)}]
        lappend pv_changes [list $t $dir $serial $swap $why $nacks $level]
        if { [llength $pv_changes] > 1 && $t < [lindex $pv_changes end-1 0] } {
            set pv_changes [lsort -integer -index 0 $pv_changes]
        }
    }
    # proc/photo/edge's values: rows of ten
    proc pv_edge { vals } {
        variable pv_edges
        set n [llength $pv_edges]
        for { set i 0 } { $i + 10 <= [llength $vals] } { incr i 10 } {
            lassign [lrange $vals $i [expr {$i + 2}]] dir t50 t10
            lappend pv_edges [list $t50 [expr {$dir > 0 ? 1 : -1}] $t10 \
                                  [expr {int([lindex $vals [expr {$i + 7}]])}] 0]
        }
        if { $n && [llength $pv_edges] > $n &&
             [lindex $pv_edges $n 0] < [lindex $pv_edges $n-1 0] } {
            set pv_edges [lsort -real -index 0 $pv_edges]
        }
    }
    # the stim_cmd_log hook: every command that came back with a swap
    proc pv_cmd { seq cmd swap t_ret args } {
        variable pv_on
        variable pv_cmds
        if { !$pv_on || [string index $cmd 0] ne "!" } return
        lappend pv_cmds [list $t_ret $swap $seq]
    }

    # ---- judging ----
    proc pv_median { l } {
        set l [lsort -real $l]
        set n [llength $l]
        if { !$n } { return {} }
        if { $n % 2 } { return [lindex $l [expr {$n / 2}]] }
        expr {([lindex $l [expr {$n / 2 - 1}]] + [lindex $l [expr {$n / 2}]]) / 2.0}
    }

    # everything that can no longer change, as of `now` (dserv us)
    proc pv_resolve { now } {
        variable pv_changes
        variable pv_edges
        variable pv_cmds
        variable pv_swaps
        variable pv_hold_us
        variable pv_cmd_hold_us
        variable pv_window_us
        variable pv_mode
        variable pv_n
        set horizon [expr {$now - $pv_hold_us}]
        while { [llength $pv_changes] && [lindex $pv_changes 0 0] < $horizon } {
            set c [lindex $pv_changes 0]
            set pv_changes [lrange $pv_changes 1 end]
            pv_judge $c
        }
        # edges older than any change still to be judged could take
        set ehz [expr {$horizon - $pv_window_us}]
        set keep {}
        foreach e $pv_edges {
            if { [lindex $e 0] >= $ehz } { lappend keep $e ; continue }
            if { ![lindex $e 4] } { dict incr pv_n extra }
        }
        set pv_edges $keep
        # '!' commands, once the change on their swap has been judged
        set chz [expr {$now - $pv_cmd_hold_us}]
        set keep {}
        foreach c $pv_cmds {
            lassign $c t swap seq
            if { $t >= $chz } { lappend keep $c ; continue }
            dict incr pv_n cmds
            if { [dict exists $pv_swaps $swap] } {
                switch -- [lindex [dict get $pv_swaps $swap] 0] {
                    dark { dict incr pv_n cmds_dark }
                    late { dict incr pv_n cmds_late }
                    default { dict incr pv_n cmds_lit }
                }
            } elseif { $pv_mode eq "marked" } {
                dict incr pv_n unmarked
            }
        }
        set pv_cmds $keep
        dict for { s v } $pv_swaps {
            if { [lindex $v 1] < $horizon - 10000000 } { dict unset pv_swaps $s }
        }
    }

    proc pv_judge { c } {
        variable pv_edges
        variable pv_lats
        variable pv_lat50
        variable pv_keep
        variable pv_max_us
        variable pv_window_us
        variable pv_min_dir_n
        variable pv_n
        variable pv_swaps
        variable pv_cmds
        variable pv_changes
        variable pv_late_run
        lassign $c t dir serial swap why nacks level
        set ne [llength $pv_edges]
        # 1: its next edge of its direction, used or not: the latency. The
        # patch appearing (dir 0) keeps none: over a scene its next edge,
        # of either direction, can be the scene's, and one such stray
        # (3.7 ms, on an LCD that lights at 12) was a direction's typical
        # latency. It still pairs by its own when there is no other.
        set own {}
        for { set k 0 } { $k < $ne } { incr k } {
            lassign [lindex $pv_edges $k] e50 edir
            if { $e50 <= $t } continue
            if { $e50 - $t > $pv_max_us } break
            if { $dir == 0 || $dir == $edir } {
                set own [expr {$e50 - $t}]
                if { $why == 1 } break
                set l [dict get $pv_lats $edir]
                lappend l $own
                if { [llength $l] > $pv_keep } { set l [lrange $l end-[expr {$pv_keep - 1}] end] }
                dict set pv_lats $edir $l
                break
            }
        }
        # 2: the unused edge nearest its flip plus its direction's latency
        set best -1
        set all [concat [dict get $pv_lats 1] [dict get $pv_lats -1]]
        if { ![llength $all] } { set all $own }
        if { [llength $all] } {
            set L [pv_median $all]
            # a direction's median once it has a few; before that its
            # newest (this change's own, from 1), not a median of one or
            # two: a stray beside it moves that -- a first change at 13.3
            # ms beside a stray 3.7 was judged by 8.5 and called late, as
            # it was by the overall median. L only for a direction with none
            foreach d {1 -1} {
                set l [dict get $pv_lats $d]
                set n [llength $l]
                set Ld($d) [expr {$n >= $pv_min_dir_n ? [pv_median $l] : ($n ? [lindex $l end] : $L)}]
            }
            set bd {}
            for { set k 0 } { $k < $ne } { incr k } {
                lassign [lindex $pv_edges $k] e50 edir - - used
                if { $used || ($dir != 0 && $dir != $edir) } continue
                set d [expr {abs($e50 - $t - $Ld($edir))}]
                if { $d > $pv_window_us } continue
                if { $best < 0 || $d < $bd } { set best $k ; set bd $d }
            }
        }
        # 3: lit, but not at its latency: its first unused edge, before the
        # next change of its direction could have lit
        set late 0
        if { $best < 0 && $dir != 0 && [llength $all] } {
            set lim [expr {$t + $pv_max_us}]
            foreach n $pv_changes {
                if { [lindex $n 1] == $dir } {
                    set lim [expr {min($lim, [lindex $n 0] + $Ld($dir) - $pv_window_us)}]
                    break
                }
            }
            for { set k 0 } { $k < $ne } { incr k } {
                lassign [lindex $pv_edges $k] e50 edir - - used
                if { $e50 <= $t } continue
                if { $e50 > $lim } break
                if { $used || $edir != $dir } continue
                set best $k
                set late 1
                break
            }
        }
        set seqs [lmap x $pv_cmds {
            if { [lindex $x 1] != $swap } continue
            lindex $x 2
        }]
        set json "\{\"serial\":$serial,\"swap\":$swap,\"level\":$level,\"why\":$why"
        append json ",\"nacks\":$nacks,\"seqs\":\[[join $seqs ,]\]"
        if { $best >= 0 } {
            lset pv_edges $best 4 1
            lassign [lindex $pv_edges $best] e50 edir e10 eseq
            set lat50 [expr {round($e50 - $t)}]
            set lat10 [expr {round($e10 - $t)}]
            set status [expr {$why == 1 ? "appear" : ($late ? "late" : "lit")}]
            if { $why != 1 } { dict lappend pv_lat50 $edir $lat50 }
            append json ",\"status\":\"$status\",\"edge\":$eseq"
            append json ",\"lat10\":$lat10,\"lat50\":$lat50"
            if { $late } { append json ",\"expect\":[expr {round($Ld($edir))}]" }
            append json ",\"light\":[expr {wide(round($e50))}]\}"
            if { $why != 1 } {
                if { !$late } {
                    dict set pv_late_run $edir 0
                } elseif { [dict get [dict incr pv_late_run $edir] $edir] >= $pv_min_dir_n } {
                    # the latency moved: start again from the late ones
                    dict set pv_lats $edir [lrange [dict get $pv_lats $edir] \
                                                end-[expr {$pv_min_dir_n - 1}] end]
                    dict set pv_late_run $edir 0
                }
            }
        } else {
            set status [expr {$why == 1 ? "appear" : "dark"}]
            append json ",\"status\":\"$status\"\}"
        }
        if { $status ne "appear" } { dict incr pv_n changes }
        dict incr pv_n $status
        dict set pv_swaps $swap [list $status $t]
        catch { dservSetData photo/pair [expr {wide($t)}] 11 $json }
        return $status
    }

    # ---- out ----
    # {n median sd min max} in ms of a list of us
    proc pv_stats { l } {
        set n [llength $l]
        if { !$n } { return [list 0 null null null null] }
        set ms [lmap x $l { expr {$x / 1000.0} }]
        set mean [expr {[tcl::mathop::+ {*}$ms] / $n}]
        set ss 0.0
        foreach x $ms { set ss [expr {$ss + ($x - $mean) ** 2}] }
        set sd [expr {$n > 1 ? sqrt($ss / ($n - 1)) : 0.0}]
        list $n [format %.3f [pv_median $ms]] [format %.3f $sd] \
            [format %.3f [tcl::mathfunc::min {*}$ms]] [format %.3f [tcl::mathfunc::max {*}$ms]]
    }
    proc pv_verdict {} {
        variable pv_n
        dict with pv_n {}
        if { $lit + $late + $dark == 0 } { return none }
        expr {$dark || $late || $extra || $unmarked ? "fail" : "pass"}
    }
    proc pv_json {} {
        variable pv_n
        variable pv_lat50
        variable pv_changes
        set json "\{\"verdict\":\"[pv_verdict]\""
        dict for { k v } $pv_n { append json ",\"$k\":$v" }
        append json ",\"pending\":[llength $pv_changes]"
        foreach { name d } { rise 1 fall -1 } {
            lassign [pv_stats [dict get $pv_lat50 $d]] n med sd lo hi
            append json ",\"$name\":\{\"n\":$n,\"median\":$med,\"sd\":$sd,\"min\":$lo,\"max\":$hi\}"
        }
        append json "\}"
    }
    proc pv_publish {} {
        catch { dservSetData photo/verify [now] 11 [pv_json] }
    }
    # one line for the log and the green light; "" before any change
    proc pv_summary {} {
        variable pv_n
        variable pv_lat50
        dict with pv_n {}
        if { $lit + $late + $dark == 0 } { return "" }
        set s "[pv_verdict]: $lit of [expr {$lit + $late + $dark}] changes lit"
        if { $late } { append s ", $late late" }
        if { $dark } { append s ", $dark dark" }
        if { $extra } { append s ", $extra extra edge[expr {$extra == 1 ? {} : {s}}]" }
        if { $unmarked } { append s ", $unmarked '!' commands not marked" }
        foreach { name d } { rise 1 fall -1 } {
            lassign [pv_stats [dict get $pv_lat50 $d]] n med sd lo hi
            if { $n } { append s "; $name $med ms (sd $sd, max $hi)" }
        }
        return $s
    }
    # END photo_verify

    # BEGIN photo_test
    #
    # Is the diode over the patch? (the gear's photo section, "Test
    # placement"; docs/photo_processor.md, "Placing the diode"). Flips the
    # patch between dark and light, a gap apart, and after each flip reads
    # what came back: the edges the detector reported, and the detector's
    # idle level. The level follows the light whether or not the step was
    # big enough to be an edge (an EMA with a 20 ms time constant, settled
    # long before the gap ends), so a step below min_step is still
    # measured, and "not over the patch" is told apart from "over it, but
    # min_step is above its step".
    #
    #   photo_test ?run? ?n? ?gap_ms?   n flips (10), gap_ms apart (250)
    #   photo_test aim ?s?              flip until stopped, at most s seconds
    #                                   (60): slide the diode until the step
    #                                   peaks
    #   photo_test stop
    #
    # Only between sessions: refused while ESS runs or a data file is open
    # (the flips would be in it), and stopped if either starts. With the
    # gear's `patch` off it draws the patch for the test and takes it away
    # after. The patch is in manual mode for the test (toggle would change
    # it every frame) and back in its own mode after.
    #
    # Out: photo/test (JSON), after each flip and at the end: id mode state
    # (running | done | stopped | error) n flipped answered reversed
    # verdict (aligned | small | partial | reversed | below_min_step |
    # no_edges | none | error; "" while running) summary; dark light step
    # best noise
    # min_step (ADC counts; step = light - dark, best the largest one
    # flip's); rise and fall {n median sd min max}, ms from the flip to the
    # light's 50%, when stim2 has flip times; flips, the last 50 as
    # {level edges step lat_ms} (level shown, edges back, the change in
    # counts toward that level, lat_ms null without an edge or flip time).
    #
    # A light line (photo din) has no level, only edges -- except a line
    # from the box's own comparator (pin mode cmp), whose box reports each
    # settled level in counts (state/cmp/level): dark, light and step are
    # measured from those (source "cmp"), with no analog stream, and the
    # verdict reads the threshold from state/cmp. An aim on such a line ends
    # (done or `stop`) by relearning the threshold where the detector now is
    # and flipping three more times so it locks.
    #
    # Self-contained: tests/test_ess_photo_test.tcl evaluates this block
    # and photo_verify's (pv_median, pv_stats), with the pt_* hooks below
    # stood in.
    #
    variable pt_on 0
    variable pt_id 0
    variable pt_timer ""
    variable pt_mode run
    variable pt_n 10
    variable pt_gap 250
    variable pt_k 0
    variable pt_flips {}          ;# {target flipped edges step lat_us dir}
    variable pt_levels {}         ;# {target level} after each flip
    variable pt_restore {}        ;# stim2 commands that undo the test
    variable pt_noise 0
    variable pt_min_step 0
    variable pt_sign 1            ;# -1 when the detector is inverted
    variable pt_din ""            ;# the light line's label (photo din); "" analog
    variable pt_off 0
    variable pt_base {}           ;# before the flip: changes edges level target
    variable pt_corner ""
    variable pt_last ""           ;# the last photo/test published
    variable pt_aim_w 2           ;# aim: levels from each level's last 2 flips
    variable pt_cmp {}            ;# {box pin sign} when the line is a box's own
                                  ;# comparator (levels in counts); {} otherwise
    variable pt_relock ""         ;# aim on a cmp line: relock flips left; "" none
    variable pt_relock_dk {}      ;# edges back after each relock flip
    variable pt_cmp_min_step 100  ;# the box's TRK_MIN_STEP: it cannot learn below

    # ---- hooks: everything outside this block ----
    proc pt_param { p } { processGetParam photo $p }
    proc pt_stim { cmd } {
        if { [info commands ::ess::rmtSend_raw] ne "" } {
            return [::ess::rmtSend_raw $cmd]
        }
        rmtSend $cmd
    }
    proc pt_get { dp } { dservGet $dp }
    proc pt_enabled {} { variable photo_enabled ; set photo_enabled }
    proc pt_patch_opts {} { variable photo_patch_opts ; set photo_patch_opts }
    proc pt_wall_off {} { dserv_wall_offset }
    # us since the diode's group last sent a block, -1 never
    proc pt_stream_age {} {
        set t [photo_newest [photo_group_pattern [processGetParam photo group]]]
        expr {$t ? [now] - $t : -1}
    }
    # why the test must not flash the corner now, or ""
    proc pt_busy {} {
        variable current
        variable open_datafile
        set sys ""
        catch { set sys $current(state_system) }
        if { $sys ne "" && ![catch { $sys status } st] && $st eq "running" } {
            return "ESS is running"
        }
        if { [info exists open_datafile] && $open_datafile ne "" } {
            return "a data file is open"
        }
        return ""
    }
    proc pt_done_hook {} { catch { photo_health } }
    # the light line (photo din): its label ("" reads the processor), why
    # it cannot be read ("" it can), its edge count and newest {value t}
    proc pt_din_line {} { variable photo_din ; set photo_din }
    proc pt_din_check {} {
        set r [photo_din_resolve]
        if { [dict get $r status] ne "ok" } { return "photo din: [dict get $r detail]" }
        photo_din_attach
        return ""
    }
    proc pt_din_count {} { variable photo_din_n ; set photo_din_n }
    proc pt_din_last {} { variable photo_din_last ; set photo_din_last }
    # A light line from the box's OWN comparator (pin mode cmp) is more than
    # a line: the box reports each settled level in counts
    # (extio/<box>/state/cmp/level), so the test measures dark, light and
    # step as it does for the processor -- with no analog on the wire.
    # {box pin sign} for such a line ({} otherwise, or on firmware without
    # cmp/level); sign -1 when the pin is active-low, which is exactly when
    # light reads lower (an LM393 module's AO).
    proc pt_cmp_line {} {
        if { [catch {
            set out {}
            set r [photo_din_resolve]
            if { [dict get $r status] eq "ok" &&
                 [regexp {^extio/([^/]+)/state/di/([0-9]+)$} [dict get $r dp] -> box pin] &&
                 [dservExists extio/$box/state/cmp/level] &&
                 [dict get [dservGet extio/$box/state/cmp] state] in {learning armed} } {
                set sign 1
                foreach part [split [dservGet extio/$box/state/pins/active_low] ", "] {
                    if { [regexp {^([0-9]+)-([0-9]+)$} $part -> a b] } {
                        if { $pin >= $a && $pin <= $b } { set sign -1 }
                    } elseif { $part eq $pin } { set sign -1 }
                }
                set out [list $box $pin $sign]
            }
            set out
        } out] } { return {} }
        return $out
    }
    proc pt_cmp_level { box } {
        if { [catch { dservGet extio/$box/state/cmp/level } v] } { return "" }
        return $v
    }
    proc pt_cmp_state { box } {
        set s {}
        catch { set s [dservGet extio/$box/state/cmp] }
        return $s
    }
    proc pt_cmp_relearn { box } { dservSet extio/$box/cmd/cmp/relearn 1 }

    proc photo_test { {what run} args } {
        switch -- $what {
            run  { lassign $args n gap
                   if { $n eq "" } { set n 10 }
                   if { $gap eq "" } { set gap 250 }
                   return [pt_start run $n $gap] }
            aim  { lassign $args s
                   if { $s eq "" } { set s 60 }
                   if { ![string is double -strict $s] || $s <= 0 } {
                       error "photo_test aim: seconds, more than 0"
                   }
                   return [pt_start aim [expr {int(ceil($s * 1000.0 / 300))}] 300] }
            stop { variable pt_on
                   variable pt_mode
                   variable pt_cmp
                   variable pt_relock
                   variable pt_n
                   variable pt_k
                   # an aim on a cmp line ends by locking the threshold where
                   # the diode now is: the next tick starts the relock
                   if { $pt_on && $pt_mode eq "aim" && [llength $pt_cmp] &&
                        $pt_relock eq "" } {
                       set pt_n $pt_k
                       return "stopping: relearning the threshold where the\
 detector is now (3 more flips)"
                   }
                   return [pt_finish stopped "stopped"] }
            default { error "photo_test ?run ?n? ?gap_ms?? | aim ?s? | stop" }
        }
    }

    proc pt_start { mode n gap } {
        variable pt_on
        variable pt_id
        variable pt_timer
        variable pt_mode
        variable pt_n
        variable pt_gap
        variable pt_k
        variable pt_flips
        variable pt_levels
        variable pt_restore
        variable pt_off
        variable pt_base
        variable pt_corner
        variable pt_din
        variable pt_cmp
        variable pt_relock
        if { $pt_on } { error "photo_test: a test is already running (photo_test stop)" }
        if { ![string is integer -strict $n] || $n < 1 || $n > 1000 } {
            error "photo_test: flips, 1 to 1000"
        }
        if { ![string is integer -strict $gap] || $gap < 100 || $gap > 5000 } {
            error "photo_test: gap_ms, 100 to 5000"
        }
        incr pt_id
        set pt_mode $mode
        set pt_n $n
        set pt_gap $gap
        set pt_k 0
        set pt_flips {}
        set pt_levels {}
        set pt_restore {}
        set pt_base {}
        set pt_cmp {}
        set pt_relock ""
        variable pt_relock_dk {}

        set why ""
        set busy [pt_busy]
        if { $busy ne "" } {
            set why "not now: $busy (the flips would be in it)"
        } elseif { ![pt_enabled] } {
            set why "photo is not enabled on this rig (gear: photo enabled)"
        } elseif { [set pt_din [pt_din_line]] ne "" } {
            set why [pt_din_check]
            if { $why eq "" } { set pt_cmp [pt_cmp_line] }
        } elseif { [pt_param n_edges] eq "" } {
            set why "this dserv has no photo processor"
        } else {
            set age [pt_stream_age]
            if { $age < 0 || $age > 2000000 } {
                set why "no blocks from the diode's group\
 [expr {$age < 0 ? {} : "for [expr {$age / 1000000}] s"}]: is it streaming?"
            }
        }
        if { $why eq "" } {
            set st ""
            catch { set st [pt_stim "photopatch status"] }
            if { [string trim $st] eq "" } {
                set why "stim2 did not answer: is it connected?"
            } elseif { [string match "!TCL_ERROR*" $st] || [catch { dict get $st on } on] } {
                set why "stim2 has no photopatch module"
            }
        }
        if { $why ne "" } {
            pt_publish error error $why
            error "photo_test: $why"
        }

        # draw the patch for the test, in manual mode; undo it after
        set opts [pt_patch_opts]
        set pt_corner "[dict get $opts -corner], [dict get $opts -size] px"
        set settle 0
        if { !$on } {
            set r [pt_stim [list photopatch set {*}$opts]]
            set r2 [pt_stim "photopatch on manual"]
            if { [string match "!TCL_ERROR*" $r] || [string match "!TCL_ERROR*" $r2] } {
                catch { pt_stim "photopatch off" }
                set why "stim2 would not draw the patch: [string trim $r$r2]"
                pt_publish error error $why
                error "photo_test: $why"
            }
            set pt_restore [list "photopatch off" \
                                "photopatch set -mode [dict get $opts -mode]"]
            set settle 1
        } elseif { [dict get $st mode] ne "manual" } {
            pt_stim "photopatch set -mode manual"
            set pt_restore [list "photopatch set -mode [dict get $st mode]"]
        }
        set pt_off [pt_wall_off]
        set pt_on 1
        pt_publish running "" "starting"
        # a patch just drawn first settles (its first frame is a change)
        set pt_timer [dservAfter [expr {$settle ? max($gap, 400) : 50}] \
                          [list ::ess::pt_tick $pt_id]]
        return $pt_id
    }

    # end of a gap: judge the flip before it, then flip again or finish
    proc pt_tick { id } {
        variable pt_on
        variable pt_id
        variable pt_timer
        variable pt_n
        variable pt_gap
        variable pt_k
        variable pt_flips
        variable pt_levels
        variable pt_noise
        variable pt_min_step
        variable pt_sign
        variable pt_off
        variable pt_base
        variable pt_din
        variable pt_cmp
        variable pt_relock
        variable pt_mode
        variable pt_cmp_min_step
        if { !$pt_on || $id != $pt_id } return
        set pt_timer ""
        set busy [pt_busy]
        if { $busy ne "" } { pt_finish stopped "stopped: $busy" ; return }

        set st [pt_stim "photopatch status"]
        if { [catch { dict get $st on } on] || !$on } {
            pt_finish error "the patch went away during the test (a system load?)"
            return
        }
        set changes [dict get $st changes]
        if { $pt_din ne "" } {
            if { [llength $pt_cmp] } {
                # the box's newest settled level, in counts
                set level_now [pt_cmp_level [lindex $pt_cmp 0]]
                if { ![string is double -strict $level_now] } { set level_now 0 }
            } else {
                # the line's value is the level: 1 light, 0 dark
                set level_now [lindex [pt_din_last] 0]
                if { $level_now eq "" } { set level_now 0 }
            }
            set edges_now [pt_din_count]
        } else {
            set level_now [pt_param level]
            set edges_now [pt_param n_edges]
        }

        if { $pt_k == 0 } {
            if { [llength $pt_cmp] } {
                set pt_noise 0
                set pt_min_step $pt_cmp_min_step
                set pt_sign [lindex $pt_cmp 2]
            } elseif { $pt_din ne "" } {
                set pt_noise 0
                set pt_min_step 0
                set pt_sign 1
            } else {
                set pt_noise [pt_param noise]
                set pt_min_step [pt_param min_step]
                # `level` is in the box's counts; with invert, light is lower
                set inv 0
                catch { set inv [pt_param invert] }
                set pt_sign [expr {[string is true -strict $inv] ? -1 : 1}]
            }
            lappend pt_levels [list [dict get $st level] $level_now]
        } elseif { $pt_relock eq "" } {
            # (a relock flip is not part of the test: the comparator is
            # learning again and reports no edge for it)
            lassign $pt_base changes0 edges0 level0 target
            set flipped [expr {$changes > $changes0}]
            set dk [expr {$edges_now - $edges0}]
            set expdir [expr {$target ? 1 : -1}]
            set dir 0
            set lat null
            if { $dk > 0 } {
                set rec {}
                if { $pt_din ne "" } {
                    lassign [pt_din_last] v t
                    if { $t ne "" } {
                        set d [expr {$v ? 1 : -1}]
                        set rec [list $d $t $t $t 0 0 1 0 $t $t]
                    }
                } else {
                    catch { set rec [pt_get proc/photo/edge] }
                }
                if { [llength $rec] >= 10 } {
                    lassign [lrange $rec end-9 end] dir t50
                    set dir [expr {int($dir)}]
                    set wall -1
                    catch { set wall [dict get $st last_change_flip_wall_us] }
                    if { $dir == $expdir && $wall > 0 } {
                        set l [expr {$t50 - ($wall + $pt_off)}]
                        if { $l > 0 && $l < 200000 } { set lat $l }
                    }
                }
            }
            set step [expr {($level_now - $level0) * $expdir * $pt_sign}]
            lappend pt_flips [list $target $flipped $dk $step $lat $dir]
            lappend pt_levels [list $target $level_now]
        } else {
            # a relock flip: only whether its edge came back
            variable pt_relock_dk
            lappend pt_relock_dk [expr {$edges_now - [lindex $pt_base 1]}]
        }

        if { $pt_k >= $pt_n } {
            # An aim on a cmp line: the detector moved, so the threshold the
            # box locked before is stale. Relearn and flip three more times --
            # it locks on the return to the first level and the third shows
            # an edge (box_cmp.h).
            if { $pt_relock eq "" && $pt_mode eq "aim" && [llength $pt_cmp] } {
                catch { pt_cmp_relearn [lindex $pt_cmp 0] }
                set pt_relock 3
            }
            if { $pt_relock eq "" || $pt_relock <= 0 } { pt_finish done "" ; return }
            incr pt_relock -1
        }

        set target [expr {[dict get $st level] ? 0 : 1}]
        set r [pt_stim "photopatch flip $target"]
        if { [string match "!TCL_ERROR*" $r] } {
            pt_finish error "stim2 refused the flip: [string trim $r]"
            return
        }
        set pt_base [list $changes $edges_now $level_now $target]
        if { $pt_relock eq "" } { incr pt_k }
        pt_publish running "" ""
        set pt_timer [dservAfter $pt_gap [list ::ess::pt_tick $id]]
    }

    proc pt_finish { state why } {
        variable pt_on
        variable pt_timer
        variable pt_restore
        if { !$pt_on } {
            if { $state eq "stopped" } { return "no test running" }
            return ""
        }
        if { $pt_timer ne "" } { catch { dservAfterCancel $pt_timer } }
        set pt_timer ""
        foreach c $pt_restore { catch { pt_stim $c } }
        set pt_restore {}
        set pt_on 0
        if { $state eq "error" } {
            set s [pt_publish error error $why]
        } else {
            lassign [pt_verdict] verdict s
            if { $state eq "stopped" && $why ne "" } { set s "$why; $s" }
            set s [pt_publish $state $verdict $s]
        }
        pt_done_hook
        return $s
    }

    # dark and light: the median settled level after flips to each (aim:
    # each one's last few, so it follows the diode as it moves)
    proc pt_dark_light {} {
        variable pt_levels
        variable pt_mode
        variable pt_aim_w
        set out {}
        foreach lv { 0 1 } {
            set l {}
            foreach p $pt_levels {
                if { [lindex $p 0] == $lv } { lappend l [lindex $p 1] }
            }
            if { $pt_mode eq "aim" } { set l [lrange $l end-[expr {$pt_aim_w - 1}] end] }
            lappend out [pv_median $l]
        }
        return $out
    }

    # the detector answers each change the wrong way round: which way the
    # gear's `photo invert` should go
    proc pt_reversed_msg { lv } {
        variable pt_sign
        if { $pt_sign < 0 } {
            return "reversed: the reading rises when the patch lights ($lv),\
 but photo invert is on: turn it off"
        }
        return "reversed: the reading falls when the patch lights ($lv):\
 an inverted detector (an LM393 module's analog out): turn on photo invert"
    }

    proc pt_counts {} {
        variable pt_flips
        set f 0 ; set a 0 ; set r 0
        foreach x $pt_flips {
            lassign $x target flipped dk step lat dir
            if { !$flipped } continue
            incr f
            set expdir [expr {$target ? 1 : -1}]
            if { $dk > 0 && $dir == $expdir } { incr a } elseif { $dk > 0 && $dir == -$expdir } { incr r }
        }
        list $f $a $r
    }

    proc pt_verdict {} {
        variable pt_noise
        variable pt_min_step
        variable pt_sign
        variable pt_corner
        variable pt_flips
        variable pt_din
        variable pt_cmp
        lassign [pt_counts] f a r
        lassign [pt_dark_light] dark light
        if { !$f } {
            if { ![llength $pt_flips] } { return [list none "no flips"] }
            return [list error "stim2 never changed the patch"]
        }
        if { [llength $pt_cmp] } { return [pt_cmp_verdict $f $a $r $dark $light] }
        if { $pt_din ne "" } { return [pt_din_verdict $f $a $r] }
        set step [expr {$dark eq "" || $light eq "" ? 0.0 : ($light - $dark) * $pt_sign}]
        set S [format %.0f $step]
        set M [format %.0f $pt_min_step]
        set N [format %.1f $pt_noise]
        set lv "step $S counts (dark [format %.0f $dark], light [format %.0f $light])"
        set seen [expr {abs($step) >= max(5.0 * $pt_noise, 3.0)}]
        if { $a == $f } {
            if { $step >= 2.0 * $pt_min_step } {
                set s "aligned: $a of $f changes answered; $lv, min_step $M"
                foreach { name d } { rise 1 fall -1 } {
                    lassign [pv_stats [pt_lats $d]] n med sd
                    if { $n } { append s "; $name $med ms (sd $sd)" }
                }
                return [list aligned $s]
            }
            return [list small "small: every change answered, but the $lv is under\
 2x min_step ($M): centre the diode on the patch or raise the brightness"]
        }
        if { $r > $a && 2 * $r >= $f } {
            return [list reversed [pt_reversed_msg $lv]]
        }
        if { $a > 0 } {
            return [list partial "partial: $a of $f changes answered; $lv against\
 min_step $M: centre the diode, raise the brightness, or lower min_step"]
        }
        if { $seen && $step < 0 } {
            return [list reversed [pt_reversed_msg $lv]]
        }
        # a min_step to suggest: a third of the step (the detector needs
        # the change to outrun its idle level; see no_edges), above noise
        set sug [format %.0f [expr {max($step / 3.0, ceil(5.0 * $pt_noise))}]]
        if { $seen && $step < $pt_min_step } {
            return [list below_min_step "below min_step: the diode sees the patch\
 ($lv, noise $N) but min_step $M is above it: centre the diode, raise the\
 brightness, or set min_step near $sug"]
        }
        if { $seen } {
            # Pi 5 rig, 2026-11-22: step 146, min_step 100, a ~10 ms LCD
            # rise. The idle level (an EMA, 20 ms) follows a change that
            # slow, so the difference from it never reaches min_step
            return [list no_edges "no edges: the diode sees the patch ($lv,\
 noise $N), above min_step $M, but no edge came: a change this slow is\
 followed by the detector's level before it is min_step away. Raise the\
 brightness, or set min_step near $sug"]
        }
        return [list none "none: no change under the diode ($lv, noise $N): it is\
 not over the patch ($pt_corner)"]
    }

    # A light line has no level to measure: only whether each flip came
    # back, and which way. The pot sets the threshold; the module's LED
    # shows where it is while the patch flips (photo_test aim).
    proc pt_din_verdict { f a r } {
        variable pt_din
        variable pt_corner
        set lt "line $pt_din"
        if { $a == $f } {
            set s "aligned: $a of $f changes answered on $lt"
            foreach { name d } { rise 1 fall -1 } {
                lassign [pv_stats [pt_lats $d]] n med sd
                if { $n } { append s "; $name $med ms (sd $sd)" }
            }
            return [list aligned $s]
        }
        if { $r > $a && 2 * $r >= $f } {
            return [list reversed "reversed: $lt goes to 1 when the patch\
 darkens: set the line active-low on the box (the LM393 boards pull low in\
 light), or off if it is set"]
        }
        if { $a > 0 } {
            return [list partial "partial: $a of $f changes answered on $lt:\
 the threshold is near one of the patch's levels. Turn the pot toward the\
 middle, or centre the detector"]
        }
        return [list none "none: no edge on $lt: is the detector over the patch\
 ($pt_corner), and its pot between the patch's dark and light? (its LED\
 should follow the patch)"]
    }

    # A line from the box's own comparator: the edges as for any line, and
    # the levels in counts (state/cmp/level), so the advice can say WHY a
    # flip went unanswered -- a step the box cannot learn from, or a
    # threshold (state/cmp's dac) that is not between the patch's levels.
    proc pt_cmp_verdict { f a r dark light } {
        variable pt_din
        variable pt_cmp
        variable pt_corner
        variable pt_sign
        variable pt_cmp_min_step
        lassign $pt_cmp box
        set lt "line $pt_din"
        set step [expr {$dark eq "" || $light eq "" ? 0.0 : ($light - $dark) * $pt_sign}]
        set S [format %.0f $step]
        set M $pt_cmp_min_step
        set lv "step $S counts (dark [format %.0f $dark], light [format %.0f $light])"
        set cs [pt_cmp_state $box]
        set st ""
        set thr ""
        catch { set st [dict get $cs state] }
        if { $st eq "armed" } { catch { set thr [expr {([dict get $cs dac] + 1) * 16}] } }
        set relearn "relearn (photo_test aim relearns as it ends, or\
 dservSet extio/$box/cmd/cmp/relearn 1)"
        set lo [expr {min($dark, $light)}]
        set hi [expr {max($dark, $light)}]

        # An aim that relocked is judged by the relock, not by its flips:
        # those came while the detector was being moved, against the
        # threshold from before, which the relock has replaced.
        variable pt_mode
        variable pt_relock
        variable pt_relock_dk
        if { $pt_mode eq "aim" && $pt_relock ne "" } {
            if { abs($step) < 3 } {
                return [list none "none: no change under the detector ($lv): it\
 is not over the patch ($pt_corner)"]
            }
            if { $step < $M } {
                return [list below_min_step "below min_step: the detector sees the\
 patch ($lv), but the box needs a step of $M to learn its threshold: centre\
 the detector or raise the brightness"]
            }
            set aimed "aimed: $lv ($a of $f changes answered while aiming)"
            if { $pt_relock > 0 } {
                return [list partial "$aimed; the relearn was cut short: flip the\
 patch until the comparator locks (photo_test run 4)"]
            }
            if { $st ne "armed" } {
                return [list no_edges "$aimed; but the relearn did not lock (the\
 comparator is $st): hold the detector still and flip the patch (photo_test run 4)"]
            }
            if { $thr eq "" || $thr <= $lo || $thr >= $hi } {
                return [list partial "$aimed; relearned at $thr counts, which is\
 not between those levels: the detector moved during the relearn. Hold it still\
 and aim again"]
            }
            if { [lindex $pt_relock_dk end] > 0 } {
                if { $step < 2 * $M } {
                    return [list small "$aimed; relearned and locked at $thr counts,\
 and the change after it answered, but the step is under 2x the $M the box\
 needs: centre the detector on the patch or raise the brightness"]
                }
                return [list aligned "$aimed; relearned and locked at $thr counts,\
 between them, and the change after it answered"]
            }
            return [list partial "$aimed; relearned at $thr counts, but the change\
 after it went unanswered: hold the detector still and aim again"]
        }

        if { $a == $f } {
            if { $step >= 2 * $M } {
                set s "aligned: $a of $f changes answered on $lt; $lv"
                if { $thr ne "" } { append s ", threshold $thr" }
                foreach { name d } { rise 1 fall -1 } {
                    lassign [pv_stats [pt_lats $d]] n med sd
                    if { $n } { append s "; $name $med ms (sd $sd)" }
                }
                return [list aligned $s]
            }
            return [list small "small: every change answered on $lt, but the $lv\
 is under 2x the $M the box needs to learn from: centre the detector on the\
 patch or raise the brightness"]
        }
        if { ($r > $a && 2 * $r >= $f) || ($a == 0 && $step <= -$M) } {
            return [list reversed "reversed: $lt goes to 1 when the patch\
 darkens ($lv): flip the pin's active_low on the box"]
        }
        if { abs($step) < 3 } {
            return [list none "none: no change under the detector ($lv): it is\
 not over the patch ($pt_corner)"]
        }
        if { $step < $M } {
            return [list below_min_step "below min_step: the detector sees the\
 patch ($lv), but the box needs a step of $M to learn its threshold: centre\
 the detector or raise the brightness"]
        }
        set word [expr {$a > 0 ? "partial: $a of $f changes answered" : "no_edges: no edge"}]
        set verdict [expr {$a > 0 ? "partial" : "no_edges"}]
        if { $st ne "armed" } {
            return [list $verdict "$word on $lt: the detector sees the patch ($lv),\
 but the box's comparator is $st: flip the patch until it locks, or $relearn"]
        }
        if { $thr ne "" && ($thr <= $lo || $thr >= $hi) } {
            return [list $verdict "$word on $lt: its threshold ($thr counts) is\
 not between the patch's levels ($lv) -- it locked before the detector was\
 where it is: $relearn"]
        }
        return [list $verdict "$word on $lt: $lv, threshold $thr: close to one\
 level, or the light is not settling: $relearn"]
    }

    # lat50s of the answered flips to a direction, us
    proc pt_lats { d } {
        variable pt_flips
        set l {}
        foreach x $pt_flips {
            lassign $x target flipped dk step lat dir
            if { $lat ne "null" && $dir == $d } { lappend l $lat }
        }
        return $l
    }

    proc pt_js { s } { string map [list \\ \\\\ \" \\\" \n " "] $s }
    # (an if, not expr's ?: -- expr would turn "12.000" back into 12.0)
    proc pt_num { x {fmt %.1f} } {
        if { [string is double -strict $x] } { return [format $fmt $x] }
        return null
    }

    proc pt_publish { state verdict summary } {
        variable pt_id
        variable pt_mode
        variable pt_n
        variable pt_k
        variable pt_flips
        variable pt_noise
        variable pt_min_step
        variable pt_last
        variable pt_din
        lassign [pt_counts] f a r
        lassign [pt_dark_light] dark light
        set step [expr {$dark eq "" || $light eq "" ? "" : $light - $dark}]
        set best ""
        foreach x $pt_flips {
            set s [lindex $x 3]
            if { $best eq "" || $s > $best } { set best $s }
        }
        # a line is 0 or 1: no levels to report -- unless it is a box's own
        # comparator, which reports them in counts (pt_cmp_line)
        variable pt_cmp
        variable pt_relock
        if { $pt_din ne "" && ![llength $pt_cmp] } { lassign {} dark light step best }
        if { $summary eq "" && $state eq "running" && $pt_relock ne "" } {
            set summary "relearning the threshold: flip [expr {3 - $pt_relock}] of 3"
        } elseif { $summary eq "" && $state eq "running" } {
            set summary "flip $pt_k of $pt_n"
            # signed as the verdict's and each flip's are: + = the light
            # reads brighter (an inverted detector's counts fall)
            variable pt_sign
            if { $step ne "" } { append summary ": step [format %.0f [expr {$step * $pt_sign}]] counts" }
            if { $best ne "" } { append summary " (best one flip [format %.0f $best])" }
        }
        set json "\{\"id\":$pt_id,\"mode\":\"$pt_mode\",\"state\":\"$state\""
        append json ",\"n\":$pt_n,\"flipped\":$f,\"answered\":$a,\"reversed\":$r"
        append json ",\"source\":\"[expr {[llength $pt_cmp] ? "cmp" : ($pt_din ne "" ? "din" : "analog")}]\""
        append json ",\"verdict\":\"$verdict\",\"summary\":\"[pt_js $summary]\""
        append json ",\"dark\":[pt_num $dark],\"light\":[pt_num $light]"
        append json ",\"step\":[pt_num $step],\"best\":[pt_num $best]"
        append json ",\"noise\":[pt_num $pt_noise %.2f],\"min_step\":[pt_num $pt_min_step]"
        foreach { name d } { rise 1 fall -1 } {
            lassign [pv_stats [pt_lats $d]] n med sd lo hi
            append json ",\"$name\":\{\"n\":$n,\"median\":$med,\"sd\":$sd,\"min\":$lo,\"max\":$hi\}"
        }
        set fl {}
        foreach x [lrange $pt_flips end-49 end] {
            lassign $x target flipped dk step1 lat
            lappend fl "\[$target,$dk,[pt_num $step1],[pt_num [expr {$lat eq "null" ? "" : $lat / 1000.0}] %.3f]\]"
        }
        append json ",\"flips\":\[[join $fl ,]\]\}"
        set pt_last $json
        catch { dservSetData photo/test [now] 11 $json }
        return $summary
    }
    # END photo_test
}

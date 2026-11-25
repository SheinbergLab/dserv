# -*- mode: tcl -*-
#
# ess_stim-1.0.tm
#
# ESS's side of stim2, the subject display: what ESS sends it and what it
# records about it.
#
#   stim_cmd_log     every command sent, as stim/cmd (the rmtSend wrapper)
#   stim_bind        the dserv streams stim2 is bound to, as stim/bind
#   stim_frame_log   stim2's per-frame record (stim/frame) and the tlock
#                    trigger line (tlock/*), with din_resolve, the DI line
#                    resolver tlock and `photo din` share
#   evt_display      which frame showed an event (ess/evt_display)
#   screen_snapshot  the display's geometry and refresh at file open
#   stim_script      the stim script that ran, as stim/script
#   stim_lib         the paradigm libraries shipped, as stim/lib/<name>
#   stim_late_draw   stim2's draw-late scheduler
#
# Extracted from ess-2.0.tm (2026-11-24), the ess_transports way: the same
# `namespace eval ess`, so every cross-reference resolves at call time;
# ess-2.0.tm requires this module at load, and this module never requires
# ess back. ess-2.0.tm calls in from configure_stim, unload_system,
# open_datafile and evt_put.
#
# Two things here run at load, as they did in ess-2.0.tm: stim_cmd_log
# renames the rmt module's C command ::rmtSend to ::ess::rmtSend_raw and
# puts its wrapper in its place, and evt_display adds its listener to
# stim_cmd_hook.
#
# The blocks keep their BEGIN/END markers: the tests evaluate them by text,
# with everything outside stood in (tests/test_ess_stim_cmd.tcl,
# test_ess_evt_display.tcl, test_ess_photo.tcl for din_resolve).

package provide ess_stim 1.0

namespace eval ess {

    # BEGIN stim_cmd_log
    #
    # Every command ESS sends to stim2 is recorded as a stim/cmd datapoint
    # (DSERV_JSON, type 11), one per rmtSend, and the datafile subscribes to
    # it unbuffered and not obs-limited (see open_datafile), so the .ess
    # carries a complete per-command record including the ones between
    # trials. Zero cost on stim2's render thread: the timing fields come
    # from an opt-in reply header stim2 prepends when a command's first
    # char is ASCII '@' (0x40):
    #
    #     ACK1 <ticks_dequeue> <ticks_done> <swapcount> <flipwall_us>\n<result>
    #
    # ticks are stim2's free-running clock in ms (%.3f), swapcount its
    # frame counter, flipwall_us the hardware flip time (CLOCK_REALTIME us;
    # 0 unless the command was a '!' swap-acked one -- "@!cmd" -- and -1
    # when the swapsync strategy has no flip time to report), and
    # <result> is byte-for-byte what the reply would have been without the
    # prefix (possibly empty, possibly "!TCL_ERROR ..."). The wrapper
    # strips the header, so EVERY EXISTING CALLER SEES EXACTLY THE REPLY
    # IT SAW BEFORE. Whether stim2 understands the prefix is probed once
    # per configure_stim (stim_cmd_probe: `stimAckHeader` answers 1 on a
    # new stim2, errors on an old one); against an old stim2 nothing is
    # prefixed and the header fields are null.
    #
    # The JSON record (field by field):
    #   seq      integer; ESS-assigned counter, continuous across stim2
    #            restarts, reset only when ESS starts
    #   cmd      string; the command text as sent, WITHOUT the prefix.
    #            Omitted for commands over 4096 chars (a stim script, a
    #            shipped library) -- those carry instead:
    #   hash     sha256 hex of the UTF-8 command text, and
    #   len      its UTF-8 byte length
    #            (where no in-process sha256 exists -- a harness without
    #            dserv's command -- cmd_head holds the first 256 chars
    #            instead of hash; len is present either way)
    #   t_send   integer us, dserv clock ([now], the evt_put clock), taken
    #            just before the blocking send
    #   t_ret    integer us, same clock, just after the reply arrived
    #   dq       stim2 ms when the command was dequeued; null without header
    #   done     stim2 ms when its evaluation finished; null without header
    #   swap     stim2 frame counter at completion; null without header
    #   flip     integer us CLOCK_REALTIME of the acked flip; null without
    #            header, and null whenever stim2 had none to give -- it
    #            sends 0 for a command that was not swap-acked and -1 when
    #            its swapsync strategy cannot time the flip (glfinish on a
    #            Mac), so any value <= 0 is stored as null
    #   status   "ok"; "error" when the result starts with "!TCL_ERROR";
    #            "io" when the exchange failed (not connected, EOF, short
    #            read: rmt.c closes the socket and returns an empty reply);
    #            "timeout" when such a failure took at least the rmtTimeout
    #            deadline; "nohdr" when a header was requested but the reply
    #            did not start with "ACK1 " (the reply is then returned to
    #            the caller unchanged)
    #
    # This block is self-contained on purpose: tests/test_ess_stim_cmd.tcl
    # evaluates exactly the text between the BEGIN/END markers inside
    # `namespace eval ess`, against the real rmt module and a fake stim2,
    # so keep it free of references to the rest of this file.
    #
    variable stim_ack_header 0
    variable stim_cmd_seq
    if { ![info exists stim_cmd_seq] } { set stim_cmd_seq 0 }
    variable stim_cmd_maxlen 4096
    # command prefixes, each called as `{*}$hook seq cmd swap t_ret flip`
    # after every command that came back with a swap number (flip: stim2's
    # wall-clock us, or null); a hook's error is ignored
    variable stim_cmd_hook
    if { ![info exists stim_cmd_hook] } { set stim_cmd_hook {} }

    # JSON string escaping WITHOUT any Tcl evaluation of the text. The
    # output is pure printable ASCII: besides being what JSON requires for
    # quotes, backslashes and control characters, dservSetData hands the
    # value to Tcl_GetByteArrayFromObj, which refuses (or mangles) a string
    # with chars outside 0..255 -- so non-ASCII must be \uXXXX-escaped too.
    proc json_escape { s } {
        # fast path: nothing to escape (the common case for stim commands)
        if { ![regexp {[^\u0020-\u007e]|[\\"]} $s] } { return $s }
        set s [string map [list \\ \\\\ \" \\\" \n \\n \r \\r \t \\t \b \\b \f \\f] $s]
        if { ![regexp {[^\u0020-\u007e]} $s] } { return $s }
        set out ""
        foreach ch [split $s ""] {
            scan $ch %c code
            if { $code >= 0x20 && $code < 0x7f } {
                append out $ch
            } elseif { $code > 0xffff } {
                set code [expr {$code - 0x10000}]
                append out [format {\u%04x\u%04x} \
                                [expr {0xd800 + ($code >> 10)}] \
                                [expr {0xdc00 + ($code & 0x3ff)}]]
            } else {
                append out [format {\u%04x} $code]
            }
        }
        return $out
    }

    # Split an ACK1 header off a reply. Returns {1 dq done swap flip result}
    # when the reply carries a well-formed header, {0} otherwise.
    proc stim_cmd_parse_ack { reply } {
        if { [string range $reply 0 4] ne "ACK1 " } { return 0 }
        set nl [string first \n $reply]
        if { $nl < 0 } {
            set line $reply
            set result ""
        } else {
            set line [string range $reply 0 [expr {$nl - 1}]]
            set result [string range $reply [expr {$nl + 1}] end]
        }
        if { ![regexp {^ACK1 (\S+) (\S+) (\S+) (\S+)$} $line -> dq done swap flip] } {
            return 0
        }
        if { ![string is double -strict $dq] || ![string is double -strict $done] ||
             ![string is wideinteger -strict $swap] ||
             ![string is wideinteger -strict $flip] } {
            return 0
        }
        return [list 1 $dq $done $swap $flip $result]
    }

    # The stim/cmd record text. Header fields are passed as "null" when
    # there was no header; numbers are emitted as given.
    proc stim_cmd_json { seq cmd t_send t_ret dq done swap flip status } {
        variable stim_cmd_maxlen
        set json "\{\"seq\":$seq"
        if { [string length $cmd] > $stim_cmd_maxlen } {
            set bytes [encoding convertto utf-8 $cmd]
            if { [info commands ::sha256] ne "" } {
                append json ",\"hash\":\"[::sha256 $cmd]\""
            } else {
                append json ",\"cmd_head\":\"[json_escape [string range $cmd 0 255]]\""
            }
            append json ",\"len\":[string length $bytes]"
        } else {
            append json ",\"cmd\":\"[json_escape $cmd]\""
        }
        append json ",\"t_send\":$t_send,\"t_ret\":$t_ret"
        append json ",\"dq\":$dq,\"done\":$done,\"swap\":$swap,\"flip\":$flip"
        append json ",\"status\":\"$status\"\}"
        return $json
    }

    # Ask stim2 whether it speaks the header. Called from configure_stim on
    # every (re)connect so a restarted stim2 of another version is picked up;
    # uses the raw command, so the probe itself is not a stim/cmd record.
    proc stim_cmd_probe {} {
        variable stim_ack_header
        set r ""
        catch { set r [::ess::rmtSend_raw stimAckHeader] }
        set stim_ack_header [expr {$r eq "1"}]
        return $stim_ack_header
    }

    # The rmtSend wrapper proper (::rmtSend tailcalls here).
    proc stim_cmd_send { args } {
        variable stim_ack_header
        variable stim_cmd_seq
        set cmd [lindex $args 0]
        set hdr $stim_ack_header
        if { $hdr } { lset args 0 @$cmd }

        set t_send [now]
        set reply [::ess::rmtSend_raw {*}$args]
        set t_ret [now]

        set seq [incr stim_cmd_seq]
        set dq null; set done null; set swap null; set flip null
        set status ok
        if { $reply eq "" } {
            # an empty reply is also what a failed exchange looks like
            # (rmt.c: "so a system can run with no display attached"); the
            # link state tells them apart. rmtConnected is a non-blocking
            # peek, paid only on empty replies.
            set connected 1
            catch { set connected [rmtConnected] }
            if { $connected eq "0" } {
                set status io
                set deadline_ms 0
                catch { set deadline_ms [rmtTimeout] }
                if { $deadline_ms > 0 && $t_ret - $t_send >= $deadline_ms * 1000 } {
                    set status timeout
                }
            }
        } elseif { $hdr } {
            set ack [stim_cmd_parse_ack $reply]
            if { [lindex $ack 0] } {
                lassign $ack - dq done swap flip reply
                # stim2 reports 0 when the command was not swap-acked and
                # -1 when its swapsync strategy has no hardware flip time
                # (glfinish on a Mac): neither is a timestamp, so neither
                # reaches a reader as one
                if { $flip <= 0 } { set flip null }
            } else {
                set status nohdr
            }
        }
        if { $status eq "ok" && [string match "!TCL_ERROR*" $reply] } {
            set status error
        }

        if { [catch {
            dservSetData stim/cmd $t_ret 11 \
                [stim_cmd_json $seq $cmd $t_send $t_ret $dq $done $swap $flip $status]
        } err] } {
            catch { ess_warning "stim/cmd record $seq not written: $err" "stim" }
        }
        # listeners for the swap each command landed on (photo_verify pairs
        # '!' commands with the patch's changes; evt_display links events)
        variable stim_cmd_hook
        if { $swap ne "null" } {
            foreach h $stim_cmd_hook {
                catch { {*}$h $seq $cmd $swap $t_ret $flip }
            }
        }
        return $reply
    }

    # Take over the rmt module's C command once; a reload of this file
    # finds rmtSend_raw already in place and only redefines the wrapper.
    if { [info commands ::ess::rmtSend_raw] eq "" && [info commands ::rmtSend] ne "" } {
        rename ::rmtSend ::ess::rmtSend_raw
    }
    proc ::rmtSend { args } { tailcall ::ess::stim_cmd_send {*}$args }
    # END stim_cmd_log

    # BEGIN stim_bind
    #
    # Which dserv streams stim2 is bound to, published as stim/bind
    # (DSERV_JSON, type 11) on every configure_stim and logged into the
    # datafile along with the streams themselves.
    #
    # A stim script that uses stim2's stimess package (stim::bind and
    # friends) subscribes to datapoints -- the dial pointer, planko's
    # catcher -- and reads them at frame time. Those streams ARE the
    # subject's behaviour as the display saw it, yet whether they reached
    # the .ess file depended on each protocol remembering its own
    # dservLoggerAddMatch. stim2 knows exactly what it is bound to, so ESS
    # asks it and logs that:
    #
    #   configure_stim          rmtSend {catch {stim::unbind_all}} right after
    #                           the probe, so a reload starts with no
    #                           bindings left over from the previous script
    #                           (the catch makes it a no-op on a stim2 without
    #                           the package); then, after the stim script
    #                           section, stim_bind_publish asks
    #                           `stim::bindings_json` and publishes the reply.
    #   stim/bind               the table stim2 answered, verbatim:
    #                             {"package":"stimess 1.0",
    #                              "bindings":[{"name":"ess/dial/pointer",
    #                                           "fields":["x","y","show","band"],
    #                                           "mode":"latest","sep":","}, ...]}
    #                           or {"package":"","bindings":[]} when stim2 has
    #                           no stimess (an old stim2 answers the query with
    #                           "!TCL_ERROR invalid command name ..."), when it
    #                           is not connected, or when its reply did not
    #                           parse. Timestamp [now]. Re-emitted into each
    #                           datafile at open (dservTouch, like stimdg).
    #   stim_bound_names        the "name" of every binding, in table order --
    #                           what open_datafile adds matches for.
    #
    # Logging policy for the bound streams (open_datafile): obs_limited,
    # every record, UNBUFFERED. The log buffer concatenates the raw data
    # bytes of successive points and keeps only the first timestamp; that is
    # fine for fixed-width samples (eyetracking/raw, em/*) and destructive
    # for these, which are delimited text ("x,y,show,band" -- the table's
    # "sep" is there because they are) of varying length: two buffered
    # points would fuse into one unreadable string. A protocol that adds its
    # own match for the same name (its file_open callback runs after these)
    # replaces ours: LogMatchDict::insert keys by the match string, so a
    # repeated pattern never yields duplicate records.
    #
    # Parsing: tcljson's json_to_dict (in dlsh.zip, already required at the
    # top of this file in the ess interp). stim_bind_names_regexp is a LAST
    # RESORT for an interp without dlsh -- the ctest harness in CI -- and
    # extracts "name":"..." values textually, without validating the rest
    # of the document. It is never reached where tcljson loads.
    #
    # Self-contained like the stim_cmd_log block: tests/test_ess_stim_cmd.tcl
    # evaluates the text between these markers in `namespace eval ess`.
    #
    variable stim_bound_names {}
    variable stim_bind_empty {{"package":"","bindings":[]}}

    # Minimal textual extraction of the "name" values -- see the block
    # comment: only for an interp where tcljson cannot be loaded.
    proc stim_bind_names_regexp { json } {
        set names {}
        foreach { - n } [regexp -all -inline {"name"\s*:\s*"((?:[^"\\]|\\.)*)"} $json] {
            lappend names [string map {{\"} \" {\\} \\ {\/} /} $n]
        }
        return $names
    }

    # The bound names in a bindings_json table. Throws on a document
    # tcljson rejects (the caller publishes the empty table then).
    proc stim_bind_names { json } {
        if { [info commands ::json_to_dict] eq "" } {
            catch { package require tcljson }
        }
        if { [info commands ::json_to_dict] eq "" } {
            return [stim_bind_names_regexp $json]
        }
        set d [::json_to_dict $json]
        set names {}
        if { [dict exists $d bindings] } {
            foreach b [dict get $d bindings] {
                if { [dict exists $b name] } { lappend names [dict get $b name] }
            }
        }
        return $names
    }

    # Ask stim2 for its binding table and publish it as stim/bind; set
    # stim_bound_names from it. With query 0 nothing is asked (stim2 is not
    # connected, or a load is about to re-source the stim script): the empty
    # table is published so stim/bind never carries a previous load's
    # bindings. Returns the names.
    proc stim_bind_publish { {query 1} } {
        variable stim_bound_names
        variable stim_bind_empty
        set reply ""
        if { $query } {
            catch { set reply [::rmtSend stim::bindings_json] }
        }
        set reply [string trim $reply]
        set names {}
        set table $stim_bind_empty
        if { [string index $reply 0] eq "\{" } {
            if { [catch { set names [stim_bind_names $reply] } err] } {
                catch { ess_warning "stim/bind: binding table from stim2 did not parse ($err); published empty" "stim" }
                set names {}
            } else {
                set table $reply
            }
        }
        set stim_bound_names $names
        # dservSetData wants a byte string (Tcl_GetByteArrayFromObj): the
        # UTF-8 bytes are what a JSON reader expects anyway
        if { [catch {
            dservSetData stim/bind [now] 11 [encoding convertto utf-8 $table]
        } err] } {
            catch { ess_warning "stim/bind not written: $err" "stim" }
        }
        return $stim_bound_names
    }
    # END stim_bind

    # BEGIN stim_frame_log
    #
    # stim2's per-frame record, stim/frame, switched on by ESS for as long
    # as a system is loaded and written into every datafile.
    #
    # stim2 (new enough) has `stimFrameLog ?<host> 0|1?`: with 1 it starts a
    # background writer that pushes ONE stim/frame datapoint per buffer
    # swap to dserv's binary return port (4620; <host> may be host:port) --
    # a DSERV_INT64 array of 7 values {frame swapcount flip_us ticks_us
    # missed cmds_applied ds_dispatched}, where frame is 1-based and
    # contiguous as received (drops show as swapcount gaps), flip_us is the
    # hardware flip time (CLOCK_REALTIME us; -1 on a glfinish Mac) and
    # ticks_us is stim2's free-running clock in MICROseconds -- the ACK1
    # header's dq/done and StimTicksF are the same clock in ms, so
    # ticks_us / 1000.0 lines up with them. With 0 it stops the writer.
    # EVERY form answers the status dict {enabled 0|1 host <h> sent <n>
    # dropped <n> connected 0|1} (a start answers at once with connected 0:
    # the writer thread connects asynchronously); the bare form is the
    # probe. An older stim2 answers "!TCL_ERROR invalid command name ...",
    # and then nothing here does anything. <host> is the dserv host as
    # stim2 knows it: the same value configure_stim connects with ($host,
    # stim2's ::dservhost), so the writer and the command socket point at
    # the same dserv.
    #
    #   configure_stim          stim_frame_enable [dservGet ess/ipaddr] (the
    #                           address stim2 sends datapoints back to; $host
    #                           is the stim2 host and is only a fallback when
    #                           ess/ipaddr is unset), right after the ACK
    #                           header probe (raw command, so like the probe
    #                           it is not a stim/cmd record). Sets
    #                           stim_frame_log to 1 when stim2 took it (the
    #                           status dict, or anything that is not an
    #                           error), 0 on an error reply; remembers the
    #                           host. Re-done
    #                           on every connect, so a restarted stim2 of
    #                           another version is picked up. The not-
    #                           connected branch records 0.
    #   unload_system           stim_frame_disable, after the system's deinit
    #                           callback: `stimFrameLog <host> 0`, only when
    #                           the flag is 1, under catch (stim2 may be gone
    #                           -- the reply is ignored either way), and the
    #                           flag goes back to 0. A reload is unload then
    #                           configure_stim, so the writer is off between
    #                           systems and on again with the next one.
    #   stim/frame              stim2's record, as it arrives on 4620.
    #
    # Logging policy (open_datafile, stim_frame_log_match): NOT obs-limited
    # -- a frame-time gap between trials is exactly what the record is for
    # -- every record, UNBUFFERED, one frame per record.
    #
    # Deliberately so (2026-10-26). The buffer argument of
    # dservLoggerAddMatch is BYTES, not points (LogClient.cpp: a point is
    # appended while it fits and the buffer flushed when the next would not),
    # and this match used to say 100, which with a 56-byte frame still gave
    # one frame per record, only held until the next frame arrived. Batching
    # 100 frames (5600) would cut a training file by about a quarter -- in a
    # 52-minute motiondir file stim/frame was 258,705 records and two thirds
    # of 34.6 MB -- but it was turned down because a batched record keeps
    # only its first frame's dserv timestamp, and that per-frame receipt
    # stamp is what places each frame against the commands and datapoints
    # around it and what measures the cross-host clock (receipt minus
    # flip_us) frame by frame; and because dslog::unpack / jsonTable expect
    # one 7-field sample per record. If size ever matters, thin the stream
    # at the source instead (stim2 sending only frames whose counters moved,
    # plus a heartbeat), which keeps a true timestamp on every record.
    #
    # Added only when the flag is 1 (an old stim2 never sends the stream).
    # The record is an int64 array: dslog::read exposes it only under
    # `dslog::wideTypes 1` -- the default read (dslog.c, wide types off)
    # DROPS DSERV_INT64 values, so a <ds>stim/frame column is empty there.
    #
    # Self-contained like the two blocks above: tests/test_ess_stim_cmd.tcl
    # evaluates the text between these markers in `namespace eval ess`.
    #
    variable stim_frame_log 0
    variable stim_frame_host {}

    # Ask stim2 to start its per-frame writer towards <host>. Returns the
    # flag it set.
    proc stim_frame_enable { host } {
        variable stim_frame_log
        variable stim_frame_host
        set stim_frame_host $host
        set r "!TCL_ERROR not sent"
        catch { set r [::ess::rmtSend_raw [list stimFrameLog $host 1]] }
        set stim_frame_log [expr {[string match "!TCL_ERROR*" $r] ? 0 : 1}]
        return $stim_frame_log
    }

    # Stop the writer, if this ESS started one. Never throws; returns 1 when
    # a stop was sent.
    proc stim_frame_disable {} {
        variable stim_frame_log
        variable stim_frame_host
        if { !$stim_frame_log } { return 0 }
        set stim_frame_log 0
        catch { ::ess::rmtSend_raw [list stimFrameLog $stim_frame_host 0] }
        return 1
    }

    # The datafile match for stim/frame (open_datafile): see the logging
    # policy above. Returns 1 when a match was added.
    proc stim_frame_log_match { filename } {
        variable stim_frame_log
        if { !$stim_frame_log } { return 0 }
        dservLoggerAddMatch $filename stim/frame 0 0 1
        # Records stim2's modules send on the same stream, one per frame
        # they drew (stimFrameRecord). stim/gazewin: which eye sample each
        # gaze-window frame was drawn from (stim2 docs/gazewin.md). Nothing
        # arrives, and nothing is logged, unless that module is in use.
        dservLoggerAddMatch $filename stim/gazewin 0 0 1
        # where each draw-late frame's time went (stim2 docs/late-draw.md)
        dservLoggerAddMatch $filename stim/latedraw 0 0 1
        # stim/tlock: which swap carried each CRS T-Lock trigger pulse to a
        # Display++, and the pulse (stim2 docs/tlock.md).
        dservLoggerAddMatch $filename stim/tlock 0 0 1
        # stim/photopatch: which swap changed the photodiode's patch, and to
        # which level (stim2 docs/photopatch.md)
        dservLoggerAddMatch $filename stim/photopatch 0 0 1
        # ... and the input its trigger output is wired to, where declared
        stim_tlock_din_match $filename
        return 1
    }
    # The rig's display is a CRS Display++ whose trigger output stim2 should
    # drive (setting `tlock enabled`, essconf.tcl; stim2 docs/tlock.md).
    # Cached here; configure_stim acts on it at each system load.
    variable display_tlock 0
    proc set_display_tlock {on} {
        variable display_tlock [expr {$on ? 1 : 0}]
        catch { display_tlock_health }
    }

    # The output bits toggle mode flips on each frame (setting `tlock
    # toggle`; 0: toggle mode is left to the stim script). The setting's
    # value is normalised to 0xNNNN by display_tlock_toggle_norm.
    variable display_tlock_toggle 0
    proc display_tlock_toggle_norm {v} {
        set v [string trim $v]
        if { $v eq "" || [string tolower $v] in {off none no false} } { return 0x0000 }
        if { ![string is integer -strict $v] || $v < 0 || $v > 0xFFFF } {
            error "tlock toggle: the output bits as a number from 0 to 0xFFFF\
 (0x0001 is DOUT 0 on the DB25, 0x4000 the trigger-out BNC; 0 is off)"
        }
        return [format "0x%04X" $v]
    }
    proc set_display_tlock_toggle {v} {
        variable display_tlock_toggle [expr {int([display_tlock_toggle_norm $v])}]
    }

    # Turn stim2's tlock pass on, when this rig declares it. Called by
    # configure_stim right after the prelude, whose `postpass reset` turns
    # every pass off: so a rig that does not declare it never has the pass
    # on, whatever the last system did. Sent with rmtSend, so the data file
    # records it and a replay draws the same row. The command is guarded
    # rather than caught: a stim2 without the tlock module answers -1, and
    # that is a warning here, not an error there. Returns 1 if the pass is
    # on, 0 if the rig does not ask for it, -1 if stim2 cannot.
    proc stim_tlock_enable {} {
        variable display_tlock
        variable display_tlock_stim
        if { !$display_tlock } { return 0 }
        set r [rmtSend {expr {[llength [info commands tlock]] ? [tlock on] : -1}}]
        if { [string trim $r] eq "1" } {
            set display_tlock_stim 1
            # toggle mode, where the rig asks for it (setting `tlock
            # toggle`): these outputs change level on every frame drawn, so
            # every session on this rig marks its frames with no word from
            # the stim script. Recorded like the `tlock on` above.
            variable display_tlock_toggle
            if { $display_tlock_toggle != 0 } {
                rmtSend [format "tlock toggle 0x%04X" $display_tlock_toggle]
            }
            catch { display_tlock_health }
            return 1
        }
        set display_tlock_stim -1
        catch { display_tlock_health }
        ::ess::ess_warning "this rig declares a Display++ trigger (tlock enabled)\
 but stim2 has no tlock command: no trigger packets are drawn" "stim"
        return -1
    }
    # The digital input the Display++ trigger output is wired to (setting
    # `tlock din`, essconf.tcl), so the data file carries the
    # edges beside stim/tlock without the analysis knowing the wiring. Named
    # the way `dial stick_commit` names its press: by LABEL, so it follows
    # whichever box is present and survives a re-pin. Three spellings:
    #
    #   <label>     a DI line's own label (extio/<dev>/state/label/<pin>):
    #               its edges, extio/<dev>/state/di/<pin>. Looked for first.
    #               Or a group's name: extio/*/state/group/<label>.
    #   a/b/c       anything with a slash is a datapoint (or a match
    #               pattern) as written
    #   ""          none
    #
    # Prefer a plain DI line over a chord group for toggle mode: a group is
    # chord-settled by the firmware, and edges a refresh apart may not
    # survive that.
    variable display_tlock_din ""
    proc set_display_tlock_din {v} {
        variable display_tlock_din [string trim $v]
        if { $display_tlock_din ne "" } { catch { din_watch_boxes } }
        catch { display_tlock_din_publish }
        catch { display_tlock_health }
    }

    proc display_tlock_din_resolve {} {
        variable display_tlock_din
        din_resolve $display_tlock_din
    }

    # BEGIN din_resolve
    # A digital input line, by label, resolved where it is right now: a
    # dict of status (none | ok | unresolved), kind (di | group | path), dp
    # (the datapoint or match pattern; "" unless ok) and detail. Resolved
    # when asked, never cached: boxes come and go. Shared by `tlock din` and
    # `photo din` (photo_din_resolve).
    #
    #   label       the DI line labelled so, on whichever CONNECTED box
    #               announces it (<io>/<dev>/state/di/<pin>); unresolved if
    #               more than one line carries it; else the chord group of
    #               that name (<io>/*/state/group/<label>)
    #   a/b...      a datapoint name, taken as written
    #   ""          none
    proc din_resolve { v } {
        if { $v eq "" } {
            return [dict create status none kind "" dp "" detail "no input declared"]
        }
        if { [string match */* $v] } {
            return [dict create status ok kind path dp $v detail "datapoint $v"]
        }
        set io [din_io]
        # a DI line labelled so: label datapoints are <io>/<dev>/state/label/<pin>.
        # Only on a connected box: one that goes away leaves its whole
        # state/ tree in dserv, labels too (nothing pushes deletions), and
        # its line would resolve -- or count as a duplicate -- with nothing
        # behind it.
        set live [din_live_boxes $io]
        set keys {}
        catch { set keys [dservKeys $io/*/state/label/*] }
        set lines {}
        set gone {}
        foreach k [lsort $keys] {
            set lab ""
            catch { set lab [string trim [lindex [split [dservGet $k] \x00] 0]] }
            if { $lab ne $v } continue
            set parts [split $k /]
            set l [list [lindex $parts 1] [lindex $parts end]]
            if { $live ne "*" && [lindex $l 0] ni $live } {
                lappend gone $l
            } else {
                lappend lines $l
            }
        }
        # Two lines with one label (two boxes, or two pins) is not a choice
        # to make by sort order: the first name would win silently, and it
        # may be the box without the sensor. Say so instead; a datapoint
        # name (a/b...) picks one.
        if { [llength $lines] > 1 } {
            set where {}
            foreach l $lines { lassign $l dev pin ; lappend where "$dev pin $pin" }
            lassign [lindex $lines 0] dev pin
            return [dict create status unresolved kind di dp "" \
                        detail "$v labels [llength $lines] lines ([join $where {, }]):\
 relabel all but one, or name the datapoint (e.g. $io/$dev/state/di/$pin)"]
        }
        if { [llength $lines] == 1 } {
            lassign [lindex $lines 0] dev pin
            return [dict create status ok kind di dp $io/$dev/state/di/$pin \
                        detail "$dev pin $pin, labelled $v"]
        }
        # a chord group of that name, on whichever box announces it
        set keys {}
        catch { set keys [dservKeys $io/*/state/group/$v/pins] }
        if { [llength $keys] } {
            set dev [lindex [split [lindex [lsort $keys] 0] /] 1]
            return [dict create status ok kind group dp $io/*/state/group/$v \
                        detail "group $v on $dev"]
        }
        if { [llength $gone] } {
            set where {}
            foreach l $gone { lassign $l dev pin ; lappend where "$dev pin $pin" }
            set which [expr {[llength $gone] > 1 ? "none of those boxes is" : "that box is not"}]
            return [dict create status unresolved kind di dp "" \
                        detail "$v is [join $where {, }], but $which connected"]
        }
        return [dict create status unresolved kind "" dp "" \
                    detail "no box announces a line or a group labelled $v"]
    }

    proc din_io {} {
        if { [info exists ::ess::io_class] && $::ess::io_class ne "" } { return $::ess::io_class }
        return extio
    }
    # The connected boxes, by device name: the roster extioconf keeps from
    # announces (<io>/boxes; input_boxes reads it the same way). "*" when
    # there is no roster at all -- an extioconf too old to publish one --
    # and every box counts.
    proc din_live_boxes { io } {
        if { [catch { dservExists $io/boxes } have] || !$have } { return * }
        set r {}
        catch { set r [dservGet $io/boxes] }
        return $r
    }
    # A line in use (photo din, tlock din) follows the boxes: when the
    # roster changes, resolve again and refresh the green lights, so
    # unplugging a box -- or the second box with the same label -- shows at
    # once instead of at the next setting change or obs end. One script on
    # the roster datapoint, shared by both.
    variable din_boxes_watched 0
    proc din_watch_boxes {} {
        variable din_boxes_watched
        if { $din_boxes_watched } return
        set din_boxes_watched 1
        set dp [din_io]/boxes
        dservAddExactMatch $dp
        dpointSetScript $dp ::ess::din_boxes_changed
    }
    proc din_boxes_changed { dpoint data } {
        variable photo_enabled
        variable display_tlock
        if { [info exists photo_enabled] && $photo_enabled } {
            catch { photo_din_attach }
            catch { photo_health }
        }
        if { [info exists display_tlock] && $display_tlock } {
            catch { display_tlock_din_publish }
            catch { display_tlock_health }
        }
    }
    # END din_resolve

    # tlock/din: the resolution, for a page and for the data
    # file (which stream the trigger edges are)
    proc display_tlock_din_publish {} {
        set r [display_tlock_din_resolve]
        dservSet tlock/din $r
        return $r
    }

    # The datafile matches for the trigger input (open_datafile, through
    # stim_frame_log_match): the line's edges, every one, unbuffered, and
    # the record saying which datapoint that is (re-emitted once the logger
    # runs: open_datafile's dservTouch). Only on a rig that draws
    # the packets (display_tlock) and declares the input. Returns 1 when
    # the edges were matched.
    proc stim_tlock_din_match { filename } {
        variable display_tlock
        if { !$display_tlock } { return 0 }
        catch { display_tlock_health }
        set r [display_tlock_din_publish]
        switch -- [dict get $r status] {
            none { return 0 }
            unresolved {
                ::ess::ess_warning "Display++ trigger input (tlock din):\
 [dict get $r detail] -- its edges are not in this data file" "stim"
                return 0
            }
        }
        dservLoggerAddMatch $filename tlock/din 0 0 1
        dservLoggerAddMatch $filename [dict get $r dp] 0 0 1
        # The clock behind those edge stamps. A box maps its own timer onto
        # dserv time from anchors (extio-zephyr box_clock.h): state/sync/*
        # is each anchor's offset, the learned rate, the last raw rate
        # sample and how many were refused, and state/in_obs is when the
        # box itself saw the obs begin and end (it stops re-anchoring in
        # between). Without them a step or a slope in edge-minus-flip
        # cannot be put down to an anchor or ruled out as one. About one
        # set a second outside an obs; only for a box DI line, where the
        # device is known.
        set dp [dict get $r dp]
        if { [dict get $r kind] eq "di" &&
             [regexp {^(.+/state)/di/[^/]+$} $dp -> base] } {
            dservLoggerAddMatch $filename $base/sync/* 0 0 1
            dservLoggerAddMatch $filename $base/in_obs 0 0 1
        }
        return 1
    }

    # THE GREEN LIGHT (tlock/health, the gear's dot on the tlock section;
    # see settings-1.0.tm). It answers the question the two knobs cannot:
    # packets are going out -- are edges coming back?
    #
    #   off    not enabled on this rig, or enabled with no input declared
    #   error  stim2 has no tlock command; the label resolves to nothing;
    #          or stim2 has sent trigger frames (stim/tlock) and the input's
    #          last edge is more than 2 s older than the last of them: a
    #          loose BNC, a monitor not reading the packet, the wrong line
    #   ok     the input is there, and its edges keep up with the packets
    #          (or none has been sent yet)
    #
    # It costs nothing between trials: no subscription to the edges, which
    # come at up to the refresh rate. It is worked out when a setting
    # changes, at a system load, at a data file's open and at each end_obs,
    # from two datapoint timestamps; and published only when it changes.
    variable display_tlock_stim 0     ;# 1 stim2 took `tlock on`, -1 it cannot
    variable display_tlock_health_last "?"
    proc display_tlock_health {} {
        variable display_tlock
        variable display_tlock_stim
        variable display_tlock_health_last
        if { !$display_tlock } {
            set h "off not enabled on this rig"
        } elseif { $display_tlock_stim < 0 } {
            set h "error stim2 has no tlock command: no trigger packets are drawn"
        } else {
            set r [display_tlock_din_resolve]
            switch -- [dict get $r status] {
                none       { set h "off packets are drawn; no input is declared for the trigger" }
                unresolved { set h "error [dict get $r detail]" }
                default {
                    # the newest edge on the line (a group's datapoint is a
                    # pattern over boxes) against the newest trigger frame
                    set t_edge 0
                    set dp [dict get $r dp]
                    set keys [list $dp]
                    if { [string match {*[*]*} $dp] } {
                        set keys {}
                        catch { set keys [dservKeys $dp] }
                    }
                    foreach k $keys {
                        set t 0
                        catch { set t [dservTimestamp $k] }
                        if { [string is wideinteger -strict $t] && $t > $t_edge } { set t_edge $t }
                    }
                    set t_pkt 0
                    catch { set t_pkt [dservTimestamp stim/tlock] }
                    if { ![string is wideinteger -strict $t_pkt] } { set t_pkt 0 }
                    if { $t_pkt == 0 } {
                        set h "ok [dict get $r detail]; no trigger sent yet"
                    } elseif { $t_edge == 0 } {
                        set h "error [dict get $r detail]: triggers sent, no edge ever seen"
                    } elseif { $t_pkt - $t_edge > 2000000 } {
                        set h "error [dict get $r detail]: triggers sent, but the last edge\
 is [expr {($t_pkt - $t_edge) / 1000000}] s older than the last of them"
                    } else {
                        set h "ok [dict get $r detail]; edges arriving"
                    }
                }
            }
        }
        if { $h ne $display_tlock_health_last } {
            set display_tlock_health_last $h
            dservSet tlock/health $h
        }
        return $h
    }
    # END stim_frame_log

    # BEGIN evt_display
    #
    # Which frame showed an event. A system changes the display with a '!'
    # command and puts the event that names it in the same action:
    #
    #     $sys add_action sample_on {
    #         my sample_on                    ;# rmtSend "!sample_on"
    #         ::ess::evt_put SAMPLE ON [now]
    #     }
    #
    # so an event put in an action after a '!' command is linked to the
    # action's last one. The event itself is unchanged (its time is still
    # when it was put); beside it goes
    #
    #   ess/evt_display   JSON, stamped at the event's time, so it is in the
    #                     event's obs: type subtype (as evt_put was given
    #                     them), t (the event's time), and the command's seq
    #                     (stim/cmd), swap, and flip (stim2's flip on the dserv
    #                     clock, us; null where stim2 has no flip time). The
    #                     light is not here: it comes 300 ms later as the
    #                     swap's photo/pair, and df::add_display_times joins
    #                     the two by swap.
    #
    # Events put before the action's first '!' command, in a transition or
    # in a callback outside an action are not linked. evt_put_shown links
    # one to the last '!' command sent anywhere, for a system that changes
    # the display from a callback. Only a stim2 with the ACK1 header reports
    # swaps (stim_cmd_log); with an older one nothing is linked.
    #
    # A flip from stim2's host is on its wall clock: ed_off (dserv clock
    # minus this host's wall clock) puts it on dserv's, which is right when
    # stim2 runs here or the two hosts share PTP. The light, from the
    # photodiode, is on dserv's clock either way.
    #
    # Self-contained: tests/test_ess_evt_display.tcl evaluates it.
    #
    variable ed_action 0        ;# 1 while a state's action runs
    variable ed_cmd {}          ;# the action's last '!' command {seq swap flip}
    variable ed_last {}         ;# the last '!' command anywhere
    variable ed_force 0         ;# evt_put_shown: link to ed_last
    variable ed_off {}          ;# dserv clock - wall clock, us ("" unknown)

    # System do_action sets ed_action 1 (and ed_cmd {}) around each action,
    # inline; evt_put calls ed_after_put only when ed_cmd or ed_force is set
    # the stim_cmd_log hook
    proc ed_cmd_heard { seq cmd swap t_ret flip } {
        if { [string index $cmd 0] ne "!" } return
        variable ed_action
        variable ed_last [list $seq $swap $flip]
        if { $ed_action } { variable ed_cmd $ed_last }
    }
    # a data file opened: the clock offset again (both clocks are slewed
    # together; only a step changes it)
    proc ed_new_file { off } { variable ed_off $off }

    # evt_put calls this after each event
    proc ed_after_put { type subtype t } {
        variable ed_cmd
        variable ed_force
        variable ed_last
        if { $ed_cmd ne "" } {
            set link $ed_cmd
        } elseif { $ed_force && $ed_last ne "" } {
            set link $ed_last
        } else {
            return
        }
        variable ed_off
        lassign $link seq swap flip
        if { $flip eq "null" || $flip eq "" || $ed_off eq "" } {
            set f null
        } else {
            set f [expr {wide($flip) + $ed_off}]
        }
        set q [list \" {} \\ {}]
        catch {
            dservSetData ess/evt_display [expr {wide($t)}] 11 \
                "\{\"type\":\"[string map $q $type]\",\"subtype\":\"[string map $q $subtype]\",\"t\":[expr {wide($t)}],\"seq\":$seq,\"swap\":$swap,\"flip\":$f\}"
        }
    }

    # evt_put, linked to the last '!' command wherever it was sent
    proc evt_put_shown { type subtype time args } {
        variable ed_force 1
        try {
            evt_put $type $subtype $time {*}$args
        } finally {
            set ed_force 0
        }
    }

    variable stim_cmd_hook
    if { ![info exists stim_cmd_hook] } { set stim_cmd_hook {} }
    if { "::ess::ed_cmd_heard" ni $stim_cmd_hook } { lappend stim_cmd_hook ::ess::ed_cmd_heard }
    # END evt_display

    # BEGIN screen_snapshot
    #
    # The display as the data file will see it, asked of stim2 when a file
    # opens: half-screen degrees, window size, refresh and frame duration --
    # the values configure_stim publishes as ess/screen_* -- and the current
    # background colour, published as ess/screen_bg "r g b" (0-255).
    #
    # Asked again at the end of every load and variant reload, after the
    # variant's init (which may set the background), so ess/screen_bg
    # describes the display between files too -- what a live mirror that
    # joins mid-session sets itself up from.
    #
    # Asked fresh at open, not trusted from configure time: file_open logs
    # ess/screen_* only if the datapoints exist, and files have been written
    # without them on a dserv that had them hours before and after
    # (human_emcalib_9point_261027163255.ess, officepi, 2026-10-27). The
    # background is recorded nowhere else, and a stim script that never
    # calls setBackground draws on whatever an earlier one left; a replay
    # cannot know it otherwise.
    #
    # Sent with rmtSend_raw, before the file opens: not a stim/cmd record, so
    # a replay never re-sends the query. One request for all nine values.
    # With no stim2, or a reply that is not nine values, the datapoints are
    # left as configure_stim set them and 0 is returned; 1 when refreshed.
    #
    # The window size is the one exception to "asked fresh": ess/screen_w|h
    # are the pixel grid of this load, the size configure_stim recorded for
    # the state system.  touch_init scales touch pixels to degrees by that
    # size for the whole load, and stim2's mouse-as-touch bridge (stimess)
    # reports in the grid it saw at configure, so a window resized since
    # the load (a dev machine) must not move the datapoints onto another
    # grid than the touches are in.  The degrees do not change with the
    # window (a resize stretches the projection), so they, the refresh and
    # the background are taken live.  With no recorded size (no state
    # system), the live size stands.
    variable screen_snapshot_query {list [screen_set HalfScreenDegreeX] [screen_set HalfScreenDegreeY] [screen_set WinWidth] [screen_set WinHeight] [screen_set ScaleX] [screen_set ScaleY] [screen_set RefreshRate] [screen_set FrameDuration] [setBackground]}

    proc screen_snapshot {} {
        variable screen_snapshot_query
        set r "!TCL_ERROR not sent"
        catch { set r [::ess::rmtSend_raw $screen_snapshot_query] }
        if { [string match "!TCL_ERROR*" $r] || [catch {llength $r} n] || $n != 9 } {
            return 0
        }
        lassign [lmap v $r {string trim $v}] hx hy ww wh sx sy hz fd bg
        if { [catch {
            set w [expr {int($ww / ($sx > 0 ? $sx : 1.0))}]
            set h [expr {int($wh / ($sy > 0 ? $sy : 1.0))}]
            if { [llength $bg] != 3 } { error "background \"$bg\"" }
        }] } { return 0 }
        lassign [screen_recorded_size] rw rh
        if { $rw ne "" } { set w $rw; set h $rh }
        dservSet ess/screen_halfx $hx
        dservSet ess/screen_halfy $hy
        dservSet ess/screen_w $w
        dservSet ess/screen_h $h
        dservSet ess/screen_refresh_rate $hz
        dservSet ess/screen_frame_duration $fd
        dservSet ess/screen_bg $bg
        return 1
    }
    # the window size configure_stim recorded for the loaded state system
    # (its screen_w and screen_h, what touch_init reads), as {w h}; {} when
    # no system has one
    proc screen_recorded_size {} {
        variable current
        if { ![info exists current(state_system)] } { return {} }
        set s $current(state_system)
        if { $s eq "" || ![info exists ${s}::screen_w] || ![info exists ${s}::screen_h] } { return {} }
        set w [set ${s}::screen_w]; set h [set ${s}::screen_h]
        if { ![string is integer -strict $w] || ![string is integer -strict $h] || $w <= 0 || $h <= 0 } { return {} }
        list $w $h
    }
    # END screen_snapshot

    # BEGIN stim_script
    #
    # The stim script that was running, published as stim/script
    # (DSERV_JSON, type 11) on every configure_stim and re-emitted into
    # every datafile, so a .ess carries the text a replay needs.
    #
    # The stim/cmd record of the script load itself (stim_cmd_log block)
    # stores only the sha256 of the text, since the load is far over the
    # 4096-char limit; stim/script carries the text, and the same hash, so
    # a reader joins the two by hash. The record (field by field):
    #   path      string; the resolved stimfile path configure_stim read
    #             ("" when nothing was looked for: stim2 not connected)
    #   hash      sha256 hex of the UTF-8 text EXACTLY as rmtSend sent it
    #             (dserv's `sha256` on the Tcl string, the same call
    #             stim_cmd_json makes, so the two hashes are byte-identical);
    #             "" when body is "" and -- see note -- where no in-process
    #             sha256 exists (a harness without dserv's command), in which
    #             case the record also carries "hash_note"
    #   len       integer; UTF-8 byte length of the text
    #   body      string; the full text, JSON-escaped (json_escape, so the
    #             record is pure ASCII as dservSetData requires); "" when no
    #             script was sent
    #   system, protocol, variant   the names of the load that sent it
    #   stim2     string; stim2's StimVersion (a Tcl-linked int every stim2
    #             has, "20" since 2.0: the cheapest version a build of any age
    #             answers) -- the reply to the raw `set StimVersion`, "" when
    #             it errored or nothing answered
    #   stimess   string; the stimess package version stim2's binding table
    #             reported ("1.0" from "package":"stimess 1.0" in stim/bind),
    #             "" when the table names no package (an old stim2, a script
    #             that did not require it, not connected)
    #   dlsh      string; the dlsh package version stim2 has loaded -- the
    #             reply to the raw `package present dlsh` ("0.20.3"), "" when
    #             it errored (no dlsh in that interp) or nothing answered.
    #             The pure-Tcl packages the stim script requires from
    #             dlsh.zip (mp_sim, traj, colorcal, ...) are deliberately
    #             NOT recorded: a replay mounts the dlsh.zip of this version
    #             and gets them all. (Paradigm libraries from the systems
    #             tree ARE recorded, as stim/lib/<name>: the stim_lib block.)
    #
    #   configure_stim          stim_script_version, then stim_script_dlsh,
    #                           right after the ACK header probe (raw
    #                           commands, so like the probe they are not
    #                           stim/cmd records). Then, like
    #                           stim/bind, the EMPTY record (path "", body
    #                           "") is published on the not-connected branch
    #                           and again before the script goes out, so a
    #                           load that fails below never leaves the
    #                           previous load's script up; the full record
    #                           is published once stim_bind_publish has
    #                           asked for the table (its "package" is where
    #                           stimess comes from), with the timestamp
    #                           taken just before the send -- the moment the
    #                           script took effect on stim2.
    #   stim/script             the record. Re-emitted into each datafile at
    #                           open (dservTouch, like stimdg and stim/bind).
    #
    # Logging policy (open_datafile): unbuffered, not obs-limited, every
    # record, next to the stim/cmd match: `dservLoggerAddMatch $filename
    # stim/script 0 0 1`, and `dservTouch stim/script` after the logger is
    # running. One record per file in the normal case.
    #
    # Self-contained like the blocks above: tests/test_ess_stim_cmd.tcl
    # evaluates the text between these markers in `namespace eval ess`.
    #
    variable stim2_version {}

    # Ask stim2 which version it is (raw, not a stim/cmd record); remembers
    # and returns it, "" on an error reply or no reply.
    proc stim_script_version {} {
        variable stim2_version
        # stimVersion (stim2 >= 0.27.3) answers the release the binary was
        # built from, e.g. "0.27.3". Older builds only have the Tcl-linked
        # StimVersion global, the wire-level format number (20); keep that
        # as the fallback so the field is never empty on a stim2 that has
        # a command port at all.
        set r ""
        catch { set r [::ess::rmtSend_raw stimVersion] }
        set r [string trim $r]
        if { $r eq "" || [string match "!TCL_ERROR*" $r] } {
            set r ""
            catch { set r [::ess::rmtSend_raw {set StimVersion}] }
            set r [string trim $r]
            if { [string match "!TCL_ERROR*" $r] } { set r "" }
        }
        set stim2_version $r
        return $stim2_version
    }

    # Ask stim2 which dlsh it has loaded (raw, not a stim/cmd record): the
    # reply to `package present dlsh`; remembers and returns it, "" on an
    # error reply (dlsh not present in that interp) or no reply.
    variable stim2_dlsh {}
    proc stim_script_dlsh {} {
        variable stim2_dlsh
        set r ""
        catch { set r [::ess::rmtSend_raw {package present dlsh}] }
        set r [string trim $r]
        if { [string match "!TCL_ERROR*" $r] } { set r "" }
        set stim2_dlsh $r
        return $stim2_dlsh
    }

    # Does this stim2 carry the configure prelude? stimess 1.1 added
    # `stim::prelude live|playback`: the dserv plumbing configure_stim used
    # to send inline (register, stimdg match, return socket, dserv_send*,
    # mouse bridge, puts -> stim/stdout), as one recorded line, with a
    # playback form a replay installs instead. Returns the stimess version
    # when it is 1.1 or newer, "" for an older stim2 or none; configure_stim
    # then sends the inline block. Raw, like the other probes: the prelude
    # line itself goes through rmtSend and is in stim/cmd.
    proc stim_prelude_version {} {
        set r ""
        catch { set r [::ess::rmtSend_raw {package require stimess}] }
        if { $r eq "" || [string match "!TCL_ERROR*" $r] } { return "" }
        if { [catch { package vcompare $r 1.1 } c] || $c < 0 } { return "" }
        return $r
    }

    # The stimess version a stim/bind table reports: "1.0" from
    # "package":"stimess 1.0", "" otherwise. Textual on purpose (the table
    # is stim2's pure-ASCII JSON; see the stim_bind block on parsers).
    proc stim_script_stimess { table } {
        if { [regexp {"package"\s*:\s*"stimess\s+((?:[^"\\]|\\.)*)"} $table -> v] } {
            return $v
        }
        return ""
    }

    # The stim/script record text.
    proc stim_script_json { path body system protocol variant stim2 stimess dlsh } {
        set bytes [encoding convertto utf-8 $body]
        set json "\{\"path\":\"[json_escape $path]\""
        if { $body eq "" } {
            append json ",\"hash\":\"\""
        } elseif { [info commands ::sha256] ne "" } {
            append json ",\"hash\":\"[::sha256 $body]\""
        } else {
            append json ",\"hash\":\"\",\"hash_note\":\"no sha256 command in this interp\""
        }
        append json ",\"len\":[string length $bytes]"
        append json ",\"body\":\"[json_escape $body]\""
        append json ",\"system\":\"[json_escape $system]\""
        append json ",\"protocol\":\"[json_escape $protocol]\""
        append json ",\"variant\":\"[json_escape $variant]\""
        append json ",\"stim2\":\"[json_escape $stim2]\""
        append json ",\"stimess\":\"[json_escape $stimess]\""
        append json ",\"dlsh\":\"[json_escape $dlsh]\"\}"
        return $json
    }

    # Publish stim/script at timestamp t: the script text sent (body, read
    # from path) under the named load. stim2 and dlsh are the remembered
    # versions, stimess comes from the stim/bind table as published. Never
    # throws; returns the record text.
    proc stim_script_publish { path body t system protocol variant } {
        variable stim2_version
        variable stim2_dlsh
        set stimess ""
        catch { set stimess [stim_script_stimess [dservGet stim/bind]] }
        set json [stim_script_json $path $body $system $protocol $variant \
                      $stim2_version $stimess $stim2_dlsh]
        if { [catch { dservSetData stim/script $t 11 $json } err] } {
            catch { ess_warning "stim/script not written: $err" "stim" }
        }
        return $json
    }
    # END stim_script

    # BEGIN stim_lib
    #
    # The paradigm libraries configure_stim -libs shipped to stim2, each
    # published as stim/lib/<name> (DSERV_JSON, type 11) at the moment it
    # is sent and re-emitted into every datafile, so a .ess carries what a
    # replay needs to write the text to its module path as
    # <name>-<version>.tm and let the stim script's `package require <name>`
    # resolve. Only libraries from <systems>/<project>/lib are recorded: a
    # pure-Tcl package from dlsh.zip (mp_sim, traj, colorcal, ...) is NOT --
    # a replay mounts the dlsh.zip of the version stim/script's "dlsh"
    # field names, and gets them all.
    #
    # As with the stim script, the stim/cmd record of the ship itself holds
    # only the sha256 when the text is over the 4096-char limit; the same
    # hash is here, so a reader joins the two. The record (field by field):
    #   name      string; the library name as given to -libs ("runner")
    #   version   string; parsed from the resolved file name
    #             <name>-<version>.tm ("1.0" from runner-1.0.tm), "" when
    #             the file name does not have that shape
    #   path      string; the resolved file stim_lib_script read
    #   hash      sha256 hex of the UTF-8 text EXACTLY as rmtSend sent it
    #             (dserv's `sha256` on the Tcl string, the same call
    #             stim_cmd_json makes); "" with a "hash_note" field where no
    #             in-process sha256 exists (a harness without dserv's
    #             command), never on a real dserv
    #   len       integer; UTF-8 byte length of the text
    #   body      string; the full text, JSON-escaped (json_escape, so the
    #             record is pure ASCII as dservSetData requires)
    #
    #   configure_stim          clears stim_lib_names first thing (every
    #                           configure starts from no libraries), then in
    #                           the -libs loop calls stim_lib_publish for
    #                           each library right before the rmtSend that
    #                           ships it (after the `package forget`), with
    #                           the timestamp of the send
    #   stim/lib/<name>         one datapoint per library, so several
    #                           coexist and each is re-emitted into each
    #                           datafile at open (dservTouch, like
    #                           stim/script)
    #   stim_lib_names          the names published by the current
    #                           configure, for open_datafile's touches
    #
    # Logging policy (open_datafile): unbuffered, not obs-limited, every
    # record, under ONE wildcard match next to the stim/script match --
    # `dservLoggerAddMatch $filename stim/lib/* 0 0 1` (the logger's
    # matches are globs, LogMatchDict MATCH_KRAUSS, like dservAddMatch) --
    # and `dservTouch stim/lib/<name>` for each name in stim_lib_names after
    # the logger is running, guarded by dservExists.
    #
    # Self-contained like the blocks above: tests/test_ess_stim_cmd.tcl
    # evaluates the text between these markers in `namespace eval ess`.
    #
    variable stim_lib_names {}

    # The version a library file name carries: <name>-<version>.tm gives
    # <version> ("1.0" for runner-1.0.tm, "1.0.2" for runner-1.0.2.tm); ""
    # when the tail does not start with "<name>-".
    proc stim_lib_version { name path } {
        set root [file rootname [file tail $path]]
        set prefix "${name}-"
        if { [string range $root 0 [expr {[string length $prefix] - 1}]] ne $prefix } {
            return ""
        }
        return [string range $root [string length $prefix] end]
    }

    # The stim/lib/<name> record text.
    proc stim_lib_json { name version path body } {
        set bytes [encoding convertto utf-8 $body]
        set json "\{\"name\":\"[json_escape $name]\""
        append json ",\"version\":\"[json_escape $version]\""
        append json ",\"path\":\"[json_escape $path]\""
        if { $body eq "" } {
            append json ",\"hash\":\"\""
        } elseif { [info commands ::sha256] ne "" } {
            append json ",\"hash\":\"[::sha256 $body]\""
        } else {
            append json ",\"hash\":\"\",\"hash_note\":\"no sha256 command in this interp\""
        }
        append json ",\"len\":[string length $bytes]"
        append json ",\"body\":\"[json_escape $body]\"\}"
        return $json
    }

    # Publish stim/lib/<name>: the library text about to be sent (body,
    # read from path, whose file name gives the version), at timestamp t
    # ([now] when omitted). Remembers the name in stim_lib_names. Never
    # throws; returns the record text.
    proc stim_lib_publish { name path body {t {}} } {
        variable stim_lib_names
        if { $t eq "" } { set t [now] }
        set json [stim_lib_json $name [stim_lib_version $name $path] $path $body]
        if { [catch { dservSetData stim/lib/$name $t 11 $json } err] } {
            catch { ess_warning "stim/lib/$name not written: $err" "stim" }
        }
        if { $name ni $stim_lib_names } { lappend stim_lib_names $name }
        return $json
    }
    # END stim_lib

    # BEGIN stim_late_draw
    #
    # stim2's draw-late scheduler (stim2 >= 0.28, `stimLateDraw ms`,
    # stim2 docs/late-draw.md): with a dynamic group it defers the step and
    # draw of each frame to ms before the vblank and stays awake until then,
    # answering commands and taking datapoints, so a frame shows what
    # arrived during it rather than what had arrived a frame earlier. ms is
    # the draw budget; at 120 Hz a budget of 3 leaves about 5 ms of
    # listening per frame. 0 = stim2's default, draw right after the flip.
    #
    # A protocol declares `add_param late_draw_ms 3.0 time float` and calls
    # `::ess::stim_late_draw $late_draw_ms` from its init callback, after
    # configure_stim, and `::ess::stim_late_draw 0` from deinit so stim2 is
    # left as found. Sent through the rmtSend wrapper on purpose, so the
    # setting lands in the data file as a stim/cmd record (the param itself
    # is logged at file open with every other param).
    #
    # Never fatal: an older stim2 answers "invalid command name", one
    # without hardware flip timestamps (a Mac's glfinish strategy) refuses,
    # and either way the task runs exactly as before, a frame later; it
    # only says so. stim2 also refuses until it has confirmed one flip, so
    # on that refusal a `!redraw` is sent once and the request retried.
    # Returns 1 when stim2 took the setting, 0 otherwise.
    proc stim_late_draw { ms {who stim} } {
        if { [catch { rmtSend "stimLateDraw $ms" } r] } { set r "!TCL_ERROR $r" }
        if { [string match "!TCL_ERROR*" $r] && $ms != 0 &&
             [string match -nocase "*flip*" $r] } {
            # no confirmed flip yet: give it one and ask again
            catch { rmtSend "!redraw" }
            if { [catch { rmtSend "stimLateDraw $ms" } r] } { set r "!TCL_ERROR $r" }
        }
        if { [string match "!TCL_ERROR*" $r] } {
            if { $ms != 0 } {
                ::ess::ess_warning "late_draw_ms $ms not applied, running\
                    without draw-late: stim2 said '[string range $r 11 end]'" \
                    $who
            }
            return 0
        }
        # the status dict stim2 answers with; enabled 1 means it took
        if { $ms != 0 && [dict exists $r enabled] && ![dict get $r enabled] } {
            ::ess::ess_warning "late_draw_ms $ms: stim2 reports draw-late\
                still disabled ($r)" $who
            return 0
        }
        return 1
    }
    # END stim_late_draw
}

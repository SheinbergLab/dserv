# extio_sync.tcl -- host-side software clock anchoring for extio boxes.
#
# WHAT THIS FIXES. Without a TTL sync line the box anchors its clock on the
# ARRIVAL of ess/in_obs, i.e. it assumes delivery is instantaneous. Measured
# against a hardware anchor on the same box, that costs exactly the one-way
# delivery: -202 us on Ethernet, ~-730 us on USB-FS (PORTING.md 2026-07-26).
# Worse, a sw anchor never teaches the rate (box_clock_sync: `if (!trusted)
# return;`), so ~25 ppm of crystal drift accumulates between anchors -- which is
# how a box reached 64 ms off after 31 idle minutes.
#
# THE APPROACH. Two things, both per-box:
#   1. estimate d, the one-way delivery, from a benign round trip
#   2. hand d to the box on a timer, so it can correct its own anchor
#
# WHY A SEPARATE DATAPOINT. ess/in_obs is doing two jobs whose requirements have
# diverged: it is the clock anchor (wants to fire often, tolerates adjustment)
# AND the obs timeline anchor (g_obs_begin_us, which scheduled pulses land
# against -- must stay tied to the true obs edge, must never be adjusted). It
# also drives the obs mirror LED and gates the %match refresh. So the clock
# anchor moves to its own key and ess/in_obs is left pristine.
#
# WHY cmd/sync AND NOT A NEW %match. The box already registers `%match
# <pfx>/cmd/* 1` (wizchip_dserv_config.c reg_build), so a new cmd key is
# forwarded with NO registration change -- and it is per-box by construction,
# which matters because d differs per box AND per transport.
#
# WHY d IS A VALUE, NOT A DOCTORED TIMESTAMP. Forward-dating the frame also
# works (dservSetData honours an explicit ts) and needs no firmware at all, but
# it writes a knowingly-wrong time into a shared datapoint, and it silently
# absorbs the skew between evaluating [now] and the frame actually leaving dserv.
# Carrying d as the value keeps the timestamp honest, puts d in the datafile
# where it can be audited, and lets the box apply it only on the sw path.
#
# FIRMWARE CONTRACT (see PORTING.md):
#   cmd/probe <seq>  -> box publishes state/probe = its OWN turnaround in us
#                      (t_send - t_recv, both box clock, so the box's unknown
#                      offset cancels). Lets the host remove box time from the
#                      RTT instead of assuming it away.
#   cmd/sync <d_us> -> box anchors with offset = (frame_ts + d_us) - receipt_box.
#                      Ignored when the box has a fresh latched TTL edge.
#
# LOAD (hot-patch, no restart):
#   dservctl extio "source /usr/local/dserv/config/extio_sync.tcl"

set ::extio_sync_enable 1
set ::extio_sync_win    30          ;# rolling RTT window (samples); 30 * 2s = 60s
set ::extio_sync_max_us 100000      ;# implausible RTT -> drop the sample
set ::extio_sync_hw_fresh_us 30000000   ;# a hw anchor older than 30 s is not "live"

array set ::extio_sync_rtt {}       ;# box -> list of recent NET rtts (us)
array set ::extio_sync_d   {}       ;# box -> estimated one-way delay (us)
array set ::extio_sync_t1  {}       ;# box -> outstanding probe send time
array set ::extio_sync_seq {}

proc extio_sync_boxes {} {
    if { ![info exists ::extio_known] } { return {} }
    return [array names ::extio_known]
}

# state/probe carries the box's own turnaround, so subtracting it leaves only
# wire+stack time. min-filtering over the window then picks the least-queued
# sample, where the symmetry assumption (d = net/2) is least wrong.
#
# T1/T4 COME FROM THE DATAPOINTS, NOT FROM [now]. Bracketing with [now] in Tcl
# put host PROCESSING into the round trip asymmetrically: the return leg picked
# up dserv's ~116 us dispatch-to-Tcl-callback latency (host/dispatch_test.sh)
# while the outbound leg did not -- and halving a round trip whose legs are not
# comparable is exactly what left 172 us on the table against the hw anchor.
# dservTimestamp(cmd/probe) is dserv's C-side stamp as it published, and
# state/probe is arrival-stamped by dserv (the box sends timestamp 0), so this
# pair brackets transit and leaves host processing out of both ends.
proc extio_sync_on_probe { dp data } {
    set box [lindex [split $dp /] 1]
    if { ![info exists ::extio_sync_t1($box)] } { return }
    unset ::extio_sync_t1($box)          ;# one outstanding probe per box

    if { ![string is integer -strict $data] } { return }
    if { [catch { set t1 [dservTimestamp extio/$box/cmd/probe] }] } { return }
    if { [catch { set t4 [dservTimestamp $dp] }] } { return }

    set net [expr { ($t4 - $t1) - $data }]
    if { $net <= 0 || $net > $::extio_sync_max_us } { return }

    lappend ::extio_sync_rtt($box) $net
    set n [llength $::extio_sync_rtt($box)]
    if { $n > $::extio_sync_win } {
        set ::extio_sync_rtt($box) \
            [lrange $::extio_sync_rtt($box) [expr {$n - $::extio_sync_win}] end]
    }
    set ::extio_sync_d($box) \
        [expr { [::tcl::mathfunc::min {*}$::extio_sync_rtt($box)] / 2 }]
}

# No [now] here: T1 is read back from cmd/probe's own timestamp in the reply
# handler. The flag only marks "one probe outstanding" so a duplicate or stale
# state/probe cannot inject a bogus sample.
proc extio_sync_probe { box } {
    set ::extio_sync_t1($box) 1
    dservSet extio/$box/cmd/probe [incr ::extio_sync_seq($box)]
}

# Does this box have a TTL sync line CONFIGURED? state/sync_pin is announced in
# the manifest and is a stable property of the box, which is what we actually
# want to branch on.
#
# Two wrong answers were tried first, both instructive:
#   state/sync/source == "hw"  -- STICKY in dserv's table. It survives box
#     reboots and means "this box synced at some point", not "now", so a
#     31-minute-old "hw" suppressed correction forever.
#   ...plus a freshness window -- fails the other way. hw anchors arrive at obs
#     boundaries, which are minutes apart between blocks, so ANY window
#     eventually lets the host stomp a good hw anchor with a worse sw one. And
#     alternating two methods that disagree by ~172 us is worse than committing
#     to either.
# A box with a wired sync line should simply never be sw-corrected.
proc extio_sync_has_hw { box } {
    if { [catch { set p [dservGet extio/$box/state/sync_pin] }] } { return 0 }
    if { ![string is integer -strict $p] } { return 0 }
    return [expr { $p >= 0 }]
}

# One pass per extioTimer/0 tick (2 s). Anchoring that often keeps inter-anchor
# drift near 50 us at 25 ppm, which is why rate learning is optional here.
proc extio_sync_tick {} {
    if { !$::extio_sync_enable } { return }

    # NEVER during an obs. Re-anchoring STEPS the box clock, and a step inside a
    # data-collection window puts two events in one trial on different mappings
    # -- the failure dserv's monotonic-timebase commit exists to prevent on the
    # host. obs boundaries already anchor and are the only provably harmless
    # moment for a discontinuity, so this ticker's job is the IDLE GAP between
    # obs (where drift actually rots), not the experiment. The box enforces this
    # too (it owns g_in_obs, closing the race where obs starts in flight); this
    # check just avoids generating the frames at all.
    if { ![catch { set io [dservGet ess/in_obs] }] && $io } { return }

    foreach box [extio_sync_boxes] {
        # Probe UNCONDITIONALLY, even on a box with a live TTL line: the estimate
        # costs one frame each way per tick and keeps d ready, so losing the sync
        # line degrades to sw correction immediately instead of after a warm-up.
        extio_sync_probe $box

        # But NEVER correct a box that currently has a hardware anchor --
        # forward-correcting a latched-edge anchor injects the very error this
        # removes.
        if { [extio_sync_has_hw $box] } { continue }
        if { [info exists ::extio_sync_d($box)] } {
            dservSet extio/$box/cmd/sync $::extio_sync_d($box)
        }
    }
}

proc extio_sync_status {} {
    set out {}
    foreach box [lsort [extio_sync_boxes]] {
        set d "-"
        set n 0
        if { [info exists ::extio_sync_d($box)] }   { set d $::extio_sync_d($box) }
        if { [info exists ::extio_sync_rtt($box)] } { set n [llength $::extio_sync_rtt($box)] }
        set src ""
        catch { set src [dservGet extio/$box/state/sync/source] }
        append out [format "%-12s d=%-6s n=%-3d source=%s\n" $box $d $n $src]
    }
    return $out
}

proc extio_sync_wire {} {
    dservAddMatch extio/*/state/probe
    dpointSetScript extio/*/state/probe extio_sync_on_probe
}

# Hot-patch the existing 2 s tick rather than adding a timer: extio_timer_cb is
# the only consumer of extioTimer/0, so wrapping it keeps one scheduler.
proc extio_timer_cb { dpoint data } {
    extio_service
    catch { extio_sync_tick }
}

extio_sync_wire
puts "extio_sync: wired (tick=2s via extioTimer/0, window=$::extio_sync_win)"

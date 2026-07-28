# ptpconf.tcl -- put every extio box onto dserv's timeline, automatically.
#
# Started from dsconf.tcl:
#     subprocess ptp "source [file join $dspath config/ptpconf.tcl]"
#
# WHAT THIS IS FOR
#
# PTP gives a box good time; it does not give it SHARED time. Two separate legs:
#
#   1. the PROTOCOL leg -- ptp4l (see systemd/dserv-ptp4l@.service) disciplines
#      each box's ENET 1588 counter to this host's PHC. Automatic, ~+/-100 ns,
#      needs nothing from us.
#   2. the ANCHOR -- dserv stamps from CLOCK_MONOTONIC plus a fixed epoch
#      constant, a DIFFERENT oscillator from the PHC. So a box disciplined to the
#      PHC still has no idea what dserv time is.
#
# This subprocess owns leg 2. It measures the constant
#
#     D = dserv_us - ptp_us = dservClockEpochOffset - (phc_us - mono_us)
#
# from two LOCAL clock reads -- no wire, no one-way delay, no asymmetry to model,
# which is exactly why this beats the old obs-anchor path -- and pushes it to
# each box as cmd/ptp/offset. Once a box has D it re-anchors itself from PTP
# every second at zero packet cost. The result is that timestamps from different
# boxes, and from dserv itself, are directly comparable.
#
# WHY A SUBPROCESS AND NOT A SHELL SCRIPT ON A TIMER
#
# The work is inherently dserv-coupled: it needs dservClockEpochOffset, it has to
# know which boxes exist, and it writes a datapoint per box. host/ptp_anchor.sh
# does all of that from outside and has to be re-run BY HAND -- per box, per
# session, and after every box reboot, because AN ANCHOR NEVER SURVIVES A BOX
# REBOOT (or a reflash). That was the single most repeated manual step of
# 2026-07-27.
#
# THE REACTIVE TRIGGER
#
# A box now publishes state/sync/source=none when it connects unanchored (added
# 2026-07-27; before that a rebooted box left the RETAINED "ptp" standing and
# read as synced while refusing every at_abs). That announcement is what makes
# this subprocess possible: we anchor on the event rather than polling. The
# periodic sweep below is a backstop for anything that announcement misses.
#
# Overridable in local/ptp.tcl -- set any of the ::ptp_* vars before this runs.

proc exit {args} { error "exit not available for this subprocess" }
errormon enable

# ---- configuration ---------------------------------------------------------
# Interface whose PHC is the grandmaster. Only used to resolve the PHC when
# there is more than one; see ptp_resolve_phc.
if { ![info exists ::ptp_iface] }  { set ::ptp_iface eth0 }
# Empty = auto-resolve. Do NOT hardcode /dev/ptp0: the index is assigned at
# driver probe and is NOT stable across reboots (this rig moved ptp0 -> ptp1
# after a Pi restart, and a hardcoded path then measured the wrong clock).
if { ![info exists ::ptp_phc] }    { set ::ptp_phc "" }
# Reject a measurement whose own uncertainty is too large rather than anchoring
# on it: dservPhcOffset reports `window`, and the error is bounded by half of it.
# A bad D looks authoritative once it is on every box, so refusing is better than
# publishing. Method B windows are tens of ns; a min-filtered method A sandwich
# is ~1.4-3.2 us on the hosts measured. 20 us is generous for both and still
# rejects a pathological read.
if { ![info exists ::ptp_window_max_ns] } { set ::ptp_window_max_ns 20000 }

# Re-anchor cadence. D moves only as fast as the PHC<->system-clock drift, which
# phc2sys holds at ~0.002 ppm => ~400 s to accumulate 1 us on the rig Pi, and
# ~3200 s on a host with hardware cross-timestamping. 300 s is conservative for
# the worst case measured and costs one exec per interval.
if { ![info exists ::ptp_period_s] } { set ::ptp_period_s 300 }

# A step in D is the ONE thing that invalidates every box's anchor at once, and
# it is otherwise invisible. dserv's epoch constant is system_us() - steady_us()
# captured once at startup, and its own comment notes those two clocks are slewed
# together by NTP and diverge ONLY ACROSS A STEP. So the change in D between
# sweeps IS a step detector, for free: normal drift between sweeps is a couple of
# microseconds, while an NTP step shows up as milliseconds.
#
# Worth having because the failure is silent: CLOCK_MONOTONIC never steps, so
# after an NTP step every box is anchored to a timeline that quietly moved, and
# nothing else would say so until someone compared timestamps across it.
if { ![info exists ::ptp_step_warn_us] } { set ::ptp_step_warn_us 1000 }

set ::ptp_timer   ""
set ::ptp_d_us    ""
set ::ptp_lasterr ""

proc ptp_pub {leaf val} { catch { dservSet ptp/$leaf $val } }

# ---- host side -------------------------------------------------------------

# Resolve the PHC from sysfs, which is readable WITHOUT root -- `ethtool -T` is
# not, and a subprocess must never depend on a sudo timestamp that expires.
# With exactly one PHC this is unambiguous; with several, fall back to ethtool.
proc ptp_resolve_phc {} {
    if { $::ptp_phc ne "" } { return $::ptp_phc }

    set l [lsort [glob -nocomplain /sys/class/ptp/ptp*]]
    if { [llength $l] == 1 } {
        return /dev/[file tail [lindex $l 0]]
    }
    if { ![catch { exec ethtool -T $::ptp_iface } out] } {
        foreach line [split $out "\n"] {
            if { [regexp {(?:provider index|PTP Hardware Clock):\s*(\d+)} $line -> idx] } {
                return /dev/ptp$idx
            }
        }
    }
    error "cannot resolve a PHC for $::ptp_iface ([llength $l] ptp devices present)"
}

# D in microseconds, or an error.
#
# dservPhcOffset is BUILT INTO dserv (src/TclServer.cpp), beside
# dservClockEpochOffset -- the two halves of the same sum. This used to exec
# extio-zephyr/host/phc_offset, which meant a binary outside the dserv install to
# locate and keep in sync (it is not under $dspath on a deployed host, and that
# broke on the first real sweep), a ~2 s fork+exec where the ioctl costs
# microseconds, and stdout as the contract. The standalone tool remains for bench
# work -- it does the two-method cross-check, drift fit and residuals this does
# not need.
proc ptp_measure_d {} {
    set phc [ptp_resolve_phc]
    set r   [dservPhcOffset $phc]          ;# dict: ns window method phc

    set ns     [dict get $r ns]
    set window [dict get $r window]
    set method [dict get $r method]

    if { $window > $::ptp_window_max_ns } {
        error "PHC read window ${window} ns > ${::ptp_window_max_ns} (method\
               $method) -- refusing to anchor on it"
    }

    ptp_pub phc       $phc
    ptp_pub method    $method
    ptp_pub window_ns $window

    # PHC - MONOTONIC is hugely positive (PHC counts from the epoch, MONOTONIC
    # from boot), so integer division needs no rounding care here.
    return [expr {[dservClockEpochOffset] - ($ns / 1000)}]
}

# ---- box side --------------------------------------------------------------

# Every live box publishes state/watchdog once a second, so it is the cheapest
# roster. Retained keys mean a long-gone box can still appear; anchoring one is
# harmless (the datapoint simply goes nowhere).
# A box is one whose watchdog is FRESH -- not one whose watchdog datapoint
# EXISTS. dserv retains datapoints forever, so a box that is unplugged leaves its
# whole state/* tree standing and keeps being counted.
#
# Found by an 8-hour soak on 2026-07-28: box3 had been powered off all night and
# still counted in BOTH ptp/boxes and ptp/anchored -- its retained
# state/sync/source still read "ptp". That is the exact bug this subprocess was
# written to close, reproduced inside it.
#
# It is not cosmetic. rig_check's anchoring test is `anchored >= boxes-under-
# test`, so an inflated count MASKS a real failure: had box1 silently lost its
# anchor, the count would have gone 3 -> 2, still passed for two boxes, and the
# check designed to catch exactly that would have reported PASS. The inflation is
# the same size as the fault it is meant to detect.
#
# Freshness, not the value: state/watchdog counts seconds since boot, so a dead
# box's value is perfectly plausible -- only its TIMESTAMP gives it away.
set ::ptp_live_us 10000000        ;# watchdog is 1 Hz; 10 s is generous and still
                                  ;# two orders off an overnight-dead box

proc ptp_boxes {} {
    set out {}
    set now [now]
    foreach k [dservKeys extio/*/state/watchdog] {
        if { ![regexp {^extio/([^/]+)/state/watchdog$} $k -> b] } continue
        set age -1
        catch { set age [expr {$now - [dservTimestamp $k]}] }
        if { $age >= 0 && $age < $::ptp_live_us } { lappend out $b }
    }
    return [lsort -unique $out]
}

# Report failures, never swallow them. An anchor that silently fails to land
# leaves a box refusing every at_abs while everything else looks healthy -- the
# same shape as the retained sync/source bug this subprocess exists to close.
# A bare `catch` here would have hidden exactly that (caught by the stub harness
# in the very first run, 2026-07-27). Returns 1 on success.
proc ptp_anchor_box {box d} {
    if { [catch { dservSet extio/$box/cmd/ptp/offset $d } err] } {
        puts "ptp: FAILED to anchor $box: $err"
        ptp_pub error "anchor $box: $err"
        return 0
    }
    return 1
}

# Measure D ONCE and fan it out. Deliberately not per-box: the measurement is a
# ~2 s blocking exec, and D is a property of this host, not of any box.
proc ptp_anchor_all {{why sweep}} {
    if { [catch { ptp_measure_d } d] } {
        if { $d ne $::ptp_lasterr } {          ;# log a NEW fault once, not every sweep
            puts "ptp: $d"
            set ::ptp_lasterr $d
        }
        ptp_pub error $d
        return
    }
    set ::ptp_lasterr ""

    if { $::ptp_d_us ne "" } {
        set delta [expr {$d - $::ptp_d_us}]
        ptp_pub d_delta_us $delta
        if { abs($delta) >= $::ptp_step_warn_us } {
            puts "ptp: WARNING D jumped ${delta} us -- looks like a clock STEP,\
                  not drift; every box was anchored to the old timeline"
            ptp_pub step_us $delta
        }
    }

    set ::ptp_d_us $d
    ptp_pub error ""
    ptp_pub d_us  $d

    set n 0
    set bad 0
    foreach b [ptp_boxes] {
        if { [ptp_anchor_box $b $d] } { incr n } else { incr bad }
    }
    foreach b [ptp_boxes] { dservAfter 3000 [list ptp_verify $b 8] }

    ptp_pub boxes  $n
    ptp_pub failed $bad
    ptp_pub last   [dservClockEpochOffset]
    ptp_pub reason $why
    puts "ptp: anchored $n box(es)[expr {$bad ? ", $bad FAILED" : ""}], D=$d us ($why)"
}

# ---- triggers --------------------------------------------------------------

# Reactive: a box announces sync/source=none when it connects without an anchor.
# Anchor just that box, reusing the last known D when we have one -- a box that
# has only just rebooted should not wait up to a full sweep period, and D is a
# host property that has not changed in the meantime.
# Push-and-VERIFY. A box announces sync/source=none the moment it connects, which
# is BEFORE ITS DOWNLINK EXISTS. Traced on the rig 2026-07-28 by watching
# cmds_rx across a reboot:
#
#   t=15s  sync/source=none   cmds_rx=0   <-- announces, but %match not yet up
#   t=18s  sync/source=sw     cmds_rx=2       downlink live
#   t=21s  sync/source=ptp    cmds_rx=3       the RETRY landed
#
# cmds_rx=0 at the moment of the announcement is the whole story: the immediate
# push had nowhere to go. That is why the first attempt vanished while an
# identical manual push minutes later took instantly -- it was never about the
# value, only about whether the box could hear yet.
#
# The anchor is idempotent and the box is the authority on whether it took, so
# retry until it reports ptp. Give up loudly rather than quietly: an anchor that
# silently fails to land leaves a box refusing every at_abs while everything
# around it looks healthy.
# How many boxes are ACTUALLY reporting anchored, which is a different question
# from how many we pushed to (ptp/boxes). Those two differing is precisely the
# failure seen on 2026-07-28 -- pushes landing nowhere while everything else read
# healthy -- and until now only the retry loop knew about it. Nothing outside
# could tell whether the anchoring SERVICE was working, only whether a given box
# happened to look right. No side effects: this reads state we already read.
proc ptp_publish_anchored {} {
    set n 0
    foreach b [ptp_boxes] {
        set src ""
        catch { set src [dservGet extio/$b/state/sync/source] }
        if { $src eq "ptp" } { incr n }
    }
    ptp_pub anchored $n
    return $n
}

proc ptp_verify {box tries} {
    set src ""
    catch { set src [dservGet extio/$box/state/sync/source] }
    ptp_publish_anchored
    if { $src eq "ptp" } return                     ;# took

    if { $tries <= 0 } {
        puts "ptp: $box never took the anchor (last sync/source='$src')"
        ptp_pub error "$box did not take the anchor"
        return
    }
    if { $::ptp_d_us ne "" } { ptp_anchor_box $box $::ptp_d_us }
    dservAfter 3000 [list ptp_verify $box [expr {$tries - 1}]]
}

proc ptp_on_sync_source {dp data} {
    if { ![regexp {^extio/([^/]+)/state/sync/source$} $dp -> box] } return
    if { $data ne "none" } return                 ;# "ptp"/"hw"/"sw" = already anchored

    if { $::ptp_d_us eq "" } {
        ptp_anchor_all "box $box appeared"        ;# no D yet: measure now
    } else {
        if { [ptp_anchor_box $box $::ptp_d_us] } {
            puts "ptp: $box announced unanchored -> pushed D=$::ptp_d_us us"
            dservAfter 3000 [list ptp_verify $box 8]
        }
    }
}

# Periodic: self-rescheduling, because dservAfter is ONE-SHOT and plain `after`
# is SILENTLY INERT in dserv (there is no Tcl event loop in these interps --
# it would never fire and nothing would say so).
proc ptp_tick {} {
    ptp_anchor_all sweep
    set ::ptp_timer [dservAfter [expr {$::ptp_period_s * 1000}] ptp_tick]
}

dservAddMatch   extio/*/state/sync/source
dpointSetScript extio/*/state/sync/source ptp_on_sync_source

ptp_pub iface    $::ptp_iface
ptp_pub period_s $::ptp_period_s

# First sweep shortly after startup rather than immediately: dserv is still
# bringing subprocesses up, and boxes may not have registered yet.
set ::ptp_timer [dservAfter 5000 ptp_tick]

puts "ptp: anchoring extio boxes on $::ptp_iface every ${::ptp_period_s}s (+ on connect)"

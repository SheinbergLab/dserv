# -*- mode: tcl -*-
#
# ess_sling-1.0.tm
#
# The "sling" response mode: the subject DRAWS a slingshot with a continuous
# input, watches the draw (and, in training, the flight it would produce),
# and commits by LETTING GO. The report is a 2-D VECTOR -- direction and
# magnitude, which the task maps onto a launch velocity.
#
# Why a third module rather than a source on ::ess::dial or ::ess::roam:
#
#   a dial   reports a 1-D angle on a ring and commits on a press.
#   a roam   integrates velocity into a free position and never commits.
#   a sling  reports a 2-D displacement and commits on RELEASE. Its live
#            cursor is not where the answer is given but where the hand has
#            pulled TO; the answer is the vector from the seat to there.
#
# The layering is the dial's and the roam's, unchanged:
#
#   the sling owns  acquisition, the pull geometry (anchor, reach), gating,
#                   the live pull for things that draw, detecting the
#                   release, and turning it into a launch vector
#   the protocol    what the vector MEANS -- what it hits, what that earns
#   owns
#
# Sources: stick | mouse | touch.
#
#   stick  slider/position (the calibrated analog stick). The DEFLECTION IS
#          THE PULL: push the stick down-left and the seat is drawn
#          down-left, so the ball launches up-right -- what a real slingshot
#          does. Engage when the deflection clears `engage`; release when it
#          falls back under `release`. A self-centring stick springs home in
#          a few samples, so the committed vector is NOT the last sample
#          above threshold (that would launch at half strength) but the
#          largest deflection in the `window_ms` before the release.
#   mouse  mouse/event (press / drag / release) from dserv's input module.
#          Press engages -- on the seat if `grab_radius` > 0, anywhere
#          otherwise, in which case the pull is measured from the press
#          point so a press far away is not an instant full draw. Release
#          commits. The y flip and the extent-to-degrees mapping are the
#          dial's (dial_mouse_range).
#   touch  mtouch/event (press / drag / lift) -- the touchscreen, in screen
#          pixels, mapped to degrees exactly as ::ess::touch_pixels_to_deg
#          does (ess/screen_w|h and ess/screen_halfx|halfy). Same gesture
#          as the mouse: a finger on the ball, dragged back, lifted. On the
#          dev Mac the stim2 window's mouse is bridged onto this datapoint
#          by configure_stim, so `touch` is the source that works there.
#
# All three feed ONE pull (sling_set_pull), so clamping, the launch mapping,
# the publishing and the commit/abort decision exist once; mouse and touch
# share one pointer path (sling_pointer_ingest) and differ only in how
# pixels become degrees.
#
# THE STATE MACHINE IS WOKEN THREE TIMES PER DRAW AT MOST: on engage, on
# commit, on abort. Never on a pull sample -- the pull is published for
# things that draw, at a throttled rate, and the machine reads the latches
# (sling_engaged / sling_committed / sling_aborted) when it is woken.
#
# Datapoints (all strings; degrees, dva/s):
#
#   ess/sling_active     0|1   -- the gate, asserted by init and deinit
#   ess/sling/sources    the source list
#   ess/sling/geometry   "anchor_x,anchor_y,reach,v_max,min_frac"
#   ess/sling/state      idle | armed | engaged | released | aborted
#   ess/sling/pull       "dx,dy,vx,vy,frac,show"  the LIVE draw: the seat's
#                        displacement from the anchor, the launch velocity a
#                        release NOW would produce, the fraction of full
#                        draw, and whether to draw it at all
#   ess/sling/release    "vx,vy,dx,dy,frac,source" published ON COMMIT --
#                        the one that belongs in the data file
#
# The launch mapping is v = -pull/reach * v_max, clamped at full draw. It is
# duplicated in the paradigm's sling_sim (~/systems/ess/lib), because this
# file cannot depend on it; tests/test_ess_sling.tcl pins the two copies
# together.
#
# WHICH SOURCE ANSWERS is the rig's to say, as it is for the dial:
#
#     setting sling sources touch
#     setting sling sources {stick touch}
#
# declared on the settings API (`sling sources`, the gear on the Sling
# panel), validated where it is written, applied live -- a bound sling
# re-initialises on the new sources without a system reload. A protocol's
# own -sources still wins over the binding (for a task where the transport
# IS the experiment); empty means "this rig declares nothing" and the module
# default (touch) applies. ess/sling/source_origin says which of the three
# is in effect and ess/sling/bound what the rig declared.
#

package require settings   ;# rig-declared sling routing (see `setting sling sources`)
package provide ess_sling 1.0

namespace eval ess {

    variable sling_pi 3.14159265358979

    # --- configuration (sling_init) ---------------------------------------
    variable sling_sources        {touch}
    variable sling_valid_sources  {stick mouse touch}
    # old / device spellings accepted at every door; docs/input_vocabulary.md
    variable sling_source_aliases
    array set sling_source_aliases { analog stick  astick stick  touchscreen touch }
    variable sling_bound_sources  {}       ;# what the rig declared
    variable sling_source_origin  default  ;# protocol | rig | default
    variable sling_init_args      {}       ;# the last init, minus -sources, for a live rebind
    variable sling_anchor_x       0.0
    variable sling_anchor_y       0.0
    variable sling_reach          3.0     ;# dva of pull at full draw
    variable sling_v_max          16.0    ;# dva/s at full draw
    variable sling_min_frac       0.15    ;# release below this = abort, not launch
    variable sling_pull_dpoint    ess/sling/pull

    # stick source
    variable sling_scale          0.0     ;# full-scale deflection; 0 = unset
    variable sling_deadzone       0.08
    variable sling_expo           1.0
    variable sling_engage         0.30    ;# fraction of full scale
    variable sling_release        0.15
    variable sling_window_ms      60
    variable sling_invert         0       ;# 1: deflection is the AIM, not the pull

    # mouse source
    variable sling_grab_radius    0.0     ;# 0 = press anywhere, pull from the press
    variable sling_mouse_scale    1.0
    variable sling_mouse_range_known 0
    variable sling_mouse_cx       0.0
    variable sling_mouse_cy       0.0
    variable sling_mouse_dpp_x    0.0
    variable sling_mouse_dpp_y    0.0

    # touch source: the screen's pixel frame -> degrees
    variable sling_touch_range_known 0
    variable sling_touch_cx       0.0
    variable sling_touch_cy       0.0
    variable sling_touch_dpp_x    0.0
    variable sling_touch_dpp_y    0.0

    # publication throttle for ess/sling/pull
    variable sling_pub_ms         0
    variable sling_min_step       0.002

    # --- live state -------------------------------------------------------
    variable sling_active         0
    variable sling_armed          0
    variable sling_armed_us       0
    variable sling_engaged        0
    variable sling_engaged_us     0
    variable sling_engaged_src    ""
    variable sling_dx             0.0
    variable sling_dy             0.0
    variable sling_frac           0.0

    # latches, cleared by sling_arm
    variable sling_committed      0
    variable sling_commit         {}      ;# {dx dy vx vy frac source}
    variable sling_commit_us      0
    variable sling_n_abort        0
    variable sling_abort_us       0

    # stick: recent samples {ts x y mag}, trimmed to window_ms
    variable sling_window         {}
    # mouse: where the pull is measured from, and whether a button is down
    variable sling_origin_x       0.0
    variable sling_origin_y       0.0

    # what the hand is doing right now, tracked whether or not the window is
    # open -- the let-go gate before a trial reads this
    variable sling_stick_f        0.0     ;# latest deflection fraction
    variable sling_pointer_down   0       ;# mouse button / finger down

    # publication bookkeeping
    variable sling_last_pub       0
    variable sling_pub_dx         0.0
    variable sling_pub_dy         0.0
    variable sling_shown          0

    ########################################################################
    # the rig binding, DECLARED (the dial's arrangement, see ess_dial)
    ########################################################################

    proc sling_source_norm { s } {
        variable sling_source_aliases
        return [expr {[info exists sling_source_aliases($s)]
                      ? $sling_source_aliases($s) : $s}]
    }

    # Normalize a source list; errors on an unknown word with a message that
    # teaches. Empty stays empty ("the rig declares nothing").
    proc sling_sources_norm { v } {
        variable sling_valid_sources
        set v [string trim $v]
        if { $v eq "" } { return "" }
        set out {}
        foreach s $v {
            set s [sling_source_norm $s]
            if { $s in {rate ring sectors} } {
                error "sling sources: `$s` is a dial READING of the stick; the\
                       sling reads the deflection directly -- use `stick`"
            }
            if { $s ni $sling_valid_sources } {
                error "sling sources: unknown source '$s' -- want any of:\
                       [join $sling_valid_sources { }] (astick/analog = stick)"
            }
            if { $s ni $out } { lappend out $s }
        }
        return $out
    }

    # What the rig declared. Called with no arguments it REPORTS.
    proc sling_bind { args } {
        variable sling_bound_sources
        if { [llength $args] == 0 } { return $sling_bound_sources }
        set sling_bound_sources [sling_sources_norm [lindex $args 0]]
        dservSet ess/sling/bound $sling_bound_sources
        return $sling_bound_sources
    }

    settings::declare sling sources -default "" \
        -validate ::ess::sling_sources_norm \
        -candidates sling \
        -doc "which inputs draw the sling: any of touch (the touchscreen; on\
              the dev Mac the stim window's mouse), mouse (dserv's mouse\
              reader), stick (the analog stick's deflection IS the pull;\
              astick/analog are old names). Applied live. Empty = the rig\
              declares nothing and the module default (touch) applies. A\
              protocol's own -sources still wins over this" \
        -apply {::ess::sling_bind_from_settings}

    # Empty CLEARS the binding (unlike the dial, nothing else here binds by
    # hand, so clearing the knob must be the undo of setting it), then a
    # bound sling that is live re-initialises on the new sources.
    proc sling_bind_from_settings { args } {
        if { [catch { ::settings::get sling sources } v] } { return }
        sling_bind [string trim $v]
        sling_rebind_live
        return
    }

    # Re-run the last init on the current binding. A protocol that named its
    # own sources is left alone. Costs the current draw, if one is in
    # progress -- changing the input device mid-trial is already a disruption.
    proc sling_rebind_live {} {
        variable sling_active
        variable sling_source_origin
        variable sling_init_args
        if { !$sling_active || $sling_source_origin eq "protocol" } { return }
        sling_init {*}$sling_init_args
        return
    }

    # Apply whatever the rig declared, once, at load (`get` lazy-loads the
    # file). catch: a bare interp with no rig file is a normal way to load.
    catch { sling_bind_from_settings }

    # What could answer a sling on this rig, right now -- the gear's picker
    # (::ess::candidates sling). Same shape as dial_source_candidates: each
    # entry names the datapoint it reads and when that last moved.
    proc sling_source_candidates {} {
        set spec [list \
            [list touch mtouch/event \
                 "a finger on the ball, dragged back, lifted (the touchscreen;\
                  on the dev Mac the stim window's mouse)" \
                 "no touchscreen is publishing (the input subprocess owns it)"] \
            [list mouse mouse/event \
                 "press, drag back, release -- dserv's mouse reader" \
                 "no mouse reader -- a dedicated mouse is opt-in BY NAME\
                  (see the input settings)"] \
            [list stick slider/position \
                 "the deflection IS the pull: push down-left, the ball flies\
                  up-right; release commits" \
                 "the slider is not publishing -- a calibrated analog stick\
                  (slider/full_scale) is needed"]]
        set out {}
        foreach e $spec {
            lassign $e route dp what hint
            set since ""
            catch { set since [dial_dp_since $dp] }
            if { $since eq "" } {
                set status unresolved
                set detail "$what -- nothing publishes $dp yet; $hint"
            } else {
                set status ok
                set detail "$what -- $dp, $since"
            }
            lappend out [dict create route $route label $route detail $detail \
                             status $status durable 1 selectable 1 multi 1 \
                             conflicts {} address $dp \
                             note "ticked ones draw; the first to let go launches"]
        }
        return $out
    }

    ########################################################################
    # the one mapping: pull -> launch velocity
    ########################################################################

    # Mirrors sling_sim::velocity exactly (see the header).
    proc sling_velocity { dx dy } {
        variable sling_reach
        variable sling_v_max
        if { $sling_reach <= 0.0 } { return {0.0 0.0} }
        set mag [expr {hypot($dx, $dy)}]
        if { $mag <= 0.0 } { return {0.0 0.0} }
        set frac [expr {$mag/$sling_reach}]
        if { $frac > 1.0 } { set frac 1.0 }
        set speed [expr {$sling_v_max*$frac}]
        return [list [expr {-$dx/$mag*$speed}] [expr {-$dy/$mag*$speed}]]
    }

    # Clamp a pull to the reach. Returns {dx dy frac}.
    proc sling_clamp { dx dy } {
        variable sling_reach
        set mag [expr {hypot($dx, $dy)}]
        if { $sling_reach <= 0.0 || $mag <= 0.0 } { return [list 0.0 0.0 0.0] }
        set frac [expr {$mag/$sling_reach}]
        if { $frac > 1.0 } {
            set k [expr {$sling_reach/$mag}]
            return [list [expr {$dx*$k}] [expr {$dy*$k}] 1.0]
        }
        return [list $dx $dy $frac]
    }

    ########################################################################
    # geometry (per trial) and publishing
    ########################################################################

    # Called with no arguments it REPORTS (the file's convention).
    proc sling_set_anchor { args } {
        variable sling_anchor_x
        variable sling_anchor_y
        if { [llength $args] == 0 } { return [list $sling_anchor_x $sling_anchor_y] }
        if { [llength $args] != 2 } { error "::ess::sling_set_anchor: want x y" }
        lassign $args x y
        set sling_anchor_x [expr {double($x)}]
        set sling_anchor_y [expr {double($y)}]
        sling_publish_geometry
        return [list $sling_anchor_x $sling_anchor_y]
    }

    proc sling_set_gain { args } {
        variable sling_reach
        variable sling_v_max
        variable sling_min_frac
        if { [llength $args] == 0 } {
            return [list reach $sling_reach v_max $sling_v_max min_frac $sling_min_frac]
        }
        foreach { k v } $args {
            switch -- $k {
                -reach    { set sling_reach    [expr {double($v)}] }
                -v_max    { set sling_v_max    [expr {double($v)}] }
                -min_frac { set sling_min_frac [expr {double($v)}] }
                default   { error "::ess::sling_set_gain: unknown option '$k'" }
            }
        }
        if { $sling_reach <= 0.0 } { error "::ess::sling_set_gain: reach must be > 0" }
        if { $sling_min_frac < 0.0 || $sling_min_frac >= 1.0 } {
            error "::ess::sling_set_gain: min_frac is a fraction of full draw in \[0,1)"
        }
        sling_publish_geometry
        return [list reach $sling_reach v_max $sling_v_max min_frac $sling_min_frac]
    }

    proc sling_publish_geometry {} {
        variable sling_anchor_x; variable sling_anchor_y
        variable sling_reach; variable sling_v_max; variable sling_min_frac
        dservSet ess/sling/geometry \
            "[format %.4f $sling_anchor_x],[format %.4f $sling_anchor_y],[format %.4f $sling_reach],[format %.4f $sling_v_max],[format %.4f $sling_min_frac]"
    }

    proc sling_publish_state { s } {
        dservSet ess/sling/state $s
    }

    # The live draw, throttled. A show change is never throttled.
    proc sling_publish_pull { ts show { force 0 } } {
        variable sling_pull_dpoint
        variable sling_pub_ms
        variable sling_min_step
        variable sling_last_pub
        variable sling_pub_dx
        variable sling_pub_dy
        variable sling_shown
        variable sling_dx
        variable sling_dy
        variable sling_frac

        if { !$force && $show == $sling_shown } {
            set due [expr {$sling_pub_ms <= 0 ||
                           ($ts - $sling_last_pub)/1000.0 >= $sling_pub_ms}]
            set moved [expr {abs($sling_dx - $sling_pub_dx) +
                             abs($sling_dy - $sling_pub_dy)}]
            if { !($due && $moved > $sling_min_step) } return
        }
        set sling_last_pub $ts
        set sling_pub_dx   $sling_dx
        set sling_pub_dy   $sling_dy
        set sling_shown    $show
        lassign [sling_velocity $sling_dx $sling_dy] vx vy
        dservSet $sling_pull_dpoint \
            "[format %.4f $sling_dx],[format %.4f $sling_dy],[format %.4f $vx],[format %.4f $vy],[format %.4f $sling_frac],$show"
    }

    proc sling_pull_hide {} {
        variable sling_pull_dpoint
        variable sling_shown
        variable sling_dx; variable sling_dy; variable sling_frac
        set sling_dx 0.0; set sling_dy 0.0; set sling_frac 0.0
        set sling_shown 0
        dservSet $sling_pull_dpoint "0.0000,0.0000,0.0000,0.0000,0.0000,0"
    }

    ########################################################################
    # the pull -- one path for every source
    ########################################################################

    # Adopt a raw pull (dx,dy from the anchor), clamp it, publish it.
    proc sling_set_pull { dx dy ts } {
        variable sling_dx; variable sling_dy; variable sling_frac
        lassign [sling_clamp $dx $dy] sling_dx sling_dy sling_frac
        sling_publish_pull $ts 1
    }

    # The hand took hold. Latches the engage time and wakes the machine ONCE.
    proc sling_engage { src ts } {
        variable sling_engaged
        variable sling_engaged_us
        variable sling_engaged_src
        variable sling_window
        set sling_engaged     1
        set sling_engaged_us  $ts
        set sling_engaged_src $src
        set sling_window      {}
        sling_publish_state engaged
        do_update
    }

    # The hand let go. A draw shorter than min_frac is an ABORT (the ball
    # settles back into the seat; the draw may be tried again); otherwise a
    # COMMIT, which closes the window -- one launch per arm.
    proc sling_let_go { dx dy src ts } {
        variable sling_engaged
        variable sling_armed
        variable sling_min_frac
        variable sling_committed
        variable sling_commit
        variable sling_commit_us
        variable sling_n_abort
        variable sling_abort_us
        variable sling_dx; variable sling_dy; variable sling_frac

        set sling_engaged 0
        lassign [sling_clamp $dx $dy] cdx cdy cfrac

        if { $cfrac < $sling_min_frac } {
            set sling_dx 0.0; set sling_dy 0.0; set sling_frac 0.0
            incr sling_n_abort
            set sling_abort_us $ts
            sling_publish_pull $ts 0 1
            sling_publish_state aborted
            do_update
            return
        }

        set sling_dx $cdx; set sling_dy $cdy; set sling_frac $cfrac
        lassign [sling_velocity $cdx $cdy] vx vy
        set sling_commit    [list $cdx $cdy $vx $vy $cfrac $src]
        set sling_commit_us $ts
        set sling_committed 1
        set sling_armed     0
        dservSet ess/sling/release \
            "[format %.4f $vx],[format %.4f $vy],[format %.4f $cdx],[format %.4f $cdy],[format %.4f $cfrac],$src"
        sling_publish_pull $ts 0 1
        sling_publish_state released
        do_update
    }

    ########################################################################
    # stick source -- the deflection is the pull
    ########################################################################

    proc sling_stick_sample { dpoint data } {
        lassign $data sx sy
        if { $sx eq "" } return
        if { $sy eq "" } { set sy 0.0 }
        sling_stick_ingest $sx $sy [dservTimestamp $dpoint]
    }

    proc sling_stick_ingest { sx sy ts } {
        variable sling_active
        variable sling_armed
        variable sling_engaged
        variable sling_scale
        variable sling_deadzone
        variable sling_expo
        variable sling_engage
        variable sling_release
        variable sling_window
        variable sling_window_ms
        variable sling_invert
        variable sling_reach
        variable sling_stick_f

        if { !$sling_active } return
        if { $sling_scale <= 0.0 } return
        if { $sling_invert } { set sx [expr {-$sx}]; set sy [expr {-$sy}] }

        set mag [expr {hypot($sx, $sy)}]
        set f   [expr {$mag/$sling_scale}]
        set sling_stick_f $f
        if { !$sling_armed } return

        if { !$sling_engaged } {
            if { $f < $sling_engage } return
            sling_engage stick $ts
            # fall through: the engaging sample is also the first pull
        } elseif { $f < $sling_release } {
            # RELEASE. Commit the largest deflection in the window, not this
            # sample: the stick is already most of the way home.
            set bx $sx; set by $sy; set bm $mag
            foreach s $sling_window {
                lassign $s wts wx wy wm
                if { $wm > $bm } { set bx $wx; set by $wy; set bm $wm }
            }
            lassign [sling_stick_pull $bx $by $bm] dx dy
            sling_let_go $dx $dy stick $ts
            return
        }

        # keep the window: samples newer than window_ms
        lappend sling_window [list $ts $sx $sy $mag]
        set cut [expr {$ts - $sling_window_ms*1000}]
        while { [llength $sling_window] && [lindex $sling_window 0 0] < $cut } {
            set sling_window [lrange $sling_window 1 end]
        }
        lassign [sling_stick_pull $sx $sy $mag] dx dy
        sling_set_pull $dx $dy $ts
    }

    # deflection -> pull displacement: direction from the raw vector, size
    # from the shaped gain (::ess::stick_gain, shared with dial and roam) times
    # the reach. Never per-axis -- see stick_velocity's comment.
    proc sling_stick_pull { sx sy mag } {
        variable sling_scale
        variable sling_deadzone
        variable sling_expo
        variable sling_reach
        if { $mag <= 0.0 } { return {0.0 0.0} }
        set g [stick_gain [expr {$mag/$sling_scale}] $sling_deadzone $sling_expo]
        set r [expr {$g*$sling_reach}]
        return [list [expr {$sx/$mag*$r}] [expr {$sy/$mag*$r}]]
    }

    ########################################################################
    # mouse source -- press, drag, release
    ########################################################################

    # Same extent -> degrees mapping as the dial's, for the same reasons.
    proc sling_mouse_range { dpoint data } {
        variable sling_mouse_range_known
        variable sling_mouse_cx
        variable sling_mouse_cy
        variable sling_mouse_dpp_x
        variable sling_mouse_dpp_y
        variable sling_mouse_scale
        lassign $data minx maxx miny maxy
        if { $maxx eq "" || $maxy eq "" } return
        set spanx [expr {$maxx - $minx}]
        set spany [expr {$maxy - $miny}]
        if { $spanx <= 0 || $spany <= 0 } return
        set sling_mouse_cx [expr {($minx + $maxx)/2.0}]
        set sling_mouse_cy [expr {($miny + $maxy)/2.0}]
        set hx ""; set hy ""
        catch { set hx [dservGet ess/screen_halfx] }
        catch { set hy [dservGet ess/screen_halfy] }
        if { ![string is double -strict $hx] || $hx <= 0 ||
             ![string is double -strict $hy] || $hy <= 0 } {
            set hx 16.0; set hy 9.0
            puts stderr "::ess::sling: ess/screen_halfx|halfy unavailable;\
                         mouse scale falling back to ${hx}x${hy} deg"
        }
        set sling_mouse_dpp_x [expr {2.0*$hx*$sling_mouse_scale/$spanx}]
        set sling_mouse_dpp_y [expr {2.0*$hy*$sling_mouse_scale/$spany}]
        set sling_mouse_range_known 1
    }

    proc sling_mouse_sample { dpoint data } {
        variable sling_mouse_range_known
        variable sling_mouse_cx; variable sling_mouse_cy
        variable sling_mouse_dpp_x; variable sling_mouse_dpp_y
        if { !$sling_mouse_range_known } return
        lassign $data x y ev
        if { $x eq "" || $y eq "" || $ev eq "" } return
        # published y grows DOWNWARD; flip into screen degrees
        set px [expr {($x - $sling_mouse_cx)*$sling_mouse_dpp_x}]
        set py [expr {($sling_mouse_cy - $y)*$sling_mouse_dpp_y}]
        sling_pointer_ingest $px $py $ev [dservTimestamp $dpoint] mouse
    }

    proc sling_mouse_ingest { px py ev ts } {
        sling_pointer_ingest $px $py $ev $ts mouse
    }

    ########################################################################
    # touch source -- a finger on the ball, dragged back, lifted
    ########################################################################

    # The screen's pixel frame, from the datapoints ::ess publishes at init.
    # Same mapping as ::ess::touch_pixels_to_deg (pixel origin top-left, y
    # downward), read from the datapoints rather than the system's variables
    # so this module does not reach into the loaded system's namespace.
    proc sling_touch_range {} {
        variable sling_touch_range_known
        variable sling_touch_cx; variable sling_touch_cy
        variable sling_touch_dpp_x; variable sling_touch_dpp_y
        set w ""; set h ""; set hx ""; set hy ""
        catch { set w  [dservGet ess/screen_w] }
        catch { set h  [dservGet ess/screen_h] }
        catch { set hx [dservGet ess/screen_halfx] }
        catch { set hy [dservGet ess/screen_halfy] }
        foreach v [list $w $h $hx $hy] {
            if { ![string is double -strict $v] || $v <= 0 } {
                set sling_touch_range_known 0
                puts stderr "::ess::sling: touch source needs ess/screen_w|h and\
                             ess/screen_halfx|halfy -- not available; touch\
                             samples will be ignored until sling_init is re-run"
                return 0
            }
        }
        set sling_touch_cx    [expr {$w/2.0}]
        set sling_touch_cy    [expr {$h/2.0}]
        set sling_touch_dpp_x [expr {2.0*$hx/$w}]
        set sling_touch_dpp_y [expr {2.0*$hy/$h}]
        set sling_touch_range_known 1
        return 1
    }

    # mtouch/event: {x y ev} in screen pixels; 0 PRESS, 1 DRAG, 2 RELEASE.
    proc sling_touch_sample { dpoint data } {
        variable sling_touch_range_known
        variable sling_touch_cx; variable sling_touch_cy
        variable sling_touch_dpp_x; variable sling_touch_dpp_y
        if { !$sling_touch_range_known } return
        lassign $data x y ev
        if { $x eq "" || $y eq "" || $ev eq "" } return
        set px [expr {($x - $sling_touch_cx)*$sling_touch_dpp_x}]
        set py [expr {($sling_touch_cy - $y)*$sling_touch_dpp_y}]
        sling_pointer_ingest $px $py $ev [dservTimestamp $dpoint] touch
    }

    ########################################################################
    # the pointer path -- mouse and touch, in degrees
    ########################################################################

    # ev: 0 PRESS, 1 DRAG, 2 RELEASE, 3 MOVE (ignored). px,py in degrees.
    proc sling_pointer_ingest { px py ev ts src } {
        variable sling_active
        variable sling_armed
        variable sling_engaged
        variable sling_anchor_x; variable sling_anchor_y
        variable sling_grab_radius
        variable sling_origin_x; variable sling_origin_y
        variable sling_pointer_down

        if { !$sling_active } return
        if { $ev == 0 } { set sling_pointer_down 1 } elseif { $ev == 2 } { set sling_pointer_down 0 }
        if { !$sling_armed } return
        switch -exact -- $ev {
            0 {
                if { $sling_engaged } return
                if { $sling_grab_radius > 0.0 } {
                    set d [expr {hypot($px - $sling_anchor_x, $py - $sling_anchor_y)}]
                    if { $d > $sling_grab_radius } return
                    set sling_origin_x $sling_anchor_x
                    set sling_origin_y $sling_anchor_y
                } else {
                    set sling_origin_x $px
                    set sling_origin_y $py
                }
                sling_engage $src $ts
                sling_set_pull [expr {$px - $sling_origin_x}] [expr {$py - $sling_origin_y}] $ts
            }
            1 {
                if { !$sling_engaged } return
                sling_set_pull [expr {$px - $sling_origin_x}] [expr {$py - $sling_origin_y}] $ts
            }
            2 {
                if { !$sling_engaged } return
                sling_let_go [expr {$px - $sling_origin_x}] [expr {$py - $sling_origin_y}] $src $ts
            }
            default {}
        }
    }

    ########################################################################
    # lifecycle
    ########################################################################

    proc sling_init { args } {
        variable sling_sources
        variable sling_valid_sources
        variable sling_anchor_x; variable sling_anchor_y
        variable sling_reach; variable sling_v_max; variable sling_min_frac
        variable sling_pull_dpoint
        variable sling_scale; variable sling_deadzone; variable sling_expo
        variable sling_engage; variable sling_release; variable sling_window_ms
        variable sling_invert
        variable sling_grab_radius; variable sling_mouse_scale
        variable sling_pub_ms; variable sling_min_step
        variable sling_active

        # Defaults on every init (see ess_roam for why every variable
        # assigned here must be declared above). Sources default to the
        # RIG's binding when it has one, so a protocol need not -- and
        # should not -- name hardware.
        variable sling_bound_sources
        variable sling_source_origin
        variable sling_init_args
        if { [llength $sling_bound_sources] } {
            set sling_sources       $sling_bound_sources
            set sling_source_origin rig
        } else {
            set sling_sources       {touch}
            set sling_source_origin default
        }
        set sling_anchor_x    0.0
        set sling_anchor_y    0.0
        set sling_reach       3.0
        set sling_v_max       16.0
        set sling_min_frac    0.15
        set sling_pull_dpoint ess/sling/pull
        set sling_scale       0.0
        set sling_deadzone    0.08
        set sling_expo        1.0
        set sling_engage      0.30
        set sling_release     0.15
        set sling_window_ms   60
        set sling_invert      0
        set sling_grab_radius 0.0
        set sling_mouse_scale 1.0
        set sling_pub_ms      0
        set sling_min_step    0.002
        variable sling_stick_f;      set sling_stick_f 0.0
        variable sling_pointer_down; set sling_pointer_down 0

        set anchor {}
        # kept for a live rebind (minus -sources, which the binding supplies)
        set sling_init_args {}
        foreach { k v } $args {
            if { $k ne "-sources" } { lappend sling_init_args $k $v }
        }
        foreach { k v } $args {
            switch -- $k {
                -sources     { set sling_sources [sling_sources_norm $v]
                               set sling_source_origin protocol }
                -anchor      { set anchor $v }
                -reach       { set sling_reach       [expr {double($v)}] }
                -v_max       { set sling_v_max       [expr {double($v)}] }
                -min_frac    { set sling_min_frac    [expr {double($v)}] }
                -scale       { set sling_scale       [expr {double($v)}] }
                -deadzone    { set sling_deadzone    [expr {double($v)}] }
                -expo        { set sling_expo        [expr {double($v)}] }
                -engage      { set sling_engage      [expr {double($v)}] }
                -release     { set sling_release     [expr {double($v)}] }
                -window_ms   { set sling_window_ms   [expr {int($v)}] }
                -invert      { set sling_invert      [expr {int($v)}] }
                -grab_radius { set sling_grab_radius [expr {double($v)}] }
                -mouse_scale { set sling_mouse_scale [expr {double($v)}] }
                -pub_ms      { set sling_pub_ms      [expr {int($v)}] }
                -min_step    { set sling_min_step    [expr {double($v)}] }
                -pull_dpoint { set sling_pull_dpoint $v }
                default { error "::ess::sling_init: unknown option '$k'" }
            }
        }

        if { ![llength $sling_sources] } {
            error "::ess::sling_init: no sources"
        }
        # A stick needs the rig's full-scale deflection to mean anything.
        # Read it here when the protocol did not say, so a rig that switches
        # to the stick from the gear works without a protocol change.
        if { "stick" in $sling_sources && $sling_scale <= 0.0 } {
            if { [dservExists slider/full_scale] } {
                catch { set sling_scale [expr {double([dservGet slider/full_scale])}] }
            }
        }
        if { $sling_release >= $sling_engage } {
            error "::ess::sling_init: -release ($sling_release) must be below\
                   -engage ($sling_engage) or every draw releases at once"
        }
        if { $sling_reach <= 0.0 } { error "::ess::sling_init: -reach must be > 0" }

        sling_deinit                ;# idempotent; drops any previous wiring

        if { [llength $anchor] } { sling_set_anchor {*}$anchor }

        if { "stick" in $sling_sources } {
            dservAddExactMatch slider/position
            dpointAddScript    slider/position ::ess::sling_stick_sample
        }
        if { "mouse" in $sling_sources } {
            variable sling_mouse_range_known
            set sling_mouse_range_known 0
            dservAddExactMatch mouse/event/range
            dpointAddScript    mouse/event/range ::ess::sling_mouse_range
            dservAddExactMatch mouse/event
            dpointAddScript    mouse/event ::ess::sling_mouse_sample
            # mouse/event/range publishes on change only; adopt a range that
            # is already known
            if { [dservExists mouse/event/range] } {
                catch { sling_mouse_range mouse/event/range [dservGet mouse/event/range] }
            }
        }
        if { "touch" in $sling_sources } {
            sling_touch_range
            dservAddExactMatch mtouch/event
            dpointAddScript    mtouch/event ::ess::sling_touch_sample
            # Ask the stim-side mouse->touch bridge (configure_stim, dev
            # Mac) for DRAG events; it is inert otherwise. No stim, or a
            # real touchscreen rig, and this is simply a no-op.
            catch { rmtSend {set ::mouse_bridge_drag 1} }
        }

        set sling_active 1
        dservSet ess/sling_active  1
        dservSet ess/sling/sources $sling_sources
        dservSet ess/sling/source_origin $sling_source_origin
        dservSet ess/sling/bound   $sling_bound_sources
        sling_publish_geometry
        sling_publish_state idle
        sling_pull_hide
        return
    }

    proc sling_deinit {} {
        variable sling_active
        variable sling_armed
        variable sling_engaged
        catch { dpointRemoveScript slider/position   ::ess::sling_stick_sample }
        catch { dpointRemoveScript mouse/event       ::ess::sling_mouse_sample }
        catch { dpointRemoveScript mouse/event/range ::ess::sling_mouse_range }
        catch { dpointRemoveScript mtouch/event      ::ess::sling_touch_sample }
        # Switch the stim-side drag bridge back off, and clear its down
        # flag in case a release was lost -- so nothing keeps streaming
        # mtouch/event after the sling is gone.
        catch { rmtSend {set ::mouse_bridge_drag 0; set ::mouse_bridge_down 0} }
        set sling_armed   0
        set sling_engaged 0
        set sling_active  0
        dservSet ess/sling_active  0
        dservSet ess/sling/sources {}
        dservSet ess/sling/source_origin {}
        sling_publish_state idle
        sling_pull_hide
        return
    }

    # Open the window: from here a draw is accepted and a release answers.
    # Clears every latch of the previous draw.
    proc sling_arm {} {
        variable sling_active
        variable sling_armed
        variable sling_armed_us
        variable sling_engaged
        variable sling_engaged_us
        variable sling_engaged_src
        variable sling_committed
        variable sling_commit
        variable sling_commit_us
        variable sling_n_abort
        variable sling_abort_us
        variable sling_window
        variable sling_sources
        variable sling_scale
        if { !$sling_active } { error "::ess::sling_arm: no sling is initialized" }
        if { "stick" in $sling_sources && $sling_scale <= 0.0 } {
            error "::ess::sling_arm: -sources stick with no -scale, so a full\
                   push means nothing. Read it from slider/full_scale"
        }
        set sling_armed       1
        set sling_armed_us    [now]
        set sling_engaged     0
        set sling_engaged_us  0
        set sling_engaged_src ""
        set sling_committed   0
        set sling_commit      {}
        set sling_commit_us   0
        set sling_n_abort     0
        set sling_abort_us    0
        set sling_window      {}
        # republish the geometry INSIDE the obs period, so the record lands
        # under the trial it describes (the roam_start argument)
        sling_publish_geometry
        sling_publish_state armed
        sling_pull_hide
        return
    }

    # Close the window without an answer (timeout, abort). The latches stay
    # readable until the next arm.
    proc sling_disarm {} {
        variable sling_armed
        variable sling_engaged
        set sling_armed   0
        set sling_engaged 0
        sling_publish_state idle
        sling_pull_hide
        return
    }

    ########################################################################
    # what the state machine reads
    ########################################################################

    proc sling_armed {}        { variable sling_armed;       return $sling_armed }
    proc sling_engaged {}      { variable sling_engaged;     return $sling_engaged }
    proc sling_engage_time {}  { variable sling_engaged_us;  return $sling_engaged_us }
    proc sling_engage_source {} { variable sling_engaged_src; return $sling_engaged_src }
    proc sling_committed {}    { variable sling_committed;   return $sling_committed }
    # {dx dy vx vy frac source} of the commit, or {}
    proc sling_release_vec {}  { variable sling_commit;      return $sling_commit }
    proc sling_release_time {} { variable sling_commit_us;   return $sling_commit_us }
    proc sling_aborts {}       { variable sling_n_abort;     return $sling_n_abort }
    proc sling_abort_time {}   { variable sling_abort_us;    return $sling_abort_us }
    # the live pull {dx dy frac}
    proc sling_pull {} {
        variable sling_dx; variable sling_dy; variable sling_frac
        return [list $sling_dx $sling_dy $sling_frac]
    }
    proc sling_active {}       { variable sling_active;      return $sling_active }

    # Is the hand on the input right now -- stick deflected past the release
    # threshold, or a mouse button / finger down? Tracked whether or not the
    # window is open, so a trial can refuse to start until the hand lets go
    # (the button family's letgo gate).
    proc sling_held {} {
        variable sling_sources
        variable sling_stick_f
        variable sling_release
        variable sling_pointer_down
        if { "stick" in $sling_sources && $sling_stick_f >= $sling_release } { return 1 }
        if { ("mouse" in $sling_sources || "touch" in $sling_sources) && $sling_pointer_down } { return 1 }
        return 0
    }

    # Live adjustment. Called with no arguments it REPORTS.
    proc sling_tune { args } {
        variable sling_scale; variable sling_deadzone; variable sling_expo
        variable sling_engage; variable sling_release; variable sling_window_ms
        variable sling_invert; variable sling_grab_radius; variable sling_mouse_scale
        variable sling_pub_ms; variable sling_min_step
        foreach { k v } $args {
            switch -- $k {
                -scale       { set sling_scale       [expr {double($v)}] }
                -deadzone    { set sling_deadzone    [expr {double($v)}] }
                -expo        { set sling_expo        [expr {double($v)}] }
                -engage      { set sling_engage      [expr {double($v)}] }
                -release     { set sling_release     [expr {double($v)}] }
                -window_ms   { set sling_window_ms   [expr {int($v)}] }
                -invert      { set sling_invert      [expr {int($v)}] }
                -grab_radius { set sling_grab_radius [expr {double($v)}] }
                -mouse_scale { set sling_mouse_scale [expr {double($v)}] }
                -pub_ms      { set sling_pub_ms      [expr {int($v)}] }
                -min_step    { set sling_min_step    [expr {double($v)}] }
                default      { error "::ess::sling_tune: unknown option '$k'" }
            }
        }
        if { $sling_release >= $sling_engage } {
            error "::ess::sling_tune: release must be below engage"
        }
        return [list scale $sling_scale deadzone $sling_deadzone expo $sling_expo \
                    engage $sling_engage release $sling_release \
                    window_ms $sling_window_ms invert $sling_invert \
                    grab_radius $sling_grab_radius mouse_scale $sling_mouse_scale \
                    pub_ms $sling_pub_ms min_step $sling_min_step]
    }

    ########################################################################
    # operator / headless driving -- the SAME paths as real hardware
    ########################################################################

    # A stick sample in raw deflection units (the slider/position frame).
    proc sling_simulate_stick { x y { ts {} } } {
        if { $ts eq "" } { set ts [now] }
        sling_stick_ingest [expr {double($x)}] [expr {double($y)}] $ts
    }

    # A mouse / touch event in screen DEGREES: press | drag | release.
    proc sling_simulate_mouse { what x y { ts {} } } {
        if { $ts eq "" } { set ts [now] }
        set ev [dict get {press 0 drag 1 release 2 move 3} $what]
        sling_pointer_ingest [expr {double($x)}] [expr {double($y)}] $ev $ts mouse
    }
    proc sling_simulate_touch { what x y { ts {} } } {
        if { $ts eq "" } { set ts [now] }
        set ev [dict get {press 0 drag 1 release 2} $what]
        sling_pointer_ingest [expr {double($x)}] [expr {double($y)}] $ev $ts touch
    }

    namespace export sling_init sling_deinit sling_arm sling_disarm \
        sling_bind sling_sources_norm sling_source_candidates \
        sling_set_anchor sling_set_gain sling_velocity sling_tune \
        sling_armed sling_engaged sling_engage_time sling_engage_source \
        sling_committed sling_release_vec sling_release_time \
        sling_aborts sling_abort_time sling_pull sling_active sling_held \
        sling_simulate_stick sling_simulate_mouse sling_simulate_touch \
        sling_pull_hide
}

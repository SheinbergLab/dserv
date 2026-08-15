# -*- mode: tcl -*-
#
# ess_dial-1.0.tm
#
# The "dial" response mode: the subject reports a DIRECTION on a circle (or
# an arc of one) by moving a continuous input, watching a live cursor, and
# committing explicitly.
#
# This is a RESPONSE MODE, not a transport. button/joystick/slider/touch each
# wrap a piece of hardware; a dial composes them into an experimental idiom.
# That is why it lives beside ess-2.0.tm rather than inside it — the next
# response mode gets its own file rather than growing the framework's.
#
# The division of labour, which matters:
#
#   the dial owns   acquisition, geometry, gating, the live cursor, and
#                   detecting a commit
#   the protocol owns  what the committed angle MEANS — whether it is
#                   correct, what the error metric is, what to reward
#
# Same layering as buttons: ::ess::button_active hands you a channel and the
# protocol decides what a channel means.
#
# Extracted from sequence/mp_pulsed and planko/ricochet, which had grown the
# same ~40-line machine twice. They had also DRIFTED, and both divergences
# were bugs that this file resolves by construction:
#
#   1. mp_pulsed compared cursor angles with abs($a - $last), so crossing
#      0/2pi produced a difference near 2pi that always cleared the deadband
#      — the cursor updated on every sample exactly where the subject reports
#      "straight right". ricochet used a circular difference. Here there is
#      one comparison and it is circular (see dial_angdiff).
#
#   2. mp_pulsed gated touch commits only on "not already consumed", not on
#      "after the response window opened", so a press landing before the
#      window could commit. ricochet gated on both. Here the window gate is
#      in dial_arm and applies to every source, so a protocol cannot forget
#      it for one modality.
#
# Arc angles are in DEGREES (matching the params and stimdg columns that feed
# them); the angles themselves are radians (matching atan2 and exit_angle).
# That split is inherited from ricochet-1.0.tm on purpose — a dial fed from a
# stimdg column should not have to convert at the call site.
#

package provide ess_dial 1.0

namespace eval ess {

    variable dial_pi 3.14159265358979

    # --- configuration (set by dial_init, adjustable per trial) -----------
    variable dial_active         0
    variable dial_sources        {swipe touch}
    variable dial_deadband_deg   2.0
    variable dial_arc_center     0.0     ;# degrees
    variable dial_arc_halfwidth  180.0   ;# degrees; 180 = whole circle
    variable dial_radius         0.0     ;# ring radius, touch source only
    variable dial_ring_tolerance 0.0     ;# band half-width around it
    variable dial_cursor_dpoint  ess/cursor

    # --- per-response state ----------------------------------------------
    # dial_armed_time is the gate: 0 means disarmed, otherwise nothing that
    # happened before it counts as a response. Every source consults it.
    variable dial_armed_time     0
    variable dial_cursor_shown   0
    variable dial_last_sent      0.0
    variable dial_swipe_last     0
    variable dial_touch_last     0
    variable dial_report_angle   -1.0
    variable dial_report_time    0
    variable dial_report_source  ""
    # A simulated commit waiting to be consumed by the next dial_response.
    # It must NOT be applied directly: dial_response is where a commit
    # becomes the protocol's response, and short-circuiting that made the
    # simulated angle unreachable (dial_response returns -1 once disarmed).
    variable dial_pending        ""

    # ---------------------------------------------------------------------
    # angle helpers
    # ---------------------------------------------------------------------

    # Signed shortest difference a-b, in (-pi, pi]. The ONLY way angles are
    # compared in this file.
    proc dial_angdiff { a b } {
        return [expr {atan2(sin($a-$b), cos($a-$b))}]
    }

    proc dial_norm2pi { a } {
        variable dial_pi
        set a [expr {fmod($a, 2*$dial_pi)}]
        if { $a < 0 } { set a [expr {$a + 2*$dial_pi}] }
        return $a
    }

    # Is this angle inside the reportable arc?
    #
    # The epsilon matters: a literal pi here is a hair under atan2's, so
    # without slack a halfwidth of 180 (meaning "the whole circle") rejects
    # the exact-opposite angle by ~3e-15. Inherited from ricochet-1.0.tm,
    # where it was found the hard way.
    proc dial_in_arc { angle_rad } {
        variable dial_pi
        variable dial_arc_center
        variable dial_arc_halfwidth
        set d [dial_angdiff $angle_rad [expr {$dial_arc_center*$dial_pi/180.}]]
        return [expr {abs($d) <= $dial_arc_halfwidth*$dial_pi/180. + 1e-9}]
    }

    # Snap an angle into the arc, to the NEARER endpoint. Crossing the
    # excluded wedge's centreline flips which end you snap to, which is fine:
    # the wedge is dead space and the subject is never shown a cursor in it.
    proc dial_clamp_arc { angle_rad } {
        variable dial_pi
        variable dial_arc_center
        variable dial_arc_halfwidth
        set c [expr {$dial_arc_center*$dial_pi/180.}]
        set h [expr {$dial_arc_halfwidth*$dial_pi/180.}]
        set d [dial_angdiff $angle_rad $c]
        if { $d >  $h } { set d $h }
        if { $d < -$h } { set d [expr {-$h}] }
        return [dial_norm2pi [expr {$c + $d}]]
    }

    # ---------------------------------------------------------------------
    # lifecycle
    # ---------------------------------------------------------------------

    # ::ess::dial_init ?-sources {swipe touch}?
    #                  ?-deadband_deg N?
    #                  ?-arc_center DEG? ?-arc_halfwidth DEG?
    #                  ?-radius R? ?-ring_tolerance T?
    #                  ?-cursor_dpoint NAME?
    #
    # Does NOT initialise the underlying transport — the protocol still calls
    # ::ess::slider_init / ::ess::touch_init with the settings it wants (swipe
    # threshold, release behaviour). The dial reads them; it does not own them.
    proc dial_init { args } {
        variable dial_active
        variable dial_sources
        variable dial_deadband_deg
        variable dial_arc_center
        variable dial_arc_halfwidth
        variable dial_radius
        variable dial_ring_tolerance
        variable dial_cursor_dpoint

        # defaults on every init, so a protocol that omits an option gets the
        # documented value rather than whatever the previous protocol set
        set dial_sources        {swipe touch}
        set dial_deadband_deg   2.0
        set dial_arc_center     0.0
        set dial_arc_halfwidth  180.0
        set dial_radius         0.0
        set dial_ring_tolerance 0.0
        set dial_cursor_dpoint  ess/cursor

        foreach { k v } $args {
            switch -- $k {
                -sources        { set dial_sources $v }
                -deadband_deg   { set dial_deadband_deg $v }
                -arc_center     { set dial_arc_center $v }
                -arc_halfwidth  { set dial_arc_halfwidth $v }
                -radius         { set dial_radius $v }
                -ring_tolerance { set dial_ring_tolerance $v }
                -cursor_dpoint  { set dial_cursor_dpoint $v }
                default { error "::ess::dial_init: unknown option '$k'" }
            }
        }

        foreach s $dial_sources {
            if { $s ni {swipe touch joystick} } {
                error "::ess::dial_init: unknown source '$s'\
                       (want swipe|touch|joystick)"
            }
        }

        # Observe slider position DIRECTLY rather than doing the cursor work
        # on the state machine's tick. dpointAddScript (not SetScript) so this
        # sits alongside ::ess::slider_process rather than displacing it --
        # before multi-script existed, the cursor had nowhere to live but the
        # SM's update, which is why the SM had to be woken per sample.
        #
        # Consequence worth having: the cursor now tracks whenever the dial is
        # armed, regardless of which state the machine happens to be in, and
        # at the device's rate rather than the SM's.
        if { "swipe" in $dial_sources } {
            dservAddExactMatch slider/position
            dpointAddScript    slider/position ::ess::dial_slider_sample
        }

        dial_disarm
        set dial_active 1
        dservSet ess/dial_active 1
        dial_publish_geometry
    }

    proc dial_deinit {} {
        variable dial_active
        if { !$dial_active } return
        # Remove only OUR script; slider_process keeps its registration.
        catch { dpointRemoveScript slider/position ::ess::dial_slider_sample }
        dial_disarm
        set dial_active 0
        dservSet ess/dial_active 0
    }

    # Publish the dial's geometry so anything outside ess can DRAW it.
    #
    # The viz subprocess has no ess package -- it can only read datapoints
    # and stimdg -- so a visualisation cannot ask the dial anything. Making
    # the dial self-describing is what lets one shared viz serve every
    # system that uses a dial, instead of each hand-rolling the ring, the
    # arc and the cursor as ricochet, mp_pulsed and motionpatch each do now.
    #
    # ess/dial/geometry : "arc_center_deg,arc_halfwidth_deg,radius"
    # ess/cursor        : "angle_rad,show"   (published live; see above)
    proc dial_publish_geometry {} {
        variable dial_arc_center
        variable dial_arc_halfwidth
        variable dial_radius
        dservSet ess/dial/geometry \
            "$dial_arc_center,$dial_arc_halfwidth,$dial_radius"
    }

    # Per-trial geometry. Separate from dial_init because the arc and the
    # ring radius usually come from stimdg and change every trial, while the
    # deadband and source list do not.
    proc dial_set_arc { center_deg halfwidth_deg } {
        variable dial_arc_center
        variable dial_arc_halfwidth
        set dial_arc_center    $center_deg
        set dial_arc_halfwidth $halfwidth_deg
        dial_publish_geometry
    }

    proc dial_set_radius { r { tolerance {} } } {
        variable dial_radius
        variable dial_ring_tolerance
        set dial_radius $r
        if { $tolerance ne "" } { set dial_ring_tolerance $tolerance }
        dial_publish_geometry
    }

    # ---------------------------------------------------------------------
    # arming — the response window gate
    # ---------------------------------------------------------------------

    # Open the response window. Anything that arrived earlier — a swipe
    # commit, a touch — is not a response and will not be reported.
    #
    # This is the whole reason the gate lives here: both protocols previously
    # carried respwin_on_time, swipe_last_processed and touch_last_processed
    # and threaded them through every source by hand, and one of them forgot
    # the window check on one source. Now a protocol calls dial_arm and the
    # omission is unrepresentable.
    proc dial_arm {} {
        variable dial_armed_time
        variable dial_cursor_shown
        variable dial_last_sent
        variable dial_swipe_last
        variable dial_touch_last
        variable dial_report_angle
        variable dial_report_time
        variable dial_report_source

        set dial_armed_time    [now]
        set dial_cursor_shown  0
        set dial_last_sent     0.0
        set dial_report_angle  -1.0
        set dial_report_time   0
        set dial_report_source ""
        set dial_pending       ""

        # Consume whatever is already sitting in the transports, so a commit
        # that landed before the window cannot be picked up as the first
        # response after it.
        set dial_swipe_last [slider_swipe_time]
        set dial_touch_last [dial_touch_timestamp]
        # Only if the joystick is OURS to arm -- a protocol may be using it
        # for something else entirely, and joystick_reset clears its latch.
        variable dial_sources
        if { "joystick" in $dial_sources } { joystick_reset }
    }

    proc dial_disarm {} {
        variable dial_armed_time
        variable dial_cursor_shown
        set dial_armed_time 0
        if { $dial_cursor_shown } { dial_cursor_update 0 0 }
        set dial_cursor_shown 0
    }

    proc dial_armed {} {
        variable dial_armed_time
        return [expr {$dial_armed_time != 0}]
    }

    # ---------------------------------------------------------------------
    # live cursor
    # ---------------------------------------------------------------------

    # Publish "angle,show" as one comma-delimited token (no spaces) to the
    # cursor datapoint. The stim subscribes and applies it in its own frame
    # loop — no rmtSend round trip, no extra redraws, no vsync stall on the
    # state loop. This format is a contract with the stim side; both ported
    # protocols already used it, and owning it here keeps it uniform.
    proc dial_cursor_update { angle show } {
        variable dial_cursor_dpoint
        dservSet $dial_cursor_dpoint "$angle,$show"
    }

    # ---------------------------------------------------------------------
    # sources
    # ---------------------------------------------------------------------

    proc dial_touch_timestamp {} {
        if { ![dservExists ess/touch_press_deg] } { return 0 }
        return [dservTimestamp ess/touch_press_deg]
    }

    # Live-cursor observer, run on every slider sample (see dial_init).
    # Does NOT wake the state machine: a cursor moving is not an event the
    # experiment needs to make a decision about, and waking the SM at a 1 kHz
    # device's rate is the pathology this whole arrangement avoids. The SM
    # hears about the COMMIT, which is the event that matters.
    proc dial_slider_sample { dpoint data } {
        variable dial_active
        variable dial_armed_time
        variable dial_pi
        variable dial_deadband_deg
        variable dial_cursor_shown
        variable dial_last_sent

        if { !$dial_active || $dial_armed_time == 0 } return

        if { ![slider_swipe_engaged] } {
            if { $dial_cursor_shown } {
                set dial_cursor_shown 0
                dial_cursor_update 0 0
            }
            return
        }

        lassign $data x y
        if { $x eq "" || $y eq "" } return
        set angle [dial_clamp_arc [dial_norm2pi [expr {atan2($y, $x)}]]]
        set deadband [expr {$dial_deadband_deg*$dial_pi/180.0}]

        if { !$dial_cursor_shown } {
            set dial_cursor_shown 1
            dial_cursor_update $angle 1
            set dial_last_sent $angle
        } elseif { abs([dial_angdiff $angle $dial_last_sent]) > $deadband } {
            dial_cursor_update $angle 1
            set dial_last_sent $angle
        }
    }

    # Trackpad / joystick swipe. sliderconf owns engagement detection and
    # only publishes slider/swipe/angle on RELEASE once magnitude cleared the
    # threshold, so this reads engagement and commits rather than recomputing
    # either. Drives the live cursor as a side effect.
    proc dial_poll_swipe {} {
        variable dial_armed_time
        variable dial_swipe_last

        # Cursor tracking lives in dial_slider_sample now; this only reports
        # the commit sliderconf publishes on release.
        set t [slider_swipe_time]
        if { $t > $dial_armed_time && $t > $dial_swipe_last } {
            set dial_swipe_last $t
            # Clamp the commit exactly like the cursor, so what is scored is
            # what the subject last saw.
            return [dial_clamp_arc [slider_swipe_angle]]
        }
        return ""
    }

    # Touchscreen: a press landing within the ring band commits its angle.
    #
    # Note the deliberate asymmetry with the cursor. An out-of-arc SWIPE is
    # clamped, because the subject is steering and sees where they are. An
    # out-of-arc TOUCH is REJECTED, because snapping a press at 12:00 round to
    # 2:00 would commit an answer the subject never chose. Same rule, opposite
    # handling, and the difference is intentional.
    proc dial_poll_touch {} {
        variable dial_armed_time
        variable dial_touch_last
        variable dial_radius
        variable dial_ring_tolerance

        set t [dial_touch_timestamp]
        if { $t == 0 } { return "" }
        if { $t <= $dial_touch_last } { return "" }
        if { $t <= $dial_armed_time } { return "" }
        set dial_touch_last $t

        lassign [dservGet ess/touch_press_deg] tx ty
        if { $tx eq "" || $ty eq "" } { return "" }

        set dist [expr {sqrt($tx*$tx + $ty*$ty)}]
        if { $dist < [expr {$dial_radius - $dial_ring_tolerance}] ||
             $dist > [expr {$dial_radius + $dial_ring_tolerance}] } { return "" }

        set angle [dial_norm2pi [expr {atan2($ty, $tx)}]]
        if { ![dial_in_arc $angle] } { return "" }
        return $angle
    }

    # 8-way joystick: a settled deflection reports its sector's direction.
    #
    # Sectors are 0-7 clockwise from up, so sector k points at 90-45k degrees.
    # This is a COARSE dial -- 45 degrees of resolution -- which is exactly
    # right for a task reporting a direction and wrong for one reporting a
    # precise bearing. Use it where the joystick is the rig's only pointing
    # device, or as a fallback alongside swipe.
    #
    # Treated like touch, not like swipe: a deflection is a discrete commit
    # rather than steering, so an out-of-arc sector is REJECTED rather than
    # snapped. Snapping would commit a direction the subject did not choose.
    #
    # joystick_response is a latch armed by joystick_reset (done in dial_arm),
    # so the window gate is the latch itself -- there is no stale value to
    # consume.
    proc dial_poll_joystick {} {
        variable dial_pi
        set sector [joystick_response]
        if { $sector < 0 } { return "" }
        set angle [dial_norm2pi [expr {(90.0 - 45.0*$sector)*$dial_pi/180.0}]]
        if { ![dial_in_arc $angle] } { return "" }
        return $angle
    }

    # ---------------------------------------------------------------------
    # the call protocols make
    # ---------------------------------------------------------------------

    # Update the live cursor and report a commit.
    #
    # Returns the committed angle in radians [0, 2pi), or -1 for no response.
    # -1 is unambiguous because a real angle is never negative.
    #
    # Call this from the response state's transition, as protocols already
    # called responded_swipe. Cheap when nothing has happened.
    proc dial_response {} {
        variable dial_active
        variable dial_armed_time
        variable dial_sources
        variable dial_report_angle
        variable dial_report_time
        variable dial_report_source
        variable dial_pending

        if { !$dial_active || $dial_armed_time == 0 } { return -1 }

        # A simulated commit is consumed here like any other source, so it
        # travels the identical path into the protocol.
        if { $dial_pending ne "" } {
            set dial_report_angle  $dial_pending
            set dial_report_time   [now]
            set dial_report_source simulate
            set dial_pending       ""
            set dial_armed_time    0
            return $dial_report_angle
        }

        foreach src $dial_sources {
            set a [dial_poll_$src]
            if { $a ne "" } {
                set dial_report_angle  $a
                set dial_report_time   [now]
                set dial_report_source $src
                # Stop accepting, but leave the cursor ALONE. Whether a
                # committed response should stay visible is a protocol
                # choice -- ricochet lets the swipe release clear it,
                # mp_pulsed shows the touched angle as confirmation -- and
                # the dial has no business deciding it. Use dial_show /
                # dial_hide to say which you want.
                set dial_armed_time 0
                return $a
            }
        }
        return -1
    }

    # Last committed angle (radians) and how it arrived. Useful in scoring
    # methods that run after responded has already returned.
    # Explicit cursor control, for protocols that want the committed angle
    # to stay on screen as confirmation (or to clear it immediately).
    proc dial_show { angle } { dial_cursor_update $angle 1 }
    proc dial_hide {}        { variable dial_cursor_shown
                               set dial_cursor_shown 0
                               dial_cursor_update 0 0 }

    proc dial_angle  {} { variable dial_report_angle;  return $dial_report_angle }
    proc dial_time   {} { variable dial_report_time;   return $dial_report_time }
    proc dial_source {} { variable dial_report_source; return $dial_report_source }

    # Inject a commit, for headless tests and pre-flight checks on a rig with
    # no subject present. Mirrors ::ess::joystick_simulate / button_simulate.
    # Goes through the same arc rules as a real commit so a simulated response
    # cannot land somewhere a real one could not.
    proc dial_simulate { angle_rad } {
        variable dial_active
        variable dial_armed_time
        if { !$dial_active } { error "::ess::dial_simulate: no dial initialised" }
        if { $dial_armed_time == 0 } { error "::ess::dial_simulate: dial not armed" }
        # Latch it and wake the state machine; dial_response consumes it on
        # the next update. Applying it here instead would set the report and
        # then make it unreachable -- dial_response returns -1 once the dial
        # is disarmed, so the simulated angle never reached the protocol.
        set a [dial_clamp_arc [dial_norm2pi $angle_rad]]
        variable dial_pending; set dial_pending $a
        do_update
        return $a
    }
}

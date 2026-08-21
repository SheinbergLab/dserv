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
# Sources: swipe | touch | joystick | mouse | stick | dpad | astick.
# Three of them read an analog stick and they are NOT interchangeable:
#   stick   deflection rotates a bearing; cursor pinned to the ring. 1-D.
#   dpad    deflection quantized to 8 sectors; cursor walks out a spoke.
#   astick  deflection IS the cursor's velocity; continuous, speed-graded.
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
    # A commit waiting to be consumed by the next dial_response, as
    # {angle source}. Used by the sources that arrive ASYNCHRONOUSLY (the
    # mouse observer, dial_simulate) rather than being polled.
    #
    # It carries its source rather than assuming one: a real mouse click
    # and an operator's simulated click both land here, and conflating
    # them would make a simulated trial indistinguishable from a subject's
    # response in the data.
    #
    # It must NOT be applied directly -- dial_response is where a commit
    # becomes the protocol's response, and short-circuiting that made the
    # angle unreachable (dial_response returns -1 once disarmed).
    variable dial_pending        ""

    # Mouse source. The mouse is a POINTING device, so it reports the way
    # the touchscreen does -- a free cursor the subject places anywhere,
    # and a click that lands on the reportable arc commits the angle it
    # points at. It is deliberately NOT the swipe's steering idiom.
    #
    # That is the touch side of this file's central asymmetry (see
    # dial_poll_touch): steering CLAMPS an out-of-arc angle because the
    # subject is watching a cursor and sees where they are; a discrete
    # press REJECTS one, because snapping a click at 12:00 round to 2:00
    # commits an answer nobody chose. A placed dot is a press, so it
    # rejects.
    #
    # The interaction is move-to-point, click-to-select, with no button
    # held while adjusting -- what a mouse is good at, and the same
    # instruction the operator's simulated dial already obeys, which is
    # the cheapest thing to explain to a human subject: "click where you
    # think it is".
    #
    # It is also the version with the LEAST machinery. Reading only an
    # angle put a singularity at the center, and every mouse-specific
    # workaround in the history of this file -- the start-radius acquire
    # gate, the acquired latch, holding the last angle through the dead
    # zone -- existed to paper over it. A dot that is simply where the
    # mouse is has no dead zone, so none of it is representable.
    # Rig-level source binding, set in local/ and NOT cleared by
    # input_reset -- bindings describe the rig and persist across systems,
    # exactly as button_bind / joystick_bind do. Empty = unbound.
    variable dial_bound_sources  {}
    # Where dial_sources came from: protocol | rig | default. Published so
    # the operator can see WHY a dial is answering the way it is.
    variable dial_source_origin  default

    # Reaching the ring band (dial_radius +/- dial_ring_tolerance, inside the
    # arc) LOCKS ON, once per response window. After that the radius stops
    # mattering and only the angle does.
    #
    # The lock is not a convenience, it is what makes the gesture possible.
    # Sweeping along an arc, a hand travels the CHORD, not the curve: sliding
    # left to right across the bottom of the arc cuts inside the band and the
    # report kept dropping out and reverting to a bare cursor mid-gesture.
    # Requiring the subject to trace a circle accurately is a motor task the
    # experiment is not trying to measure.
    #
    # So: get to the ring once to engage, then sweep freely and commit.
    #
    # Two things are deliberately NOT relaxed by the lock:
    #
    #   the ARC still applies -- leaving the reportable wedge is a real
    #   "no answer here", not a shortcut, so it drops the lock's benefit and
    #   shows the plain cursor again
    #
    #   the same in_band drives BOTH the drawing and the acceptance, so a
    #   catcher on screen always means a click will be taken. Latching only
    #   the display would promise an answer the dial then refuses.
    #
    # Note what this costs: a locked-on click is accepted at a radius a TOUCH
    # would reject, so mouse and touch are no longer the same rule. That is
    # the price of the gesture, and it is one-way -- everything a touch
    # accepts, the mouse accepts too.
    #
    # --- stick source (velocity steering) --------------------------------
    #
    # Direct pointing with a thumbstick was tried first and was HARDER than
    # the trackpad, which is what started this whole line of work. The
    # reasons are structural, not tuning: a stick has ~1cm of travel, so
    # atan2 of the deflection is twitchy near center and only steady at
    # full push -- precision depends on how hard you hold it -- and it
    # self-centers, so releasing loses the direction and you can never park
    # one and refine it.
    #
    # Integrating instead inverts all three: resolution comes from how LONG
    # you hold rather than how far you push, a small deflection is a slow
    # and therefore fine adjustment, and the cursor stays where it was left.
    #
    # Horizontal deflection drives rotation, deliberately: reaching for a
    # left/right sweep to move round a circle is what people actually do --
    # David did it with the mouse before this existed.
    variable dial_stick_rate     180.0   ;# deg/s at full deflection
    variable dial_stick_deadzone 0.08    ;# fraction of full scale
    # Response curve: rate = full * f^expo, f being deflection past the
    # deadzone rescaled to 0..1. 1.0 is linear; 2-3 gives a slow, fine
    # region near center and full speed still available at the edge, which
    # is what a single knob for "slow gain" and "fast gain" really wants.
    variable dial_stick_expo     2.0
    variable dial_stick_invert   0
    variable dial_stick_commit_dp "extio/*/state/group/stick_select"
    variable dial_stick_scale    0.0     ;# full-scale deflection, learned
    # Floor below which a deflection is not evidence of full travel. Guards
    # the learner against defining full scale from an uncalibrated stick's
    # resting offset. Set -stick_min_scale above the resting magnitude your
    # rig actually shows.
    variable dial_stick_min_scale 1.0
    variable dial_stick_last_ts  0
    variable dial_stick_angle    0.0
    variable dial_stick_moved    0

    variable dial_mouse_range_known 0
    variable dial_mouse_cx 0.0
    variable dial_mouse_cy 0.0

    # Pixels-to-degrees for the mouse's declared extent. An angle is
    # scale-free, so the old angle-only reading needed only the center; a
    # PLACED dot needs a scale, because now the dot's distance from the
    # center is a real quantity that has to agree with the arc the stim
    # draws. Derived in dial_mouse_range from the extent and ::ess's screen
    # degrees, so a rig that declares its mouse extent correctly in
    # inputconf.tcl gets the mapping for free.
    variable dial_mouse_dpp_x    0.0
    variable dial_mouse_dpp_y    0.0

    # Fraction of the screen the full mouse extent covers. 1.0 means a
    # sweep across the whole extent crosses the whole screen. Below 1.0
    # trades reach for precision -- the dot moves less per count -- which
    # is the knob to turn if subjects overshoot the arc.
    variable dial_mouse_scale    1.0

    # Where the dot is, in degrees: "x,y,show,in_band".
    #
    # The dial's ONE output, written by every source. A steering source
    # places its cursor on the ring; a pointing source places it where the
    # hand is. Either way a consumer draws this and nothing else.
    #
    # It replaced ess/cursor's "angle_rad,show". Carrying both meant every
    # stim needed a branch to decide which was authoritative, and getting
    # that precedence wrong hid the catcher on the exact frame the mouse
    # wanted it drawn.
    #
    # One publication per sample also keeps the dial off a hazard in the
    # dserv->stim path: two datapoints published back-to-back with no work
    # between them could lose the first (a stim2 defect, since fixed), which
    # a dual publish hit on every sample at mouse rate.
    #
    # in_band is the affordance, and it is not optional. A press that misses
    # is silently ignored (it must be -- see above), so without a visible
    # "this will accept" state the subject clicks and nothing happens, with
    # nothing on screen to say why. The stim brightens the ring on it.
    variable dial_pointer_dpoint ess/dial/pointer
    variable dial_pointer_shown  0
    variable dial_pointer_band   -1
    variable dial_pointer_x      0.0
    variable dial_pointer_y      0.0

    # Positional deadband for republishing the dot, in degrees. The reader
    # already decimates motion, so this is only insurance against a fast
    # mouse; it never gates a CHANGE OF BAND, which must be crisp.
    variable dial_pointer_deadband 0.05

    # Has the subject reached the ring band yet this response window? Set by
    # the first on-ring sample, cleared by dial_arm. Per-trial state, not
    # configuration.
    variable dial_mouse_locked   0

    # --- dpad source (travelling cursor from a 4-switch d-pad) -----------
    #
    # The SAME four switches as the joystick source, read differently, so
    # dial_init refuses both at once -- the swipe/stick precedent above.
    #
    #   joystick   a settled deflection REPORTS its sector. Discrete, no
    #              cursor, nothing in between.
    #   dpad       holding a direction walks a cursor OUT along that spoke
    #              from the center; reaching the ring is the response.
    #
    # Built for teaching an animal to use a joystick, where the discrete
    # version gives nothing to learn from: push, and either it counted or
    # it did not. A cursor travelling while the switch is held makes the
    # contingency visible the whole way, which is what shaping needs.
    #
    # It is the only source whose INPUT is constant while its OUTPUT must
    # keep changing. The mouse's events are its own clock and the analog
    # stick streams samples even at a fixed deflection, but four switches
    # held down produce exactly one event and then silence -- so this is
    # the one source that carries a timer (see dial_dpad_tick).
    #
    # Position, not just an angle, so it rides ess/dial/pointer like the
    # mouse and inherits the whole display: the dot, the switch to the
    # catcher at the band, the in_band affordance. A stim that already
    # draws a mouse dial draws this one with no changes at all.
    variable dial_dpad_rate      8.0    ;# deg of travel per second held
    # How finely the walking cursor is DRAWN, in ms. Not how fast it travels:
    # dial_dpad_tick integrates rate*dt from [now], so changing this changes
    # smoothness and nothing else.
    #
    # Was 16 ("about a frame") before anything measured it, and 60 Hz is
    # precisely the wrong neighbour to sit next to: 1000/16 = 62.5 Hz beats
    # against a 60 Hz display at 2.5 Hz, so roughly every 400 ms one frame
    # gets two updates or none. That reads as a periodic hitch rather than as
    # uniform motion, and it is the milder twin of a bug this file already
    # had once -- astick shipped a 16 ms publish gate, measured 50 Hz, and
    # juddered for exactly the same reason.
    #
    # 8 ms (125 Hz) clears any display rate, so every frame draws a fresh
    # position. The cost is 125 timer wakeups a second instead of 62.5 and a
    # ~30-byte datapoint at 125 Hz -- negligible beside the analog stream a
    # stick-driven dpad is already carrying.
    #
    # It also halves the quantization on WHEN the cursor reached the ring,
    # since the commit check runs per tick, so arrival RT gets sharper too.
    variable dial_dpad_tick_ms   8      ;# 125 Hz, above any display rate
    # ring: reaching the band IS the response. Held apart from the travel
    # so the criterion can be sharpened later -- a dwell rule ("stop on the
    # target and stay there") changes only this switch and dial_dpad_tick's
    # last branch, not the movement.
    variable dial_dpad_commit    ring   ;# ring | none

    # Acceleration on a sustained hold, the digital-clock idiom: the longer
    # the direction is held, the faster the cursor travels.
    #
    #   rate(t) = min(rate + accel*t, rate_max)      t = seconds held
    #
    # 0 accel is a constant rate and is the default, because acceleration
    # is not free here: with commit "ring" the time from first deflection
    # to the report IS the reaction time, and a rate that changes under the
    # animal makes that a nonlinear function of hold duration rather than a
    # measurement. Worth having for a long travel or an impatient subject;
    # worth knowing what it costs the dependent measure.
    variable dial_dpad_accel     0.0    ;# extra deg/s for every second held
    variable dial_dpad_rate_max  0.0    ;# cap; 0 = uncapped

    # per-response state
    variable dial_dpad_r         0.0    ;# how far out, in degrees
    variable dial_dpad_angle     -1.0   ;# current spoke, radians; -1 = none
    variable dial_dpad_timer     ""     ;# dservAfter id, "" = not running
    variable dial_dpad_last_us   0      ;# previous tick, for dt
    variable dial_dpad_hold_us   0      ;# start of this hold, for accel
    variable dial_dpad_deflected 0      ;# is a direction held right now?
    # The spoke the cursor is ON, and the spoke the stick is ASKING for, as
    # sectors (0..7, -1 none). Kept as integers so "same spoke?" is an exact
    # comparison rather than a float epsilon.
    variable dial_dpad_sector    -1
    variable dial_dpad_want      -1

    # Which of the eight directions are LIVE this trial. Empty = all of
    # them, which is the default and the whole-circle case.
    #
    # This is the discrete twin of the arc: the arc says which directions
    # are reportable when the report is continuous, this says it when the
    # device has eight. A reduced target set is rarely contiguous --
    # {0 2 4 6} cardinal, {1 3 5 7} oblique -- so an arc cannot express it,
    # which is why the arc stays the full circle and this carries the
    # restriction instead.
    #
    # A direction that is not live behaves exactly as CENTERED: the cursor
    # does not move. It is not rejected at the end of a reach, it simply
    # never sets out, so an animal on the reduced rungs of the ladder gets
    # no travel at all for a direction that was never on offer.
    variable dial_dpad_sectors   {}
    # Walking back to the center after a rejected reach. See dial_rearm:
    # the cursor is never teleported, so the animal's own release is what
    # resets it.
    variable dial_dpad_homing    0

    # --- astick source (rate control from an analog stick) ----------------
    #
    # The third reading of a stick in this file, and the distinctions are
    # worth stating because all three get called "the analog stick":
    #
    #   stick    horizontal deflection rotates a BEARING at a rate; the
    #            cursor is pinned to the ring. One-dimensional -- it reads
    #            y and ignores it.
    #   dpad     the deflection is quantized to one of eight SECTORS (by
    #            ::ess::joystick's analog transport) and the cursor walks
    #            out along that spoke at a fixed rate.
    #   astick   the deflection vector IS the cursor's velocity. Direction
    #            continuous, speed proportional to how far you push.
    #
    # What rate control buys over the dpad is that speed becomes the
    # subject's to control -- a hard push crosses the field, a gentle one
    # trims -- and that the bearing is not confined to eight spokes.
    #
    # What it costs is the spoke itself, and that cost is specific: the
    # dpad's dial_dpad_sectors ("only these directions are live this trial",
    # the mechanism behind the reduced rungs of a shaping ladder) is
    # meaningful only because the cursor can never be anywhere BUT on a
    # spoke. With a free 2D velocity an animal can simply curve around an
    # off-list direction, so there is deliberately no dial_astick_sectors.
    # A protocol that needs that gating wants the dpad.
    #
    # NO TIMER, unlike the dpad. That is not an oversight and not a
    # simplification -- it is the whole reason this source is shorter. The
    # dpad carries a dservAfter tick because four switches emit one event
    # and then fall silent, so something has to keep the cursor moving. An
    # analog stick streams at the box's sample rate whether or not it is
    # moving, and every sample carries the box's own timestamp -- so the
    # integration rides the samples, and its dt comes off a PTP-disciplined
    # grid rather than a ~60 Hz Tcl timer that drifts with scheduler load.
    #
    # Holding on release survives, which was the surprise: a spring stick
    # returning to center means velocity zero, so the cursor STOPS where it
    # got to rather than snapping home. Progress made in a short push is
    # kept, which is the property the dpad was built around for shaping.
    variable dial_astick_rate     8.0   ;# deg of travel per second, full push
    variable dial_astick_deadzone 0.08  ;# fraction of full scale
    # Response curve applied to SPEED only, never per-axis. Shaping x and y
    # separately would bend the path: with expo 2, a push at 45 degrees has
    # both components squared, which preserves the diagonal but NOT any
    # other bearing -- the cursor would bow away from the direction the hand
    # is actually pointing. Magnitude in, direction untouched.
    variable dial_astick_expo     2.0
    # Full-scale deflection in slider output units. NO default that pretends
    # to be right and no learning from the largest sample seen: a learned
    # scale means the speed changes as the subject pushes harder over a
    # session, and the resting offset of an uncalibrated stick once defined
    # full deflection outright (see dial_stick_min_scale). dial_arm errors
    # if this is unset, the same way it errors on a missing radius.
    variable dial_astick_scale    0.0
    variable dial_astick_commit   ring  ;# ring | none
    # Per-response state: where the cursor is, in degrees, and when we last
    # integrated.
    variable dial_astick_x        0.0
    variable dial_astick_y        0.0
    variable dial_astick_last_ts  0
    variable dial_astick_moved    0
    # Arrived and committed. Cleared by dial_arm / dial_rearm. See the
    # commit branch: without it this source has no way to stop, because
    # unlike the dpad it has no timer whose cancellation ends the reach.
    variable dial_astick_committed 0
    # Optional throttle on POINTER publishes, in ms. 0 = OFF, and off is the
    # right default. Set it only if a rig has a specific reason to cap the
    # traffic, and then check the arithmetic below first.
    #
    # This defaulted to 16 for one revision, on the reasoning that 200
    # updates a second into a 60 Hz display is two thirds of them discarded.
    # True about bandwidth, BACKWARDS about smoothness, and measurably so:
    # publishing faster than the display only costs bytes, while publishing
    # slower than it guarantees repeated frames, which is visible judder.
    #
    # Worse, the achievable rates are QUANTIZED. Samples arrive on the box's
    # grid (5 ms at 200 Hz), so a gate of N ms actually yields 200/ceil(N/5)
    # Hz -- 200, 100, 66.7, 50, 40 and nothing in between. 16 ms does not
    # give 62.5 Hz, it gives 50: under the display, and juddering.
    #
    # The positional deadband is the honest limiter, because it asks the
    # right question -- would the drawn picture actually change -- rather
    # than a clock's. See dial_astick_min_step.
    variable dial_astick_pub_ms   0
    variable dial_astick_last_pub 0
    # Movement below this is not worth a publish, in degrees. Much smaller
    # than dial_pointer_deadband (0.05), which is sized as insurance against
    # a 1 kHz mouse and is far too coarse here: at a slow 3 deg/s it holds
    # publishes back to 50 Hz -- under the display again, for the very
    # motion where smoothness is most visible. This source is already capped
    # by the sample rate, so it only needs to skip a cursor that is not
    # moving at all.
    variable dial_astick_min_step 0.002
    # Largest gap this will integrate across, in seconds. A gap longer than
    # this is capped, not skipped -- see the sample handler for why. 0.1 is
    # comfortably above a healthy 0.005 and well under the 0.175 stalls the
    # teensy box was measured producing.
    variable dial_astick_max_dt   0.1
    # Diagnostics, per response window. Cleared by dial_arm; readable via
    # dial_astick_tune with no arguments. These count things done to this
    # source by its input, not by the subject, and having them on the rig is
    # what turns "the cursor feels wrong" into a number.
    variable dial_astick_n_dup    0
    variable dial_astick_n_gap    0
    # Set by dial_rearm when a rejected reach left the cursor in the band.
    # While set, arriving does not commit -- the cursor must first come back
    # inside the band. See dial_rearm for why astick cannot use homing.
    variable dial_astick_reapproach 0

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
    # excluded wedge's centerline flips which end you snap to, which is fine:
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

    # ::ess::dial_bind ?sources?
    #
    # Declare which transports this RIG answers a dial with; returns the
    # current binding when called with no argument.
    #
    # This is the rig's business, not the protocol's. A subject reports
    # with a trackpad on one rig and a dedicated mouse on another, and
    # nothing about the experiment changes -- so a protocol that names its
    # hardware needs a different copy per rig, which is the thing local/
    # exists to prevent. Same split as button_bind / joystick_bind:
    # binding is the rig's, activation (dial_init) is the system's.
    #
    #   # local/input.tcl on a rig with a dedicated mouse
    #   ::ess::dial_bind {mouse touch}
    #
    # A protocol may still pass -sources explicitly when the response mode
    # genuinely requires a particular transport; that wins over the binding.
    proc dial_bind { args } {
        variable dial_bound_sources
        if { [llength $args] == 0 } { return $dial_bound_sources }
        set dial_bound_sources [lindex $args 0]
        dservSet ess/dial/bound $dial_bound_sources
        return $dial_bound_sources
    }

    # ::ess::dial_init ?-sources {swipe touch}?
    #                  ?-deadband_deg N?
    #                  ?-arc_center DEG? ?-arc_halfwidth DEG?
    #                  ?-radius R? ?-ring_tolerance T?
    #                  ?-cursor_dpoint NAME?
    #
    # Does NOT initialize the underlying transport — the protocol still calls
    # ::ess::slider_init / ::ess::touch_init with the settings it wants (swipe
    # threshold, release behavior). The dial reads them; it does not own them.
    proc dial_init { args } {
        variable dial_active
        variable dial_sources
        variable dial_deadband_deg
        variable dial_arc_center
        variable dial_arc_halfwidth
        variable dial_radius
        variable dial_ring_tolerance
        # Every namespace variable this proc assigns MUST be declared here.
        # Without the declaration `set` makes a LOCAL, the namespace value
        # keeps its default, and the option is silently ignored -- which is
        # exactly what happened to -start_radius and every -stick_* option:
        # they appeared to work only because the defaults were the values
        # being tested with.
        variable dial_mouse_scale
        variable dial_dpad_rate
        variable dial_dpad_accel
        variable dial_dpad_rate_max
        variable dial_dpad_tick_ms
        variable dial_dpad_commit
        variable dial_pointer_dpoint
        variable dial_stick_rate
        variable dial_stick_deadzone
        variable dial_stick_expo
        variable dial_stick_min_scale
        variable dial_stick_invert
        variable dial_stick_commit_dp
        variable dial_astick_rate
        variable dial_astick_deadzone
        variable dial_astick_expo
        variable dial_astick_scale
        variable dial_astick_commit
        variable dial_astick_pub_ms
        variable dial_astick_min_step

        # defaults on every init, so a protocol that omits an option gets
        # the documented value rather than whatever the previous protocol
        # set. Sources default to the RIG's binding when it has one, so a
        # protocol need not -- and should not -- name hardware.
        variable dial_bound_sources
        variable dial_source_origin
        if { [llength $dial_bound_sources] } {
            set dial_sources       $dial_bound_sources
            set dial_source_origin rig
        } else {
            set dial_sources       {swipe touch}
            set dial_source_origin default
        }
        set dial_deadband_deg   2.0
        set dial_arc_center     0.0
        set dial_arc_halfwidth  180.0
        set dial_radius         0.0
        set dial_ring_tolerance 0.0
        set dial_mouse_scale      1.0
        set dial_dpad_rate        8.0
        set dial_dpad_accel       0.0
        set dial_dpad_rate_max    0.0
        set dial_dpad_tick_ms     8     ;# see the variable: 16 beat with 60 Hz
        set dial_dpad_commit      ring
        set dial_stick_rate     180.0
        set dial_stick_deadzone 0.08
        set dial_stick_expo     2.0
        set dial_stick_min_scale 1.0
        set dial_astick_rate      8.0
        set dial_astick_deadzone  0.08
        set dial_astick_expo      2.0
        # 0 = unset. dial_arm refuses rather than sitting motionless.
        set dial_astick_scale     0.0
        set dial_astick_commit    ring
        # 0 = no throttle, which is the right default: see the variable's
        # own comment for why a clock-based cap made the cursor judder.
        set dial_astick_pub_ms    0
        set dial_astick_min_step  0.002
        set dial_stick_invert   0
        set dial_stick_commit_dp "extio/*/state/group/stick_select"
        set dial_pointer_dpoint ess/dial/pointer

        foreach { k v } $args {
            switch -- $k {
                -sources        { set dial_sources $v
                                  set dial_source_origin protocol }
                -deadband_deg   { set dial_deadband_deg $v }
                -arc_center     { set dial_arc_center $v }
                -arc_halfwidth  { set dial_arc_halfwidth $v }
                -radius         { set dial_radius $v }
                -ring_tolerance { set dial_ring_tolerance $v }
                -mouse_scale    { set dial_mouse_scale $v }
                -dpad_rate      { set dial_dpad_rate $v }
                -dpad_accel     { set dial_dpad_accel $v }
                -dpad_rate_max  { set dial_dpad_rate_max $v }
                -dpad_tick_ms   { set dial_dpad_tick_ms $v }
                -dpad_commit    { set dial_dpad_commit $v }
                -pointer_dpoint { set dial_pointer_dpoint $v }
                # Errors rather than being accepted and ignored. It named a
                # radius in MOUSE PIXELS, gating a start the free dot no
                # longer has. Silently reinterpreting it as ring geometry
                # would leave a call site reading fine and behaving wrongly,
                # which is the failure this file has already had once.
                -start_radius   { error "::ess::dial_init: -start_radius is\
                                         gone with the angle-only mouse; use\
                                         -ring_tolerance" }
                -stick_rate     { set dial_stick_rate $v }
                -stick_deadzone { set dial_stick_deadzone $v }
                -stick_expo     { set dial_stick_expo $v }
                -stick_min_scale { set dial_stick_min_scale $v }
                -stick_invert   { set dial_stick_invert [expr {$v ? 1 : 0}] }
                -stick_commit   { set dial_stick_commit_dp $v }
                -astick_rate     { set dial_astick_rate $v }
                -astick_deadzone { set dial_astick_deadzone $v }
                -astick_expo     { set dial_astick_expo $v }
                -astick_scale    { set dial_astick_scale $v }
                -astick_commit   { set dial_astick_commit $v }
                -astick_pub_ms   { set dial_astick_pub_ms [expr {int($v)}] }
                -astick_min_step { set dial_astick_min_step [expr {double($v)}] }
                # ess/cursor is no longer written by any source; the one
                # output is ess/dial/pointer (-pointer_dpoint).
                -cursor_dpoint  { error "::ess::dial_init: -cursor_dpoint is\
                                         gone with ess/cursor; the dial\
                                         publishes ess/dial/pointer, set with\
                                         -pointer_dpoint" }
                default { error "::ess::dial_init: unknown option '$k'" }
            }
        }

        # swipe, stick and astick are three readings of ONE slider/position,
        # and any two of them would fight over the same cursor. dpad joins
        # the exclusion because on an analog rig its sectors are DERIVED
        # from that same stick (::ess::joystick's analog transport), so
        # dpad+astick is one hand driving the cursor two ways at once --
        # which reads on screen as a cursor that will not track.
        set steering [lsearch -all -inline -regexp $dial_sources \
                          {^(swipe|stick|astick|dpad)$}]
        if { [llength $steering] > 1 } {
            error "::ess::dial_init: {$steering} are alternative readings of\
                   the same device -- choose one"
        }
        # Both read the same four switches: one reports a settled sector,
        # the other walks a cursor out along it.
        if { "joystick" in $dial_sources && "dpad" in $dial_sources } {
            error "::ess::dial_init: joystick and dpad are alternative\
                   readings of the same switches -- choose one"
        }

        foreach s $dial_sources {
            if { $s ni {swipe touch joystick mouse stick dpad astick} } {
                error "::ess::dial_init: unknown source '$s'\
                       (want swipe|touch|joystick|mouse|stick|dpad|astick)"
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
        if { "stick" in $dial_sources } {
            variable dial_stick_scale
            set dial_stick_scale 0.0
            dservAddExactMatch slider/position
            dpointAddScript    slider/position ::ess::dial_stick_sample
            dservAddMatch      $dial_stick_commit_dp
            dpointAddScript    $dial_stick_commit_dp ::ess::dial_stick_commit
        }

        if { "astick" in $dial_sources } {
            variable dial_astick_x;       set dial_astick_x       0.0
            variable dial_astick_y;       set dial_astick_y       0.0
            variable dial_astick_last_ts; set dial_astick_last_ts 0
            variable dial_astick_moved;   set dial_astick_moved   0
            variable dial_astick_committed;  set dial_astick_committed  0
            variable dial_astick_reapproach; set dial_astick_reapproach 0
            variable dial_astick_last_pub;   set dial_astick_last_pub   0
            dservAddExactMatch slider/position
            dpointAddScript    slider/position ::ess::dial_astick_sample
        }

        if { "dpad" in $dial_sources } {
            variable dial_dpad_r;      set dial_dpad_r      0.0
            variable dial_dpad_angle;  set dial_dpad_angle  -1.0
            variable dial_dpad_sector; set dial_dpad_sector -1
            variable dial_dpad_want;   set dial_dpad_want   -1
            dial_dpad_stop
            dservAddExactMatch ess/joystick/dir
            dpointAddScript    ess/joystick/dir ::ess::dial_dpad_dir
        }

        if { "mouse" in $dial_sources } {
            variable dial_mouse_range_known
            set dial_mouse_range_known 0
            dservAddExactMatch mouse/event/range
            dpointAddScript    mouse/event/range ::ess::dial_mouse_range
            dservAddExactMatch mouse/event
            dpointAddScript    mouse/event ::ess::dial_mouse_sample

            # SEED the range from its current value. Subscriptions fire on
            # CHANGE only, and mouse/event/range is published once when the
            # reader starts -- at boot, long before any system loads. So
            # subscribing alone leaves dial_mouse_range_known 0 forever,
            # and dial_mouse_sample then drops every sample on its second
            # line: no cursor, no commit, and nothing on screen to say why.
            #
            # The device republishes on reconnect, so the subscription is
            # still needed; this only covers the already-running case.
            if { [dservExists mouse/event/range] } {
                catch {
                    dial_mouse_range mouse/event/range \
                        [dservGet mouse/event/range]
                }
            }
        }

        # Assert the pointer as hidden, for the same reason dial_deinit
        # asserts the gate: a subscriber that already holds a stale dot
        # cannot tell "absent" from "not shown", and a leftover dot from a
        # previous system is worse than no dot at all.
        dial_pointer_hide

        dial_disarm
        set dial_active 1
        dservSet ess/dial_active 1
        dial_publish_sources
        dial_publish_geometry
    }

    # Always publishes the gate, even when no dial was active.
    #
    # It used to return early in that case, which broke the panel across a
    # dserv restart: a fresh interp has dial_active 0, so input_reset's
    # dial_deinit published NOTHING, ess/dial_active did not exist, and the
    # page's dservTouch had nothing to republish -- so a Dial panel left
    # over from the previous system stayed on screen under a system with no
    # dial at all.
    #
    # A gate datapoint has to be ASSERTED, not merely left unset: absent
    # and false look identical to the publisher and completely different to
    # a subscriber that already has a stale value. The other input tools
    # (button, joystick, slider) already publish theirs unconditionally.
    #
    # Every step below is idempotent, so none of it needs guarding.
    proc dial_deinit {} {
        variable dial_active
        # Remove only OUR scripts; slider_process keeps its registration.
        catch { dpointRemoveScript slider/position ::ess::dial_slider_sample }
        catch { dpointRemoveScript slider/position ::ess::dial_stick_sample }
        catch { dpointRemoveScript slider/position ::ess::dial_astick_sample }
        variable dial_stick_commit_dp
        catch { dpointRemoveScript $dial_stick_commit_dp ::ess::dial_stick_commit }
        catch { dpointRemoveScript ess/joystick/dir ::ess::dial_dpad_dir }
        catch { dial_dpad_stop }
        catch { dpointRemoveScript mouse/event ::ess::dial_mouse_sample }
        catch { dpointRemoveScript mouse/event/range ::ess::dial_mouse_range }
        dial_disarm
        set dial_active 0
        dservSet ess/dial_active 0
        # Clear the companions too, so the panel cannot show a previous
        # system's sources or arc if it is ever displayed again.
        dservSet ess/dial/sources {}
        dservSet ess/dial/source_origin {}
        # The BINDING is a rig property and outlives any system, so it is
        # re-asserted rather than cleared -- the panel should still be able
        # to say what this rig answers dials with when none is loaded.
        variable dial_bound_sources
        dservSet ess/dial/bound $dial_bound_sources
        dial_pointer_hide
    }

    # Publish the dial's geometry so anything outside ess can DRAW it.
    #
    # The viz subprocess has no ess package -- it can only read datapoints
    # and stimdg -- so a visualization cannot ask the dial anything. Making
    # the dial self-describing is what lets one shared viz serve every
    # system that uses a dial, instead of each hand-rolling the ring, the
    # arc and the cursor as ricochet, mp_pulsed and motionpatch each do now.
    #
    # ess/dial/geometry : "arc_center_deg,arc_halfwidth_deg,radius"
    # ess/dial/pointer  : "x_deg,y_deg,show,in_band"   (the live cursor)
    #
    # A stim draws ess/dial/pointer and needs to know nothing about which
    # transport is feeding it: a swipe or a stick puts the cursor on the
    # ring, a mouse or a d-pad puts it where the hand is, and the drawing
    # is identical. That is what keeps "who answers this dial" a rig
    # decision (dial_bind) rather than something a protocol encodes -- and
    # it is why there is one datapoint here rather than two.
    # Which sources this dial is listening to.
    #
    # Published because the dial's input is otherwise invisible: with
    # -sources {swipe} the transport is the slider, so the operator ends up
    # on the SLIDER panel to manage a DIAL, and with -sources {mouse} there
    # is no slider panel at all and nothing says why. Stating it on the dial
    # makes the slider panel legible as the swipe source's transport rather
    # than a mystery.
    proc dial_publish_sources {} {
        variable dial_sources
        variable dial_source_origin
        variable dial_bound_sources
        dservSet ess/dial/sources $dial_sources
        # WHY these sources, not just which. A protocol naming -sources
        # WINS over the rig's binding, and until this was published nothing
        # on screen said so: the rig could declare {mouse touch}, the loaded
        # system answer with a dpad, and the config you could read was not
        # the config in effect. That is the same shape as a joystick_init
        # sitting inert in local/ -- found the hard way the same day.
        #
        #   protocol : the loaded system named them (dial_init -sources)
        #   rig      : ::ess::dial_bind in local/post-input.tcl
        #   default  : neither, so the module's own {swipe touch}
        dservSet ess/dial/source_origin $dial_source_origin
        dservSet ess/dial/bound         $dial_bound_sources
    }

    proc dial_publish_geometry {} {
        variable dial_arc_center
        variable dial_arc_halfwidth
        variable dial_radius
        dservSet ess/dial/geometry \
            "$dial_arc_center,$dial_arc_halfwidth,$dial_radius"
    }

    # ---------------------------------------------------------------------
    # the free pointer (mouse)
    # ---------------------------------------------------------------------

    # Publish "x,y,show,in_band" -- degrees, y up, origin at the center of
    # the dial's circle, matching ess/dial/geometry and ess/touch_press_deg
    # so a stim can draw the dot in the same frame it drew the ring.
    proc dial_pointer_update { x y show in_band } {
        variable dial_pointer_dpoint
        dservSet $dial_pointer_dpoint \
            "[format %.4f $x],[format %.4f $y],$show,$in_band"
    }

    proc dial_pointer_hide {} {
        variable dial_pointer_shown
        variable dial_pointer_band
        set dial_pointer_shown 0
        set dial_pointer_band  -1
        dial_pointer_update 0 0 0 0
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
        variable dial_stick_last_ts; set dial_stick_last_ts 0
        variable dial_stick_moved;   set dial_stick_moved 0
        variable dial_stick_angle;   set dial_stick_angle 0.0
        # astick starts each window at the ORIGIN, for the same reason the
        # mouse does: a cursor left where the last trial ended anchors the
        # report toward the previous answer, and makes the directions no
        # longer equidistant. Clearing last_ts as well, so the first sample
        # of the window establishes dt rather than integrating across the
        # inter-trial gap.
        variable dial_astick_x;       set dial_astick_x       0.0
        variable dial_astick_y;       set dial_astick_y       0.0
        variable dial_astick_last_ts; set dial_astick_last_ts 0
        variable dial_astick_moved;   set dial_astick_moved   0
        variable dial_astick_reapproach; set dial_astick_reapproach 0
        variable dial_astick_committed;  set dial_astick_committed  0
        variable dial_astick_last_pub;   set dial_astick_last_pub   0
        variable dial_astick_n_dup;      set dial_astick_n_dup      0
        variable dial_astick_n_gap;      set dial_astick_n_gap      0

        # Consume whatever is already sitting in the transports, so a commit
        # that landed before the window cannot be picked up as the first
        # response after it.
        set dial_swipe_last [slider_swipe_time]
        set dial_touch_last [dial_touch_timestamp]
        # Only if the joystick is OURS to arm -- a protocol may be using it
        # for something else entirely, and joystick_reset clears its latch.
        variable dial_sources
        if { "joystick" in $dial_sources } { joystick_reset }

        # Put the mouse's virtual cursor back at the ORIGIN -- the center of
        # the circle, which names no direction, not the ring point at
        # arc_center, which names one and would anchor every report toward
        # it.
        #
        # The mouse is the one source that can do this. It is a RELATIVE
        # device whose absolute position is a fiction the reader maintains
        # (input.c integrates counts into a virtual cursor and clamps it to
        # a declared extent), so recentering desynchronises nothing -- there
        # is no physical position for the dot to disagree with, the way a
        # finger disagrees with a touchscreen.
        #
        # Two things come of it, and the second is the stronger one:
        #
        #   the report is unanchored -- each trial starts from nothing
        #   rather than from the previous trial's answer, which also ends
        #   the random walk into a corner where the extent clamps and whole
        #   directions stop being reportable
        #
        #   every reportable direction is EQUIDISTANT -- from the center of
        #   a circle each arc angle is one radius away, so motor cost is
        #   flat across directions and cannot confound the thing being
        #   measured. Starting from the last answer varies both.
        #
        # Validate here rather than in dial_init: the band comes from the
        # ring radius, and a protocol taking its radius from stimdg has not
        # set one when dial_init runs. By arm time all per-trial geometry is
        # in.
        #
        # Both halves matter. A radius of 0 accepts a click at the center; a
        # tolerance of 0 (the default) accepts nothing at all, which is the
        # worse failure because it looks like a subject who will not respond.
        # POINTER sources -- the ones that place a dot rather than report an
        # angle outright. They share the ring-band requirement and start the
        # window with the dot at the origin. Only the mouse also has a
        # device cursor to recenter; the dpad's cursor is ours and simply
        # starts at zero.
        variable dial_radius
        variable dial_ring_tolerance
        # Every source but the bare joystick now needs a RADIUS: the
        # steering ones place their cursor on the ring, the pointing ones
        # measure against it. Only the ones that accept BY the band need a
        # tolerance as well.
        set needs_ring [expr {[llength [lsearch -all -inline \
            -regexp $dial_sources {^(swipe|stick|astick|mouse|dpad|touch)$}]] > 0}]
        set needs_band [expr {[llength [lsearch -all -inline \
            -regexp $dial_sources {^(mouse|dpad|astick|touch)$}]] > 0}]
        if { $needs_ring && $dial_radius <= 0 } {
            error "::ess::dial_arm: this dial needs a ring radius --\
                   call dial_set_radius (the cursor is placed on it)"
        }
        if { $needs_band && $dial_ring_tolerance <= 0 } {
            error "::ess::dial_arm: this dial accepts by the ring band and\
                   needs a tolerance -- pass one to dial_set_radius, or\
                   -ring_tolerance to dial_init"
        }
        # astick converts a deflection into a SPEED, so it needs to know what
        # full deflection is. Refuse here rather than sit motionless: a
        # cursor that never leaves the origin looks exactly like a subject
        # who will not respond, which is the failure this file already calls
        # the worse one.
        if { "astick" in $dial_sources } {
            variable dial_astick_scale
            if { $dial_astick_scale <= 0 } {
                error "::ess::dial_astick: no full-scale deflection --\
                       pass -astick_scale to dial_init (in slider output\
                       units; measure it, do not assume 2048 counts of throw)"
            }
        }
        if { ("mouse" in $dial_sources) || ("dpad" in $dial_sources) ||
             ("astick" in $dial_sources) } {
            if { "mouse" in $dial_sources } {
                catch { send input "inputRecenter mouse" }
            }
            variable dial_dpad_r;       set dial_dpad_r       0.0
            variable dial_dpad_angle;   set dial_dpad_angle   -1.0
            variable dial_dpad_sector;  set dial_dpad_sector  -1
            variable dial_dpad_want;    set dial_dpad_want    -1
            variable dial_dpad_last_us; set dial_dpad_last_us 0
            variable dial_dpad_hold_us;  set dial_dpad_hold_us  0
            variable dial_dpad_homing;   set dial_dpad_homing   0
            variable dial_dpad_deflected; set dial_dpad_deflected 0
            dial_dpad_stop
            # Recentered means the dot IS at the origin, so publish it there
            # rather than leaving the previous trial's position on screen
            # until the first movement.
            variable dial_pointer_shown; set dial_pointer_shown 1
            variable dial_pointer_band;  set dial_pointer_band  0
            # Each response window starts unengaged, so the subject has to
            # reach the ring again rather than inheriting the last trial's
            # lock and being able to commit from anywhere immediately.
            variable dial_mouse_locked;  set dial_mouse_locked  0
            variable dial_pointer_x;     set dial_pointer_x     0.0
            variable dial_pointer_y;     set dial_pointer_y     0.0
            # The ONLY publish in this block, deliberately.
            #
            # This is what puts the dot on screen the moment the response
            # window opens, rather than leaving the subject looking at
            # nothing until they happen to move. It is also the cue that
            # invites the movement in the first place.
            #
            # It used to be followed immediately by a dial_cursor_update to
            # clear the ghost, and that pairing INTERMITTENTLY lost this
            # publish somewhere between dserv and the stim -- measured
            # roughly one arm in three -- so the dot kept the previous
            # trial's position until the first mouse delta moved it. Two
            # datapoints written back-to-back with no work between them is
            # the trigger; the cause is not understood and is worth finding,
            # because anything else that publishes in pairs is exposed to it.
            #
            # The ghost clear is not merely deferred, it is UNNECESSARY:
            # ess/cursor is not written by anything now, so there is no
            # second indicator left to take down.
            dial_pointer_update 0 0 1 0
        }
    }

    # Re-open the response window WITHOUT resetting where the cursor is.
    #
    # For the forgiving path: a reach that landed somewhere the protocol
    # will not accept should not be answered by teleporting the cursor back
    # to the center. That is a jump the subject did not cause, and with a
    # dpad it is worse than cosmetic -- the cursor would still be out in the
    # band, so the very next deflection would re-commit instantly from
    # wherever it sat.
    #
    # Instead the dpad enters HOMING: holding achieves nothing, and letting
    # the stick center walks the cursor back in at the travel rate. The
    # reset becomes something the animal performs rather than something
    # done to it -- and returning to center is a real joystick skill worth
    # training rather than papering over.
    #
    # dial_arm remains the right call at the START of a trial, where the
    # cursor genuinely should begin at the origin.
    proc dial_rearm {} {
        variable dial_active
        variable dial_armed_time
        variable dial_sources
        variable dial_report_angle
        variable dial_report_time
        variable dial_report_source
        variable dial_pending

        if { !$dial_active } return
        set dial_armed_time    [now]
        set dial_report_angle  -1.0
        set dial_report_time   0
        set dial_report_source ""
        set dial_pending       ""

        if { "dpad" in $dial_sources } {
            variable dial_dpad_r
            variable dial_dpad_homing
            # Already home? Then there is nothing to walk back and the next
            # deflection may start immediately.
            set dial_dpad_homing [expr {$dial_dpad_r > 0.0 ? 1 : 0}]
            variable dial_dpad_deflected
            if { $dial_dpad_homing && !$dial_dpad_deflected } {
                variable dial_dpad_last_us; set dial_dpad_last_us [now]
                variable dial_dpad_timer
                if { $dial_dpad_timer eq "" } { dial_dpad_tick }
            }
        }

        # astick cannot borrow the dpad's homing, and the reason is worth
        # stating: homing works because releasing a d-pad means "walk back",
        # whereas releasing an analog stick means velocity ZERO -- the cursor
        # simply stops. So a rejected reach would sit out in the band, and
        # the next twitch in any direction would re-commit instantly from
        # where it already was, scoring a response the subject never made.
        #
        # Instead the commit is LATCHED OFF until the cursor has come back
        # inside the band under the subject's own steering. Retrying costs a
        # real journey in and out again, which is the same contingency the
        # dpad's homing creates by different means.
        if { "astick" in $dial_sources } {
            variable dial_astick_reapproach
            variable dial_astick_x
            variable dial_astick_y
            variable dial_radius
            variable dial_ring_tolerance
            set r [expr {sqrt($dial_astick_x*$dial_astick_x +
                              $dial_astick_y*$dial_astick_y)}]
            set dial_astick_reapproach \
                [expr {$r >= $dial_radius - $dial_ring_tolerance ? 1 : 0}]
            # The commit latch has to come off or the re-opened window would
            # be dead: nothing would integrate, and the trial would wait out
            # its timeout with a frozen cursor.
            variable dial_astick_committed; set dial_astick_committed 0
        }
        return
    }

    proc dial_disarm {} {
        variable dial_armed_time
        variable dial_cursor_shown
        variable dial_pointer_shown
        set dial_armed_time 0
        # A cursor still walking after the window closed would invite a
        # commit the dial has stopped listening for.
        catch { dial_dpad_stop }
        if { $dial_cursor_shown } { dial_cursor_update 0 0 }
        set dial_cursor_shown 0
        # The dot tracks a live device, so unlike the ring cursor there is
        # no reading under which it should survive the response window --
        # a dot still moving after the dial stopped listening invites
        # clicks that do nothing.
        if { $dial_pointer_shown } { dial_pointer_hide }
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
    # Place the report cursor ON THE RING at this angle, as a POINTER.
    #
    # One output contract for every source. The steering sources (swipe,
    # stick) name an angle and nothing else, so their cursor is placed at
    # the ring radius; the pointing sources (mouse, dpad) place theirs
    # wherever the hand is. A consumer therefore draws ess/dial/pointer and
    # never learns which transport is feeding it -- which is what makes
    # "who answers this dial" a rig decision the stim cannot see.
    #
    # This replaced ess/cursor's "angle_rad,show". Two contracts meant every
    # stim carried a branch, and getting the precedence wrong hid a catcher
    # on the frame the mouse wanted it shown. in_band is 1 here because a
    # steered angle is always clamped into the arc: if it is on screen at
    # all, committing it is legal.
    proc dial_cursor_update { angle show } {
        variable dial_radius
        variable dial_pointer_shown
        variable dial_pointer_band
        variable dial_pointer_x
        variable dial_pointer_y
        if { !$show } { dial_pointer_hide; return }
        set px [expr {$dial_radius*cos($angle)}]
        set py [expr {$dial_radius*sin($angle)}]
        set dial_pointer_x     $px
        set dial_pointer_y     $py
        set dial_pointer_band  1
        set dial_pointer_shown 1
        dial_pointer_update $px $py 1 1
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

        if { ![dial_in_ring [expr {sqrt($tx*$tx + $ty*$ty)}]] } { return "" }

        set angle [dial_norm2pi [expr {atan2($ty, $tx)}]]
        if { ![dial_in_arc $angle] } { return "" }
        return $angle
    }

    # Is this distance from the center inside the ring band?
    #
    # Shared by touch and mouse so the two cannot drift apart: a mouse is a
    # touch with a visible cursor, and "landed on the ring" has to mean the
    # same thing for both or the same click reports differently depending on
    # which hardware made it. This file already carries two bugs that were
    # exactly that kind of drift (see the header).
    proc dial_in_ring { dist } {
        variable dial_radius
        variable dial_ring_tolerance
        return [expr {$dist >= $dial_radius - $dial_ring_tolerance &&
                      $dist <= $dial_radius + $dial_ring_tolerance}]
    }

    # The extent's center and scale, from mouse/event/range
    # ([0 max_x 0 max_y]). Needed before any position can be computed, so a
    # sample arriving first is simply dropped rather than measured from
    # (0,0) at an unknown scale.
    #
    # The extent maps onto the SCREEN: crossing the whole extent crosses
    # the whole display (times -mouse_scale). That is the mapping a subject
    # can form a model of, and it makes the declared extent in inputconf.tcl
    # the single place a rig states how far a hand movement goes.
    proc dial_mouse_range { dpoint data } {
        variable dial_mouse_range_known
        variable dial_mouse_cx
        variable dial_mouse_cy
        variable dial_mouse_dpp_x
        variable dial_mouse_dpp_y
        variable dial_mouse_scale
        lassign $data minx maxx miny maxy
        if { $maxx eq "" || $maxy eq "" } return
        set spanx [expr {$maxx - $minx}]
        set spany [expr {$maxy - $miny}]
        if { $spanx <= 0 || $spany <= 0 } return

        set dial_mouse_cx [expr {($minx + $maxx)/2.0}]
        set dial_mouse_cy [expr {($miny + $maxy)/2.0}]

        # The screen's half-extent in degrees, from the DATAPOINTS ::ess
        # publishes at init. Not $::ess::screen_halfx -- there is no such
        # variable (the values live in the loaded system's own namespace),
        # and reading it under a bare catch silently produced a scale from
        # the fallback instead: a dot running at 68% of true size on a
        # 23.4x13.3 degree screen, self-consistent enough that nothing
        # looked wrong.
        #
        # So the fallback now SAYS SO. A wrong scale is invisible by
        # construction -- every angle still works, the ring is still
        # reachable, only the gain is off -- which is exactly the kind of
        # failure that has to announce itself or never be found.
        set hx ""
        set hy ""
        catch { set hx [dservGet ess/screen_halfx] }
        catch { set hy [dservGet ess/screen_halfy] }
        if { ![string is double -strict $hx] || $hx <= 0 ||
             ![string is double -strict $hy] || $hy <= 0 } {
            set hx 16.0
            set hy 9.0
            puts stderr "::ess::dial: ess/screen_halfx|halfy unavailable;\
                         mouse scale falling back to ${hx}x${hy} deg --\
                         the dot's gain will be wrong if that is not the\
                         real screen"
        }

        set dial_mouse_dpp_x [expr {2.0*$hx*$dial_mouse_scale/$spanx}]
        set dial_mouse_dpp_y [expr {2.0*$hy*$dial_mouse_scale/$spany}]
        set dial_mouse_range_known 1
    }

    # Mouse as a free pointer: place the dot, click on the arc to report.
    #
    # MOVE (3) and DRAG (1) both move the dot -- whether a button happens
    # to be held should not change where a pointer is. PRESS (0) reports,
    # because button-down is the moment of decision and gives the cleaner
    # reaction time; RELEASE (2) is ignored, so a click is one response
    # rather than two.
    #
    # A press only reports if the dot is on the ring band and inside the
    # arc. Anywhere else it does NOTHING -- deliberately, and this is the
    # one place the design has a cost: an ignored click is invisible. That
    # is what in_band pays for. The dot carries "you are on target"
    # continuously, so the subject knows before clicking, and a click that
    # does nothing is one they could see coming.
    #
    # ONE publication per sample: ess/dial/pointer. The ring ghost is not
    # driven from here (see the dial_pointer_dpoint comment) -- the dot is
    # the only moving thing, and it stopped stuttering when it became the
    # only publish.
    proc dial_mouse_sample { dpoint data } {
        variable dial_active
        variable dial_armed_time
        variable dial_pi
        variable dial_deadband_deg
        variable dial_cursor_shown
        variable dial_last_sent
        variable dial_pending
        variable dial_mouse_range_known
        variable dial_mouse_cx
        variable dial_mouse_cy
        variable dial_mouse_dpp_x
        variable dial_mouse_dpp_y
        variable dial_pointer_shown
        variable dial_pointer_band
        variable dial_pointer_x
        variable dial_pointer_y
        variable dial_pointer_deadband

        if { !$dial_active || $dial_armed_time == 0 } return
        if { !$dial_mouse_range_known } return

        lassign $data x y ev
        if { $x eq "" || $y eq "" } return
        if { $ev == 2 } return   ;# button-up is not a second response

        # Published y grows DOWNWARD (mouse_reader keeps REL_Y's sense, the
        # same as a touchscreen's ABS_Y), so flip it into the dial's world.
        set px [expr {($x - $dial_mouse_cx)*$dial_mouse_dpp_x}]
        set py [expr {($dial_mouse_cy - $y)*$dial_mouse_dpp_y}]

        set r      [expr {sqrt($px*$px + $py*$py)}]
        set angle  [dial_norm2pi [expr {atan2($py, $px)}]]
        set in_arc [dial_in_arc $angle]

        # Reaching the band engages the report; from then on the radius is
        # free and only the arc still gates. See the lock's rationale above.
        variable dial_mouse_locked
        if { $in_arc && [dial_in_ring $r] } { set dial_mouse_locked 1 }
        set band [expr {($dial_mouse_locked && $in_arc) ? 1 : 0}]

        if { $ev == 0 } {
            # REPORT. Rejected out of band, exactly as a touch off the ring
            # is rejected -- the same dial_in_ring and dial_in_arc pair, and
            # for the same reason: a press is a discrete choice, so there is
            # nothing to snap it to that the subject actually chose.
            if { !$band } return
            # Latched as pending and consumed by dial_response, so a mouse
            # report takes the same path into the protocol as every other
            # source.
            set dial_pending [list $angle mouse]
            do_update
            return
        }

        # MOVE -- do NOT wake the state machine (a 1 kHz mouse would
        # otherwise drive the SM at 1 kHz). Publish the dot on real
        # movement, and ALWAYS on a band change: the deadband exists to
        # thin a fast mouse, never to delay the affordance.
        set moved [expr {abs($px - $dial_pointer_x) > $dial_pointer_deadband ||
                         abs($py - $dial_pointer_y) > $dial_pointer_deadband}]
        if { $moved || $band != $dial_pointer_band || !$dial_pointer_shown } {
            set dial_pointer_x     $px
            set dial_pointer_y     $py
            set dial_pointer_band  $band
            set dial_pointer_shown 1
            dial_pointer_update $px $py 1 $band
        }
    }

    # ---------------------------------------------------------------------
    # dpad: a cursor that walks out along the held spoke
    # ---------------------------------------------------------------------

    # Travel rate right now, in degrees per second.
    #
    # THE place acceleration lives. Everything else -- the tick, the
    # commit, the display -- is written against "what is the rate at this
    # instant", so a different curve is a change here and nowhere else.
    # Linear is the digital-clock behavior; a staged or exponential ramp
    # would substitute cleanly.
    proc dial_dpad_rate_now { held_s } {
        variable dial_dpad_rate
        variable dial_dpad_accel
        variable dial_dpad_rate_max
        set r [expr {$dial_dpad_rate + $dial_dpad_accel*$held_s}]
        if { $dial_dpad_rate_max > 0 && $r > $dial_dpad_rate_max } {
            set r $dial_dpad_rate_max
        }
        return $r
    }

    # ::ess::dial_dpad_sectors ?list?
    #
    # Per-trial, like dial_set_arc: the protocol names which directions are
    # on offer. Called with no argument it REPORTS the current list -- the
    # same contract as dial_bind, and for the same reason: a read that
    # writes is a trap for anything that displays state.
    proc dial_dpad_sectors { args } {
        variable dial_dpad_sectors
        if { [llength $args] == 0 } { return $dial_dpad_sectors }
        set dial_dpad_sectors [lindex $args 0]
        return $dial_dpad_sectors
    }

    # Is this direction on offer this trial?
    proc dial_dpad_live { sector } {
        variable dial_dpad_sectors
        if { ![llength $dial_dpad_sectors] } { return 1 }   ;# all eight
        return [expr {[lsearch -exact -integer $dial_dpad_sectors $sector] >= 0}]
    }

    # ::ess::dial_dpad_tune ?-rate X? ?-accel Y? ?-rate_max Z?
    #
    # Retune the travel WITHOUT re-running dial_init, which reads these once
    # at protocol load. That is what a live param needs to reach: changing
    # cursor_accel from the params panel updated the variable while the
    # cursor kept walking at the old speed, with nothing to say why.
    #
    #   $s add_live_param cursor_accel 0.0 float {::ess::dial_dpad_tune -accel}
    #
    # Takes effect on the NEXT tick, so a hold already in flight keeps the
    # rate it started with -- with commit "ring" the travel time is part of
    # that trial's reaction time, and changing it underneath would corrupt
    # the measurement rather than merely surprise the subject.
    #
    # Called with no arguments it REPORTS, per this file's convention.
    proc dial_dpad_tune { args } {
        variable dial_dpad_rate
        variable dial_dpad_accel
        variable dial_dpad_rate_max
        if { [llength $args] == 0 } {
            return [list -rate $dial_dpad_rate -accel $dial_dpad_accel \
                        -rate_max $dial_dpad_rate_max]
        }
        foreach { k v } $args {
            switch -- $k {
                -rate     { set dial_dpad_rate $v }
                -accel    { set dial_dpad_accel $v }
                -rate_max { set dial_dpad_rate_max $v }
                default   { error "::ess::dial_dpad_tune: unknown option '$k'" }
            }
        }
        return [list -rate $dial_dpad_rate -accel $dial_dpad_accel \
                    -rate_max $dial_dpad_rate_max]
    }

    proc dial_dpad_stop {} {
        variable dial_dpad_timer
        if { $dial_dpad_timer ne "" } {
            catch { dservAfterCancel $dial_dpad_timer }
            set dial_dpad_timer ""
        }
    }

    # ess/joystick/dir carries the SECTOR (0..7 clockwise from up) or -1 for
    # centered, republished by joystick_ingest on every state change.
    #
    # Deflection starts the timer; centering stops it and the cursor HOLDS
    # where it got to. Holding rather than decaying is deliberate for
    # shaping: progress made in a short push is kept, so an animal can
    # reach the ring in several bursts instead of needing one sustained
    # hold it cannot yet produce. Requiring the hold is a LATER criterion,
    # and it is a decay rule here, not a redesign.
    proc dial_dpad_dir { dpoint data } {
        variable dial_active
        variable dial_armed_time
        variable dial_pi
        variable dial_dpad_angle
        variable dial_dpad_timer
        variable dial_dpad_last_us
        variable dial_dpad_hold_us

        if { !$dial_active || $dial_armed_time == 0 } return
        if { ![string is entier -strict $data] } return

        variable dial_dpad_deflected
        variable dial_dpad_homing
        # declared HERE, not beside its first use further down: the centered
        # branch below returns long before that point, and a `set` without
        # this line makes a LOCAL, leaving the namespace value untouched.
        # That is the third time this file has been bitten by it.
        variable dial_dpad_want

        if { $data < 0 } {
            set dial_dpad_deflected 0
            # Clear the REQUEST too, not just the deflection flag.
            #
            # Without this, "am I meant to be walking?" lived only in the
            # timer, so correctness rested on dservAfterCancel winning every
            # race. A tick already queued when the release landed would find
            # want still naming the old spoke, extend, re-arm, and keep going
            # -- and with acceleration on it did not creep, it ran to the ring
            # and COMMITTED a response nobody made.
            #
            # With want cleared, any stray tick hits the centered guard and
            # stops itself. The state says what should happen; the timer is
            # only how it happens.
            set dial_dpad_want -1
            # Centered. Normally that HOLDS the cursor where it got to; while
            # homing it is the opposite -- releasing is the thing that walks
            # it back, so the timer keeps running.
            if { $dial_dpad_homing } {
                if { $dial_dpad_timer eq "" } {
                    set dial_dpad_last_us [now]
                    dial_dpad_tick
                }
            } else {
                dial_dpad_stop
            }
            return
        }

        # Homing is judged on the RAW deflection, before the liveness gate:
        # while coming home, holding anything at all achieves nothing, and
        # only letting the stick center walks the cursor back.
        if { $dial_dpad_homing } {
            set dial_dpad_deflected 1
            dial_dpad_stop
            return
        }

        # A direction that is not on offer this trial is treated exactly as
        # centered: the cursor HOLDS where it is. Not a retraction -- an
        # off-axis push is "not pushing", and should cost nothing rather
        # than undo travel already earned. Returning to a live direction
        # resumes from here, since neither the spoke nor the request moved.
        if { ![dial_dpad_live $data] } {
            set dial_dpad_deflected 0
            dial_dpad_stop
            return
        }

        set dial_dpad_deflected 1

        # Record what the stick is ASKING for. The cursor does not move here
        # -- dial_dpad_tick decides how to get there, and the answer is never
        # "instantly".
        #
        # A new direction is reached BY WAY OF THE CENTER: the cursor
        # retracts along the spoke it is on, and only once it reaches zero
        # does it set out along the new one. Re-aiming at the current radius
        # was the earlier rule and it swung the cursor around the circle --
        # a jump to the other side, which reads as a teleport rather than as
        # the subject's own doing.
        #
        # It also keeps a real contingency: changing your mind costs the
        # travel you already spent, and passing through the center is the
        # same return-to-center skill the joystick task is teaching.
        #
        # The report still names one of the eight directions the device can
        # express, because the cursor is only ever ON a spoke. A free 2D
        # velocity would curve the path and land the report between sectors,
        # a precision four switches do not have.
        set dial_dpad_want $data

        if { $dial_dpad_timer eq "" } {
            set dial_dpad_last_us [now]
            set dial_dpad_hold_us [now]
            dial_dpad_tick
        }
    }

    # One step of travel. Publishes the pointer; does NOT wake the state
    # machine -- the SM hears about the commit, not about the cursor, the
    # same rule every other source here follows.
    proc dial_dpad_tick {} {
        variable dial_active
        variable dial_armed_time
        variable dial_dpad_r
        variable dial_dpad_angle
        variable dial_dpad_timer
        variable dial_dpad_last_us
        variable dial_dpad_hold_us
        variable dial_dpad_tick_ms
        variable dial_dpad_commit
        variable dial_radius
        variable dial_ring_tolerance
        variable dial_pending
        variable dial_pointer_shown
        variable dial_pointer_band
        variable dial_pointer_x
        variable dial_pointer_y

        variable dial_dpad_sector
        variable dial_dpad_want
        variable dial_pi          ;# the spoke's angle is computed HERE now

        # RESCHEDULE FIRST, and let dial_dpad_stop be the only thing that
        # ends the walk.
        #
        # It used to reschedule on the last line, which made every early
        # return a decision to stop -- three of them deliberate, and any
        # fourth one (an unexpected error, a guard added later) a silent
        # freeze of the cursor with nothing to say why. A one-shot timer
        # that must be re-armed at the bottom of its own body is only as
        # reliable as its least-travelled exit path.
        #
        # dserv has no periodic timer a subprocess may claim freely: the
        # timer pool is handed to the state machine wholesale
        # (timerSetScript over 0..nTimers-1 -> do_update), so dservAfter is
        # what there is. This gets the robustness without needing one.
        set dial_dpad_timer [dservAfter $dial_dpad_tick_ms ::ess::dial_dpad_tick]

        if { !$dial_active || $dial_armed_time == 0 } { dial_dpad_stop; return }
        # Nothing to do only when there is neither a spoke to be on nor one
        # being asked for. Guarding on the ANGLE here was wrong once the
        # spoke came to be adopted inside this proc: the angle is still -1
        # on the first tick of a reach, so the cursor never set out at all.
        if { $dial_dpad_sector < 0 && $dial_dpad_want < 0 } { dial_dpad_stop; return }

        set t  [now]
        set dt [expr {($t - $dial_dpad_last_us)/1.0e6}]
        set dial_dpad_last_us $t
        # A gap this large means the interp was busy elsewhere; integrating
        # across it would teleport the cursor.
        if { $dt <= 0 || $dt > 0.25 } { set dt 0.0 }

        variable dial_dpad_homing
        variable dial_dpad_deflected

        if { $dial_dpad_homing } {
            # Walking back in at the base rate. No acceleration: coming home
            # is not a response and should not be a race.
            variable dial_dpad_rate
            set dial_dpad_r [expr {$dial_dpad_r - $dial_dpad_rate*$dt}]
            if { $dial_dpad_r <= 0.0 } {
                set dial_dpad_r     0.0
                set dial_dpad_homing 0
            }
            set px [expr {$dial_dpad_r*cos($dial_dpad_angle)}]
            set py [expr {$dial_dpad_r*sin($dial_dpad_angle)}]
            set dial_pointer_x     $px
            set dial_pointer_y     $py
            set dial_pointer_band  0
            set dial_pointer_shown 1
            dial_pointer_update $px $py 1 0
            # arrived home -- nothing left to walk
            if { !$dial_dpad_homing } { dial_dpad_stop }
            return
        }

        variable dial_dpad_sector
        variable dial_dpad_want
        variable dial_dpad_rate

        # First deflection of the window: adopt the spoke and set out.
        if { $dial_dpad_sector < 0 && $dial_dpad_want >= 0 } {
            set dial_dpad_sector $dial_dpad_want
            set dial_dpad_angle [dial_norm2pi \
                [expr {(90.0 - 45.0*$dial_dpad_sector)*$dial_pi/180.0}]]
        }

        # Centered with nothing to walk back: hold where we are. The dir
        # handler stops the timer on release, so reaching here means an
        # ordering we did not expect -- holding is the safe reading, and
        # extending on a released stick would be the damaging one.
        if { $dial_dpad_want < 0 } { dial_dpad_stop; return }

        if { $dial_dpad_want != $dial_dpad_sector } {
            # Asked for a different spoke: come back in first. No
            # acceleration on the way home -- reconsidering is not a race.
            set dial_dpad_r [expr {$dial_dpad_r - $dial_dpad_rate*$dt}]
            if { $dial_dpad_r <= 0.0 } {
                set dial_dpad_r      0.0
                set dial_dpad_sector $dial_dpad_want
                set dial_dpad_angle [dial_norm2pi \
                    [expr {(90.0 - 45.0*$dial_dpad_sector)*$dial_pi/180.0}]]
                # The new spoke is a new hold, so acceleration starts over.
                set dial_dpad_hold_us $t
            }
        } else {
            set held [expr {($t - $dial_dpad_hold_us)/1.0e6}]
            set dial_dpad_r \
                [expr {$dial_dpad_r + [dial_dpad_rate_now $held]*$dt}]
        }

        # Park at the band's outer edge. With commit "ring" this is barely
        # reachable, but with commit "none" it keeps a held switch from
        # walking the cursor off the screen.
        set rmax [expr {$dial_radius + $dial_ring_tolerance}]
        if { $dial_dpad_r > $rmax } { set dial_dpad_r $rmax }

        set px [expr {$dial_dpad_r*cos($dial_dpad_angle)}]
        set py [expr {$dial_dpad_r*sin($dial_dpad_angle)}]
        set band [expr {([dial_in_ring $dial_dpad_r] &&
                         [dial_in_arc $dial_dpad_angle]) ? 1 : 0}]

        set dial_pointer_x     $px
        set dial_pointer_y     $py
        set dial_pointer_band  $band
        set dial_pointer_shown 1
        dial_pointer_update $px $py 1 $band

        # Commit on reaching the RING RADIUS -- the target's center -- not
        # the band's near edge. The band still drives in_band, so the
        # affordance lights as the cursor enters it and the cursor then
        # travels the last of the way and lands ON the target. Selecting
        # from the near edge left it visibly short of the thing it chose.
        if { $dial_dpad_r >= $dial_radius && [dial_in_arc $dial_dpad_angle] &&
             $dial_dpad_commit eq "ring" } {
            # Arriving IS the response: no second action to learn, and the
            # animal has watched the cursor earn it the whole way.
            set dial_pending [list $dial_dpad_angle dpad]
            dial_dpad_stop
            do_update
            return
        }
    }

    # Commits arrive asynchronously via dial_dpad_tick latching dial_pending,
    # which dial_response consumes before it polls. Nothing to poll here.
    proc dial_poll_dpad {} { return "" }

    # ---------------------------------------------------------------------
    # astick: the deflection vector is the cursor's velocity
    # ---------------------------------------------------------------------

    # Runs on every slider sample. Like every other cursor observer here it
    # does NOT wake the state machine -- the SM hears about the commit, not
    # about the cursor.
    proc dial_astick_sample { dpoint data } {
        variable dial_active
        variable dial_armed_time
        variable dial_astick_rate
        variable dial_astick_deadzone
        variable dial_astick_expo
        variable dial_astick_scale
        variable dial_astick_commit
        variable dial_astick_x
        variable dial_astick_y
        variable dial_astick_last_ts
        variable dial_astick_moved
        variable dial_radius
        variable dial_ring_tolerance
        variable dial_pending
        variable dial_pointer_shown
        variable dial_pointer_band
        variable dial_pointer_x
        variable dial_pointer_y
        variable dial_pointer_deadband

        variable dial_astick_committed
        if { !$dial_active || $dial_armed_time == 0 } return
        if { $dial_astick_committed } return
        if { $dial_astick_scale <= 0 } return

        lassign $data sx sy
        if { $sx eq "" } return
        if { $sy eq "" } { set sy 0.0 }

        # dt from the DATAPOINT, which carries the box's stamp (sliderconf
        # passes it through), not a Tcl clock.
        set ts [dservTimestamp $dpoint]
        if { $dial_astick_last_ts == 0 } { set dial_astick_last_ts $ts; return }
        set dt [expr {($ts - $dial_astick_last_ts)/1.0e6}]
        set dial_astick_last_ts $ts

        # A ZERO dt carries no time to integrate, so there is nothing to do
        # with it -- but it is not rare and it is not this file's fault.
        # Measured on a teensy box at 200 Hz: 7.4% of blocks arrive stamped
        # IDENTICALLY to the one before, microseconds apart in host time.
        # Counted rather than silently skipped, because "the box duplicates
        # timestamps" is a claim that should be checkable from the rig.
        variable dial_astick_n_dup
        if { $dt <= 0 } { incr dial_astick_n_dup; return }

        # A LARGE gap means samples stopped arriving. Measured on the same
        # box: 41 gaps over 105 ms, worst 175 ms, with the box's own
        # dropped/late/slips/overruns counters all flat.
        #
        # This used to `return`, discarding the gap entirely -- and that is
        # what made a box-side stall look like a cursor bug. The subject was
        # holding the stick the whole time, so the travel WAS intended;
        # throwing it away freezes the cursor for the full duration of the
        # stall. The d-pad never showed the same stalls because its timer
        # integrates from [now] and simply catches up.
        #
        # So catch up, but CAP it: integrating a full 175 ms at speed is a
        # visible jump, and integrating an arbitrarily long gap (a paused
        # box, a resumed trial) would teleport the cursor across the field.
        # The cap turns a freeze into a small jump, which is the least-wrong
        # reading of "the stick was held and we did not hear about it".
        variable dial_astick_max_dt
        variable dial_astick_n_gap
        if { $dt > $dial_astick_max_dt } {
            incr dial_astick_n_gap
            set dt $dial_astick_max_dt
        }

        # Deflection -> velocity, in ::ess::stick_velocity: deadzone,
        # rescale, expo, and speed-from-magnitude-direction-from-the-vector.
        # Shared with the `stick` source and the roaming mode.
        lassign [stick_velocity $sx $sy $dial_astick_scale \
                     $dial_astick_deadzone $dial_astick_expo \
                     $dial_astick_rate] vx vy
        if { $vx == 0.0 && $vy == 0.0 } return     ;# at rest: cursor holds

        set dial_astick_x [expr {$dial_astick_x + $vx*$dt}]
        set dial_astick_y [expr {$dial_astick_y + $vy*$dt}]
        set dial_astick_moved 1

        set r [expr {sqrt($dial_astick_x*$dial_astick_x +
                          $dial_astick_y*$dial_astick_y)}]
        set a [expr {atan2($dial_astick_y, $dial_astick_x)}]

        # The ARC still gates, and out-of-arc is CLAMPED rather than
        # rejected -- astick is a steering source, the subject is watching
        # the cursor and can see where it is, which is the side of this
        # file's documented asymmetry that clamps. Clamp the BEARING and
        # keep the radius, then write the position back so the integrator
        # does not keep accumulating into a wall.
        set ca [dial_clamp_arc $a]
        if { $ca != $a } {
            set a $ca
            set dial_astick_x [expr {$r*cos($a)}]
            set dial_astick_y [expr {$r*sin($a)}]
        }

        # Park at the band's outer edge, as the dpad does: with commit
        # "none" a held stick would otherwise drive the cursor off screen.
        set rmax [expr {$dial_radius + $dial_ring_tolerance}]
        if { $r > $rmax } {
            set r $rmax
            set dial_astick_x [expr {$r*cos($a)}]
            set dial_astick_y [expr {$r*sin($a)}]
        }

        set band [expr {[dial_in_ring $r] ? 1 : 0}]

        # Publish when the drawn picture would change, and NEVER gate a
        # change of band on either limiter -- the affordance has to be crisp
        # even when the cursor is barely moving, or the ring lights late and
        # the subject cannot tell that arriving will be accepted.
        variable dial_astick_pub_ms
        variable dial_astick_last_pub
        variable dial_astick_min_step
        set due [expr {$dial_astick_pub_ms <= 0 ||
                       ($ts - $dial_astick_last_pub)/1000.0 >=
                       $dial_astick_pub_ms}]
        set moved [expr {abs($dial_astick_x - $dial_pointer_x) +
                         abs($dial_astick_y - $dial_pointer_y)}]
        if { !$dial_pointer_shown || $band != $dial_pointer_band ||
             ($due && $moved > $dial_astick_min_step) } {
            set dial_astick_last_pub $ts
            set dial_pointer_x     $dial_astick_x
            set dial_pointer_y     $dial_astick_y
            set dial_pointer_band  $band
            set dial_pointer_shown 1
            dial_pointer_update $dial_astick_x $dial_astick_y 1 $band
        }

        # A rejected reach left the cursor out here; it does not count again
        # until it has come back inside the band under the subject's own
        # steering (see dial_rearm).
        variable dial_astick_reapproach
        if { $dial_astick_reapproach } {
            if { $r < $dial_radius - $dial_ring_tolerance } {
                set dial_astick_reapproach 0
            }
            return
        }

        # Commit on reaching the RING RADIUS -- the target's center, not the
        # band's near edge -- so the cursor lands ON the thing it chose.
        # Arriving IS the response: no second action to learn, and the
        # subject has watched the cursor earn it the whole way.
        if { $r >= $dial_radius && $dial_astick_commit eq "ring" } {
            # LATCH, and stop. The dpad's equivalent calls dial_dpad_stop
            # here, which kills its timer and ends the walk; this source has
            # no timer to kill, so without an explicit latch it simply keeps
            # running -- re-setting dial_pending and calling do_update on
            # EVERY sample, at the box's 200 Hz, until the state machine
            # gets round to reading the response and disarming.
            #
            # Two things came of that, and the first is the one you see: the
            # state machine woken 200 times a second in the middle of a
            # trial. The second is that the cursor kept integrating after it
            # had already committed, so it crept on past the target it had
            # just chosen.
            #
            # A latch rather than a disarm: disarming is dial_response's
            # job, and doing it here would consume the window before the
            # protocol had read the answer out of it.
            set dial_astick_committed 1
            set dial_pending [list $a astick]
            do_update
        }
    }

    # Live adjustment, mirroring dial_dpad_tune. rate and expo are found by
    # watching a subject, so they want to be reachable from an
    # add_live_param without a reload.
    proc dial_astick_tune { args } {
        variable dial_astick_rate
        variable dial_astick_deadzone
        variable dial_astick_expo
        variable dial_astick_scale
        variable dial_astick_commit
        variable dial_astick_max_dt
        variable dial_astick_n_dup
        variable dial_astick_n_gap
        foreach { k v } $args {
            switch -- $k {
                -rate     { set dial_astick_rate     [expr {double($v)}] }
                -deadzone { set dial_astick_deadzone [expr {double($v)}] }
                -expo     { set dial_astick_expo     [expr {double($v)}] }
                -scale    { set dial_astick_scale    [expr {double($v)}] }
                -pub_ms   {
                    variable dial_astick_pub_ms
                    set dial_astick_pub_ms [expr {int($v)}]
                }
                -min_step {
                    variable dial_astick_min_step
                    set dial_astick_min_step [expr {double($v)}]
                }
                -max_dt   { set dial_astick_max_dt [expr {double($v)}] }
                -commit   {
                    if { $v ni {ring none} } {
                        error "::ess::dial_astick_tune: commit is ring|none"
                    }
                    set dial_astick_commit $v
                }
                default { error "::ess::dial_astick_tune: unknown option '$k'" }
            }
        }
        if { $dial_astick_deadzone < 0 || $dial_astick_deadzone >= 1.0 } {
            error "::ess::dial_astick_tune: deadzone is a FRACTION of full\
                   scale, in [0,1)"
        }
        return [list rate $dial_astick_rate deadzone $dial_astick_deadzone \
                     expo $dial_astick_expo scale $dial_astick_scale \
                     commit $dial_astick_commit max_dt $dial_astick_max_dt \
                     dup_stamps $dial_astick_n_dup gaps $dial_astick_n_gap]
    }

    # Commits arrive asynchronously via dial_astick_sample latching
    # dial_pending. Nothing to poll.
    proc dial_poll_astick {} { return "" }

    # Velocity steering from a self-centering stick.
    #
    # Reads slider/position, so it inherits sliderconf's center, deadzone,
    # scale and invert calibration -- which is what makes integrating safe.
    # An uncalibrated stick never rests at exactly zero, and an integrator
    # turns that residue into a cursor that drifts with nobody touching it.
    #
    # dt comes from the DATAPOINT's timestamps, not a Tcl clock: the extio
    # box stamps on a PTP-disciplined grid, so the rotation rate does not
    # wander with scheduler jitter the way a timer-driven integrator would.
    proc dial_stick_sample { dpoint data } {
        variable dial_active
        variable dial_armed_time
        variable dial_pi
        variable dial_deadband_deg
        variable dial_cursor_shown
        variable dial_last_sent
        variable dial_stick_rate
        variable dial_stick_deadzone
        variable dial_stick_invert
        variable dial_stick_scale
        variable dial_stick_last_ts
        variable dial_stick_angle
        variable dial_stick_moved
        variable dial_stick_min_scale
        variable dial_arc_center

        if { !$dial_active || $dial_armed_time == 0 } return

        lassign $data x y
        if { $x eq "" } return

        set ts [dservTimestamp $dpoint]
        if { $dial_stick_last_ts == 0 } { set dial_stick_last_ts $ts; return }
        set dt [expr {($ts - $dial_stick_last_ts)/1.0e6}]
        set dial_stick_last_ts $ts
        # A gap this large means samples were missed or the trial just
        # started; integrating across it would jump the cursor.
        if { $dt <= 0 || $dt > 0.25 } return

        # Full scale is LEARNED from the largest deflection seen, so the
        # rate is relative to this stick's actual travel rather than a
        # constant maintained per rig -- but it is learned only from
        # deflections that are plausibly deliberate.
        #
        # Bootstrapping off ANY sample was wrong and produced a first-trial
        # meander: a stick whose center is not calibrated does not rest at
        # zero (measured -0.39 on rpi500, whose sliderconf center is the
        # generic 2048), so the very first sample made the RESTING OFFSET
        # the definition of full deflection -- f = 1.0, full-rate rotation,
        # from an untouched stick. It settled once a real push raised the
        # scale, which is why it only ever showed up on trial one.
        set ax [expr {abs($x)}]
        if { $ax > $dial_stick_min_scale && $ax > $dial_stick_scale } {
            set dial_stick_scale $ax
        }
        if { $dial_stick_scale <= 0 } return

        # Deadzone, the rescale past it, and the expo curve are all in
        # ::ess::stick_gain, shared with the astick source (and the roaming
        # mode) so the three cannot drift apart -- which is the whole reason
        # this file exists. See its comment for why the rescale matters.
        #
        # The SIGN stays here, because this source is one-dimensional: a
        # deflection either way rotates the bearing the corresponding way,
        # where a 2-D source gets its direction from the vector instead.
        variable dial_stick_expo
        set f  [expr {$x/$dial_stick_scale}]
        set af [stick_gain [expr {abs($f)}] \
                    $dial_stick_deadzone $dial_stick_expo]
        if { $af <= 0.0 } return                   ;# at rest: no drift
        set f [expr {$f < 0 ? -$af : $af}]
        if { $dial_stick_invert } { set f [expr {-$f}] }

        # First deflection of the trial starts the cursor at the arc center
        # -- the neutral choice -- and shows it. Until then there is nothing
        # on screen, so the ball is not competing with a report cursor.
        if { !$dial_stick_moved } {
            set dial_stick_moved 1
            set dial_stick_angle [expr {$dial_arc_center*$dial_pi/180.0}]
        }

        # Sign convention: deflect RIGHT to advance clockwise, which is what
        # a left/right sweep to move round a circle intends.
        set rate [expr {$dial_stick_rate*$dial_pi/180.0}]
        set dial_stick_angle \
            [dial_norm2pi [expr {$dial_stick_angle - $f*$rate*$dt}]]
        set angle [dial_clamp_arc $dial_stick_angle]
        set dial_stick_angle $angle       ;# park at the arc edge, do not wrap past it

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

    # The stick's own push-select. Commits the INTEGRATED angle -- not the
    # deflection direction, which is what sliderconf's swipe path would
    # commit and is a different answer entirely.
    proc dial_stick_commit { dpoint data } {
        variable dial_active
        variable dial_armed_time
        variable dial_pending
        variable dial_stick_angle
        variable dial_stick_moved

        if { !$dial_active || $dial_armed_time == 0 } return
        if { ![string is entier -strict $data] || $data == 0 } return
        if { !$dial_stick_moved } return   ;# nothing steered yet: not a response

        set dial_pending [list [dial_clamp_arc $dial_stick_angle] stick]
        do_update
    }

    proc dial_poll_stick {} { return "" }

    # Mouse commits arrive asynchronously via dial_mouse_sample latching
    # dial_pending, which dial_response consumes before it polls sources.
    # Nothing to poll here.
    proc dial_poll_mouse {} { return "" }

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
            lassign $dial_pending dial_report_angle dial_report_source
            set dial_report_time   [now]
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
    proc dial_show { angle } { variable dial_cursor_shown
                               set dial_cursor_shown 1
                               dial_cursor_update $angle 1 }
    proc dial_hide {}        { variable dial_cursor_shown
                               set dial_cursor_shown 0
                               dial_pointer_hide }

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
        if { !$dial_active } { error "::ess::dial_simulate: no dial initialized" }
        if { $dial_armed_time == 0 } { error "::ess::dial_simulate: dial not armed" }
        # Latch it and wake the state machine; dial_response consumes it on
        # the next update. Applying it here instead would set the report and
        # then make it unreachable -- dial_response returns -1 once the dial
        # is disarmed, so the simulated angle never reached the protocol.
        set a [dial_clamp_arc [dial_norm2pi $angle_rad]]
        variable dial_pending; set dial_pending [list $a simulate]
        do_update
        return $a
    }
}

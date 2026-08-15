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

    # Mouse source. The mouse is an ABSOLUTE pointer in a declared extent,
    # so the dial reads mouse/event straight rather than going through the
    # slider's swipe machinery: direction is atan2 about the extent's
    # centre, and radius is ignored (a dial only has an angle).
    #
    # That makes the interaction move-to-choose, click-to-select, with no
    # button held while adjusting -- which is what a mouse is good at, and
    # a nicer fit for "choose, adjust, select" than press-drag-release.
    # Rig-level source binding, set in local/ and NOT cleared by
    # input_reset -- bindings describe the rig and persist across systems,
    # exactly as button_bind / joystick_bind do. Empty = unbound.
    variable dial_bound_sources  {}

    # How far the virtual cursor must leave the centre before the dial
    # takes a direction seriously. Below it the cursor stays HIDDEN, so a
    # stray count does not silently pick an angle and the subject's first
    # deliberate movement chooses the start -- which is what the trackpad's
    # press-then-drag gave for free.
    variable dial_start_radius   40

    # Has the subject chosen a direction yet this trial? The start radius
    # gates ACQUIRING one; it must not keep gating once one is chosen.
    # --- stick source (velocity steering) --------------------------------
    #
    # Direct pointing with a thumbstick was tried first and was HARDER than
    # the trackpad, which is what started this whole line of work. The
    # reasons are structural, not tuning: a stick has ~1cm of travel, so
    # atan2 of the deflection is twitchy near centre and only steady at
    # full push -- precision depends on how hard you hold it -- and it
    # self-centres, so releasing loses the direction and you can never park
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
    variable dial_stick_invert   0
    variable dial_stick_commit_dp "extio/*/state/group/stick_select"
    variable dial_stick_scale    0.0     ;# full-scale deflection, learned
    variable dial_stick_last_ts  0
    variable dial_stick_angle    0.0
    variable dial_stick_moved    0

    variable dial_mouse_acquired 0

    variable dial_mouse_range_known 0
    variable dial_mouse_cx 0.0
    variable dial_mouse_cy 0.0

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
        return $dial_bound_sources
    }

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

        # defaults on every init, so a protocol that omits an option gets
        # the documented value rather than whatever the previous protocol
        # set. Sources default to the RIG's binding when it has one, so a
        # protocol need not -- and should not -- name hardware.
        variable dial_bound_sources
        set dial_sources        [expr {[llength $dial_bound_sources] ?
                                       $dial_bound_sources : {swipe touch}}]
        set dial_deadband_deg   2.0
        set dial_arc_center     0.0
        set dial_arc_halfwidth  180.0
        set dial_radius         0.0
        set dial_ring_tolerance 0.0
        set dial_start_radius   40
        set dial_stick_rate     180.0
        set dial_stick_deadzone 0.08
        set dial_stick_invert   0
        set dial_stick_commit_dp "extio/*/state/group/stick_select"
        set dial_cursor_dpoint  ess/cursor

        foreach { k v } $args {
            switch -- $k {
                -sources        { set dial_sources $v }
                -deadband_deg   { set dial_deadband_deg $v }
                -arc_center     { set dial_arc_center $v }
                -arc_halfwidth  { set dial_arc_halfwidth $v }
                -radius         { set dial_radius $v }
                -ring_tolerance { set dial_ring_tolerance $v }
                -start_radius   { set dial_start_radius $v }
                -stick_rate     { set dial_stick_rate $v }
                -stick_deadzone { set dial_stick_deadzone $v }
                -stick_invert   { set dial_stick_invert [expr {$v ? 1 : 0}] }
                -stick_commit   { set dial_stick_commit_dp $v }
                -cursor_dpoint  { set dial_cursor_dpoint $v }
                default { error "::ess::dial_init: unknown option '$k'" }
            }
        }

        # Both read slider/position and would fight over the cursor.
        if { "swipe" in $dial_sources && "stick" in $dial_sources } {
            error "::ess::dial_init: swipe and stick are alternative readings\
                   of the same device -- choose one"
        }

        foreach s $dial_sources {
            if { $s ni {swipe touch joystick mouse stick} } {
                error "::ess::dial_init: unknown source '$s'\
                       (want swipe|touch|joystick|mouse|stick)"
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
        variable dial_stick_commit_dp
        catch { dpointRemoveScript $dial_stick_commit_dp ::ess::dial_stick_commit }
        catch { dpointRemoveScript mouse/event ::ess::dial_mouse_sample }
        catch { dpointRemoveScript mouse/event/range ::ess::dial_mouse_range }
        dial_disarm
        set dial_active 0
        dservSet ess/dial_active 0
        # Clear the companions too, so the panel cannot show a previous
        # system's sources or arc if it is ever displayed again.
        dservSet ess/dial/sources {}
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
        dservSet ess/dial/sources $dial_sources
    }

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
        variable dial_mouse_acquired
        set dial_mouse_acquired 0
        variable dial_stick_last_ts; set dial_stick_last_ts 0
        variable dial_stick_moved;   set dial_stick_moved 0
        variable dial_stick_angle;   set dial_stick_angle 0.0

        # Consume whatever is already sitting in the transports, so a commit
        # that landed before the window cannot be picked up as the first
        # response after it.
        set dial_swipe_last [slider_swipe_time]
        set dial_touch_last [dial_touch_timestamp]
        # Only if the joystick is OURS to arm -- a protocol may be using it
        # for something else entirely, and joystick_reset clears its latch.
        variable dial_sources
        if { "joystick" in $dial_sources } { joystick_reset }

        # Put the mouse's virtual cursor back in the middle. Without this
        # each trial resumes at the PREVIOUS trial's answer, which anchors
        # the next report, and the cursor eventually random-walks into a
        # corner where it clamps and whole directions stop being
        # reportable. Recentred, the trial starts from nothing and the
        # subject's first movement picks the direction.
        if { "mouse" in $dial_sources } {
            catch { send input "inputRecenter mouse" }
        }
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

    # The extent's centre, from mouse/event/range ([0 max_x 0 max_y]).
    # Needed before any angle can be computed, so a sample arriving first
    # is simply dropped rather than measured from (0,0).
    proc dial_mouse_range { dpoint data } {
        variable dial_mouse_range_known
        variable dial_mouse_cx
        variable dial_mouse_cy
        lassign $data minx maxx miny maxy
        if { $maxx eq "" || $maxy eq "" } return
        set dial_mouse_cx [expr {($minx + $maxx)/2.0}]
        set dial_mouse_cy [expr {($miny + $maxy)/2.0}]
        set dial_mouse_range_known 1
    }

    # Mouse as an absolute pointer: move to choose and adjust, click to
    # select.
    #
    # MOVE (3) and DRAG (1) both steer -- whether a button happens to be
    # held while adjusting should not change what the cursor does. PRESS
    # (0) commits, because button-down is the moment of decision and gives
    # the cleaner reaction time; RELEASE (2) is ignored so a click is one
    # response, not two.
    #
    # Radius is ignored -- a dial has only an angle. A non-square extent
    # therefore costs nothing: it means some directions are reachable at
    # larger radius than others, while the angle itself stays true because
    # pixels are square.
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

        if { !$dial_active || $dial_armed_time == 0 } return
        if { !$dial_mouse_range_known } return

        lassign $data x y ev
        if { $x eq "" || $y eq "" } return

        # Published y grows DOWNWARD (mouse_reader keeps REL_Y's sense, the
        # same as a touchscreen's ABS_Y), so flip it into the dial's world.
        set dx [expr {$x - $dial_mouse_cx}]
        set dy [expr {$dial_mouse_cy - $y}]

        # The start radius gates ACQUIRING a direction, once per trial --
        # not maintaining one.
        #
        # Gating it continuously made the cursor blink: sweeping the mouse
        # horizontally to move round the circle carries the cursor straight
        # through the centre, and every crossing dropped inside the radius
        # and hid it. Once a direction has been chosen the cursor stays put
        # and simply HOLDS its last angle through the dead zone, where
        # there is no meaningful direction to update to.
        variable dial_start_radius
        variable dial_mouse_acquired
        set inside [expr {($dx*$dx + $dy*$dy) <
                          ($dial_start_radius*$dial_start_radius)}]

        if { $inside && !$dial_mouse_acquired } {
            # Nothing chosen yet: show nothing, and a PRESS here is not a
            # response -- the subject has not picked anything.
            return
        }
        if { $inside } { return }   ;# acquired: hold the last angle
        set dial_mouse_acquired 1

        set angle [dial_clamp_arc [dial_norm2pi [expr {atan2($dy, $dx)}]]]

        if { $ev == 0 } {
            # SELECT. Latched as pending and consumed by dial_response, so
            # a mouse commit takes the same path into the protocol as
            # every other source.
            set dial_pending [list $angle mouse]
            do_update
            return
        }
        if { $ev == 2 } return   ;# button-up is not a second response

        # CHOOSE / ADJUST -- steer the cursor, do NOT wake the state
        # machine (a 1 kHz mouse would otherwise drive the SM at 1 kHz).
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

    # Velocity steering from a self-centring stick.
    #
    # Reads slider/position, so it inherits sliderconf's centre, deadzone,
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
        # rate is expressed relative to this stick's actual travel rather
        # than to a calibration constant that would have to be maintained
        # per rig.
        set ax [expr {abs($x)}]
        if { $ax > $dial_stick_scale } { set dial_stick_scale $ax }
        if { $dial_stick_scale <= 0 } return

        set f [expr {$x/$dial_stick_scale}]
        if { abs($f) < $dial_stick_deadzone } return   ;# at rest: no drift
        if { $dial_stick_invert } { set f [expr {-$f}] }

        # First deflection of the trial starts the cursor at the arc centre
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
        variable dial_pending; set dial_pending [list $a simulate]
        do_update
        return $a
    }
}

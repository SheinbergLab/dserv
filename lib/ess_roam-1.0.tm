# -*- mode: tcl -*-
#
# ess_roam-1.0.tm
#
# The "roam" response mode: the subject drives an agent FREELY around a
# bounded arena for as long as the trial lasts. Foraging, patch-leaving,
# navigation, open-field search.
#
# Written from docs/ess_roam_sketch.md, whose argument for a separate module
# still holds and is worth restating, because the obvious move is to add a
# source to ::ess::dial and it is the wrong one:
#
#   a dial   arms a window, moves a cursor, takes ONE commit, disarms. Every
#            variable in it is per-response state, and its ring is where the
#            answer is GIVEN.
#   a roam   has no commit and no window. The agent moves for the whole obs
#            period and what you measure is a TRAJECTORY. Its boundary is a
#            WALL -- the opposite meaning on the same geometry.
#
# The dial's `sectors` source is rail-guided by construction: it holds one
# radius on one spoke and reaches a new direction by retracting through the
# center. That is a feature there (four switches cannot express a bearing
# between sectors, so the report stays on one of eight) and it is exactly
# what a roaming agent must not do.
#
# The layering is the dial's, unchanged:
#
#   roam owns       integration, arena geometry, the edge rule, the agent's
#                   pose, and testing it against regions
#   the protocol    what a region MEANS -- what being in one earns, whether
#   owns            the trial ends, what to reward
#
# Sources: sectors | rate.
#
#   sectors  ess/joystick/dir (0..7 clockwise from up, -1 centered) sets a
#            VELOCITY DIRECTION; a timer integrates it. Eight headings, one
#            speed. This is the d-pad reading, and the one this module was
#            built for: a free 2-D position driven by four switches.
#   rate     slider/position: the deflection vector IS the velocity, so the
#            heading is continuous and the speed is the subject's. Needs an
#            analog stick and a rig that declares slider/full_scale.
#
# Both feed ONE integrator (roam_step), so the arena, the edge rule, the
# region tests and the publishing exist once. Adding a third source is a
# velocity function, not another copy of this machine -- which is the lesson
# ess_dial was extracted to record.
#
# THE STATE MACHINE IS NOT WOKEN ON MOTION. Every cursor source in ess_dial
# already refuses to, and there it is a nicety because a reach lasts under a
# second. Here the agent moves for the entire obs period: at a 8 ms tick a
# 10 s trial is 1250 steps, and waking the SM on each is 1250 evaluations of
# every transition in the system. do_update is called on a REGION TRANSITION
# and nowhere else -- entering or leaving a patch is the event the protocol
# is waiting for.
#
# Pose is published as a DSERV_FLOAT triple, degrees and ms:
#
#   ess/roam/pos       "x y t" as binary fff, position in degrees with the
#                      origin at screen center, t in ms since roam_start
#
# x and y lead deliberately. dserv's C `windows` processor tests a point
# against N regions and publishes only on state change, and processAttach
# binds it to ANY datapoint -- so this pose can be handed to it unchanged
# if the region count or the sample rate ever outgrows the Tcl test below.
# windows.c takes float_vals[0] and [1] after a `len >= 2*sizeof(float)`
# check, so the trailing t rides along without it noticing. The region API
# here is deliberately em_region_set's shape so that swap changes no
# protocol.
#
# t is the newer half of the contract; roam_publish_pose says why a
# buffered publish-on-change stream has to carry its own time. Files
# written before it are 2-float, and ess/roam/layout in the file says
# which one you have.
#
# Companion datapoints, all strings, for things that DRAW:
#
#   ess/roam/geometry  "circle,R" | "rect,x0,x1,y0,y1"
#   ess/roam/layout    "x,y" | "x,y,t" -- the pose payload's decode key
#   ess/roam/regions   "states_bitmask,changed_bitmask"
#   ess/roam/wall      0|1  -- is the agent against the boundary right now
#   ess/dial/pointer   "x,y,show,in_band"  (see roam_init -pointer_dpoint)
#
# The last one is borrowed on purpose. `ess/dial/pointer` is the established
# LIVE CURSOR channel -- ess_control.html draws it, and four stim files
# already subscribe to it -- and a roamed agent is drawn exactly the way a
# dial's cursor is. Publishing a second, identical channel would mean every
# consumer learning which response mode is loaded in order to draw the same
# dot. Roam does NOT set ess/dial_active: it is not a dial, it just writes
# the cursor everything already reads.
#

package provide ess_roam 1.0

namespace eval ess {

    variable roam_pi 3.14159265358979

    # --- configuration (roam_init) ---------------------------------------
    variable roam_sources        {sectors}
    variable roam_valid_sources  {sectors rate}
    variable roam_arena          {}       ;# {circle R} | {rect x0 x1 y0 y1}
    variable roam_edge           clamp
    variable roam_pose_dpoint    ess/roam/pos
    variable roam_pointer_dpoint ess/dial/pointer
    # What a pose payload holds, published as ess/roam/layout and recorded
    # so a reader can tell a 3-float file from the 2-float ones written
    # before roam_publish_pose started carrying its own time.
    variable roam_pose_layout    x,y,t

    # sectors source: eight headings at one speed
    variable roam_rate           8.0      ;# deg/s
    variable roam_accel          0.0      ;# extra deg/s per second held
    variable roam_rate_max       0.0      ;# 0 = uncapped
    variable roam_tick_ms        8        ;# 16 beats with a 60 Hz display

    # rate source: the deflection vector is the velocity
    variable roam_scale          0.0      ;# full-scale deflection; 0 = unset
    variable roam_deadzone       0.08
    variable roam_expo           2.0
    # A gap larger than this means samples stopped arriving. Integrating the
    # whole gap teleports the agent; discarding it freezes one that was
    # being driven the entire time. Capping is the least-wrong reading --
    # the same conclusion, and the same number, ess_dial reached after
    # measuring a teensy box stalling for up to 175 ms.
    variable roam_max_dt         0.05

    # pointer publication throttle (the pose is never throttled -- it is the
    # data). 0 = publish whenever the agent moves.
    variable roam_pub_ms         0
    variable roam_min_step       0.002

    # --- live state -------------------------------------------------------
    variable roam_active         0
    variable roam_moving         0        ;# roam_start/roam_stop gate
    variable roam_started_us     0
    variable roam_x              0.0
    variable roam_y              0.0
    variable roam_path           0.0      ;# integrated distance, degrees
    variable roam_on_wall        0
    variable roam_wall_hits      0        ;# 0->1 transitions since start

    # sectors source
    variable roam_sector         -1
    variable roam_timer          ""
    variable roam_last_us        0
    variable roam_hold_us        0

    # rate source
    variable roam_last_ts        0
    variable roam_n_dup          0
    variable roam_n_gap          0

    # regions. roam_region(win) = {active type cx cy hx hy}
    variable roam_region
    array set roam_region {}
    variable roam_region_states  0
    variable roam_entered_win    -1
    variable roam_entered_us     0

    # publication bookkeeping
    variable roam_last_pub       0
    variable roam_pub_x          0.0
    variable roam_pub_y          0.0
    variable roam_pub_band       -1
    variable roam_shown          0

    ########################################################################
    # arena geometry
    ########################################################################

    # ::ess::roam_set_arena circle R
    # ::ess::roam_set_arena rect x0 x1 y0 y1
    #
    # Per-trial, separate from roam_init for the same reason dial_set_radius
    # is: the boundary usually comes from stimdg and can change every trial,
    # while the source list and the tuning do not.
    #
    # Called with no arguments it REPORTS, which is this file's convention
    # throughout -- a read that writes is a trap for anything displaying
    # state.
    proc roam_set_arena { args } {
        variable roam_arena
        if { [llength $args] == 0 } { return $roam_arena }
        set kind [lindex $args 0]
        switch -exact -- $kind {
            circle {
                if { [llength $args] != 2 } {
                    error "::ess::roam_set_arena: circle wants one radius"
                }
                set r [expr {double([lindex $args 1])}]
                if { $r <= 0 } {
                    error "::ess::roam_set_arena: circle radius must be > 0"
                }
                set roam_arena [list circle $r]
            }
            rect {
                if { [llength $args] != 5 } {
                    error "::ess::roam_set_arena: rect wants x0 x1 y0 y1"
                }
                lassign [lrange $args 1 end] x0 x1 y0 y1
                if { $x1 <= $x0 || $y1 <= $y0 } {
                    error "::ess::roam_set_arena: rect wants x0 < x1 and\
                           y0 < y1, got {$x0 $x1 $y0 $y1}"
                }
                set roam_arena [list rect [expr {double($x0)}] \
                                    [expr {double($x1)}] \
                                    [expr {double($y0)}] [expr {double($y1)}]]
            }
            default {
                error "::ess::roam_set_arena: want circle|rect, got '$kind'"
            }
        }
        roam_publish_geometry
        return $roam_arena
    }

    # Bring a position inside the arena. Returns {x y hit}, where hit is 1 if
    # the point was outside and had to be pulled back.
    #
    # Circle clamping is RADIAL, which is what makes a wall behave like one:
    # pushing along the boundary keeps the tangential component and the agent
    # SLIDES rather than sticking. Clamping the offending axis (the rect
    # rule) does the same thing for a box.
    proc roam_clamp { x y } {
        variable roam_arena
        if { ![llength $roam_arena] } { return [list $x $y 0] }
        switch -exact -- [lindex $roam_arena 0] {
            circle {
                set r [lindex $roam_arena 1]
                set d [expr {sqrt($x*$x + $y*$y)}]
                if { $d <= $r || $d == 0.0 } { return [list $x $y 0] }
                set k [expr {$r/$d}]
                return [list [expr {$x*$k}] [expr {$y*$k}] 1]
            }
            rect {
                lassign [lrange $roam_arena 1 end] x0 x1 y0 y1
                set hit 0
                if { $x < $x0 } { set x $x0; set hit 1 }
                if { $x > $x1 } { set x $x1; set hit 1 }
                if { $y < $y0 } { set y $y0; set hit 1 }
                if { $y > $y1 } { set y $y1; set hit 1 }
                return [list $x $y $hit]
            }
        }
        return [list $x $y 0]
    }

    proc roam_publish_geometry {} {
        variable roam_arena
        dservSet ess/roam/geometry [join $roam_arena ,]
    }

    ########################################################################
    # regions -- what the protocol calls a patch, a target, a goal
    #
    # Deliberately em_region_set's shape (win index, type, center, half
    # widths, an on/off flag, a states bitmask, and a per-window query), so
    # a protocol written against this reads like one written against eye
    # windows, and so the C windows processor can replace the test below
    # without touching a protocol.
    #
    # The test IS in Tcl here, and that is a considered choice rather than a
    # shortcut: this module's first user has at most 8 regions on a 125 Hz
    # tick -- 1000 distance comparisons a second, against a state machine
    # that evaluates every transition in the system on each wake. Attaching
    # a second `windows` processor instance would cost a config/triggers.tcl
    # edit on every rig, and the ain param API selects a processor GLOBALLY
    # (ainSetProcessor) while em_region_on assumes its selection survives --
    # so a second instance is a live hazard to eye windows, not just extra
    # wiring. When the region count or the rate makes it worth it, the pose
    # datapoint is already in the format the processor wants.
    ########################################################################

    # type 1 = circle (hx is the radius), type 0 = rect (hx, hy half-widths)
    proc roam_region_set { win type cx cy hx { hy {} } } {
        variable roam_region
        # The states are a BITMASK, matching em_windows, so the index is
        # bounded. Said here rather than discovered as a region that simply
        # never reports being entered.
        if { ![string is entier -strict $win] || $win < 0 || $win > 31 } {
            error "::ess::roam_region_set: win must be 0..31, got '$win'"
        }
        if { $hy eq "" } { set hy $hx }
        set roam_region($win) [list 1 $type [expr {double($cx)}] \
                                   [expr {double($cy)}] \
                                   [expr {double($hx)}] [expr {double($hy)}]]
        return
    }

    # The common case: a circular patch of radius r at (cx, cy).
    proc roam_patch_set { win cx cy r } {
        roam_region_set $win 1 $cx $cy $r
    }

    proc roam_region_on { win } {
        variable roam_region
        if { ![info exists roam_region($win)] } {
            error "::ess::roam_region_on: region $win is not set"
        }
        lset roam_region($win) 0 1
    }

    proc roam_region_off { win } {
        variable roam_region
        if { [info exists roam_region($win)] } {
            lset roam_region($win) 0 0
        }
    }

    # Clear ALL regions. Called from roam_start's protocol-facing siblings
    # rather than automatically: a protocol whose patches do not change
    # between trials should not have to rebuild them.
    proc roam_regions_clear {} {
        variable roam_region
        variable roam_region_states
        array unset roam_region
        array set roam_region {}
        set roam_region_states 0
    }

    proc roam_in_region { win } {
        variable roam_region_states
        return [expr {($roam_region_states & (1 << $win)) != 0}]
    }

    proc roam_regions {} {
        variable roam_region_states
        return $roam_region_states
    }

    # The FIRST region entered since roam_start, or -1. Latched, so a
    # transition can read it at its leisure -- the same contract as
    # joystick_response, and for the same reason: the agent may already have
    # left the patch by the time the state machine looks.
    proc roam_entered {} {
        variable roam_entered_win
        return $roam_entered_win
    }

    # ... and WHEN, in dserv-clock microseconds. This is the response's
    # timestamp: the moment the agent arrived, not the moment the hand first
    # moved.
    proc roam_entered_time {} {
        variable roam_entered_us
        return $roam_entered_us
    }

    proc roam_clear_entry {} {
        variable roam_entered_win
        variable roam_entered_us
        set roam_entered_win -1
        set roam_entered_us  0
    }

    # Test the current pose against every active region and publish/wake ONLY
    # on a change of state. Returns the new bitmask.
    #
    # wake 0 SEEDS instead: the states are recomputed and published, but no
    # entry is latched and the state machine is not woken. roam_start uses it
    # so that placing the agent inside a patch and then starting does not
    # register as an arrival nobody made -- and so that a do_update does not
    # re-enter the state machine from inside the action that called
    # roam_start.
    proc roam_test_regions { ts { wake 1 } } {
        variable roam_region
        variable roam_region_states
        variable roam_entered_win
        variable roam_entered_us
        variable roam_x
        variable roam_y

        set states 0
        foreach win [array names roam_region] {
            lassign $roam_region($win) active type cx cy hx hy
            if { !$active } continue
            set dx [expr {$roam_x - $cx}]
            set dy [expr {$roam_y - $cy}]
            if { $type == 1 } {
                set in [expr {($dx*$dx + $dy*$dy) <= $hx*$hx}]
            } else {
                set in [expr {abs($dx) <= $hx && abs($dy) <= $hy}]
            }
            if { $in } { set states [expr {$states | (1 << $win)}] }
        }

        if { $states == $roam_region_states && $wake } { return $states }

        set changed [expr {$states ^ $roam_region_states}]
        set entered [expr {$changed & $states}]
        set roam_region_states $states

        if { !$wake } {
            dservSet ess/roam/regions "$states,0"
            return $states
        }

        # Latch the first ENTRY of this bout. Lowest window index wins when
        # two are entered on one step -- which only happens if the protocol
        # overlapped its patches, and picking deterministically beats
        # picking by array traversal order.
        if { $entered && $roam_entered_win < 0 } {
            for { set w 0 } { $w < 32 } { incr w } {
                if { $entered & (1 << $w) } {
                    set roam_entered_win $w
                    set roam_entered_us  $ts
                    break
                }
            }
        }

        dservSet ess/roam/regions "$states,$changed"

        # THE one place this module wakes the state machine. See the header.
        do_update
        return $states
    }

    ########################################################################
    # the integrator -- one for every source
    ########################################################################

    # Advance the agent by a velocity held for dt seconds, then clamp,
    # publish and test. ts is the sample's time in dserv-clock microseconds:
    # the box's stamp where there is one, so a region entry carries the
    # hardware's time rather than Tcl's.
    proc roam_step { vx vy dt ts } {
        variable roam_moving
        variable roam_x
        variable roam_y
        variable roam_path
        variable roam_on_wall
        variable roam_wall_hits

        if { !$roam_moving } return
        if { $dt <= 0 } return

        set nx [expr {$roam_x + $vx*$dt}]
        set ny [expr {$roam_y + $vy*$dt}]
        lassign [roam_clamp $nx $ny] nx ny hit

        set dx [expr {$nx - $roam_x}]
        set dy [expr {$ny - $roam_y}]
        set moved [expr {sqrt($dx*$dx + $dy*$dy)}]

        set roam_x $nx
        set roam_y $ny
        set roam_path [expr {$roam_path + $moved}]

        # Wall contacts are counted on the 0->1 TRANSITION, not per step:
        # held against the boundary the agent is in contact for hundreds of
        # ticks, and "how often did it run into the wall" is the measure
        # anyone actually wants.
        if { $hit != $roam_on_wall } {
            set roam_on_wall $hit
            if { $hit } { incr roam_wall_hits }
            dservSet ess/roam/wall $hit
        }

        # Publish the pose only when it CHANGED. Held against the wall the
        # position is constant, and republishing it at the tick rate would
        # write hundreds of identical samples into the trajectory record.
        if { $moved > 0.0 } { roam_publish_pose $ts }
        roam_publish_pointer $ts

        roam_test_regions $ts
    }

    # The trajectory record. DSERV_FLOAT triple {x y t}, degrees and ms,
    # origin at screen center -- the eyetracking/position contract for the
    # first two, plus the sample's OWN time.
    #
    # WHY THE TIME IS IN THE PAYLOAD. This stream is logged buffered
    # (ess-2.0.tm), and dserv's log buffer concatenates the data of
    # successive datapoints while keeping only the FIRST one's timestamp
    # -- so ten poses reach the file under one stamp. That is fine for a
    # uniformly sampled source, whose sample times can be counted off the
    # block. It is NOT fine here, because the pose publishes only when the
    # agent MOVED: a block spanning 160 ms had a pause somewhere in it and
    # nothing in the file says where.
    #
    # The fix is the one the extio ain blocks already use -- carry the
    # time in the payload rather than relying on the dserv envelope --
    # and it is much cheaper than the alternatives. Measured on a
    # 160-trial forage session: 2 floats buffered 560 KB (times inferred),
    # 2 floats UNBUFFERED 2084 KB (times exact, buffering given up), 3
    # floats buffered 751 KB (times exact, buffering kept). The +27% buys
    # exactness and costs nothing downstream -- a trials dg carries three
    # floats per sample either way, the third one just stops being a
    # reconstruction.
    #
    # t is ms since roam_start, so the bout's first sample is 0.0.
    # float32 represents integer ms exactly to 2^24 = 4.7 hours, well past
    # any obs period. Extraction does not depend on the origin anyway: it
    # reads each sample as record_stamp + (t - t_of_that_record's_first),
    # which stays exact across a roam restarted mid-obs.
    #
    proc roam_publish_pose { ts } {
        variable roam_pose_dpoint
        variable roam_started_us
        variable roam_x
        variable roam_y
        if { $ts <= 0 } { set ts [now] }
        dservSetData $roam_pose_dpoint $ts 2 \
            [binary format fff $roam_x $roam_y \
                 [expr {($ts - $roam_started_us)/1000.0}]]  ;# 2 = DSERV_FLOAT
    }

    # The drawable cursor, throttled: a stim redrawing at the display's rate
    # gains nothing from more than one update per frame, and this crosses a
    # socket to another process.
    #
    # in_band reports "the agent is inside some active region", which is the
    # affordance a stim wants to light. A change of band is NEVER throttled,
    # for the reason ess_dial documents: the highlight has to be crisp even
    # when the agent is barely moving.
    proc roam_publish_pointer { ts } {
        variable roam_pointer_dpoint
        variable roam_pub_ms
        variable roam_min_step
        variable roam_last_pub
        variable roam_pub_x
        variable roam_pub_y
        variable roam_shown
        variable roam_region_states
        variable roam_x
        variable roam_y

        set band [expr {$roam_region_states != 0}]
        variable roam_pub_band
        if { ![info exists roam_pub_band] } { set roam_pub_band -1 }

        set due [expr {$roam_pub_ms <= 0 ||
                       ($ts - $roam_last_pub)/1000.0 >= $roam_pub_ms}]
        set moved [expr {abs($roam_x - $roam_pub_x) +
                         abs($roam_y - $roam_pub_y)}]
        if { $roam_shown && $band == $roam_pub_band &&
             !($due && $moved > $roam_min_step) } return

        set roam_last_pub $ts
        set roam_pub_x    $roam_x
        set roam_pub_y    $roam_y
        set roam_pub_band $band
        set roam_shown    1
        dservSet $roam_pointer_dpoint \
            "[format %.4f $roam_x],[format %.4f $roam_y],1,$band"
    }

    proc roam_pointer_hide {} {
        variable roam_pointer_dpoint
        variable roam_shown
        variable roam_pub_band
        set roam_shown    0
        set roam_pub_band -1
        dservSet $roam_pointer_dpoint "0.0000,0.0000,0,0"
    }

    ########################################################################
    # sectors source -- eight headings from four switches
    ########################################################################

    # Speed right now. THE place acceleration lives, so a different curve is
    # a change here and nowhere else.
    #
    # Note the default is 0: with a foraging trial the agent is steering for
    # its whole duration, and a speed that keeps growing under it makes the
    # arena effectively smaller the longer the animal searches.
    proc roam_rate_now { held_s } {
        variable roam_rate
        variable roam_accel
        variable roam_rate_max
        set r [expr {$roam_rate + $roam_accel*$held_s}]
        if { $roam_rate_max > 0 && $r > $roam_rate_max } {
            set r $roam_rate_max
        }
        return $r
    }

    # ess/joystick/dir carries the SECTOR (0..7 clockwise from up) or -1 for
    # centered, republished by joystick_ingest on every state change.
    #
    # This is where roam and the dial's `sectors` source part company: a new
    # direction is adopted IMMEDIATELY. The dial retracts to the center
    # first, because its cursor is only ever on a spoke and its report is one
    # of eight bearings. Here the agent's position is 2-D and the path
    # between two pushes is the thing being measured, so turning has to be
    # turning.
    proc roam_dir { dpoint data } {
        variable roam_active
        variable roam_moving
        variable roam_sector
        variable roam_timer
        variable roam_last_us
        variable roam_hold_us

        if { !$roam_active || !$roam_moving } return
        if { ![string is entier -strict $data] } return

        set d [expr {int($data)}]
        if { $d == $roam_sector } return

        # Acceleration is per-heading: a new push is a new hold. Without
        # this, flicking between two directions would inherit whatever speed
        # the previous one had ramped to.
        set roam_hold_us [now]

        if { $d < 0 } {
            # Centered. The agent HOLDS where it got to rather than decaying
            # home -- progress made in a short push is kept, so an animal
            # that cannot yet produce a sustained hold can still cross the
            # arena in bursts. Requiring the hold is a later criterion, and
            # it is a decay rule here, not a redesign.
            set roam_sector -1
            roam_stop_timer
            return
        }

        set roam_sector $d
        if { $roam_timer eq "" } {
            set roam_last_us [now]
            roam_tick
        }
    }

    proc roam_stop_timer {} {
        variable roam_timer
        if { $roam_timer ne "" } {
            catch { dservAfterCancel $roam_timer }
            set roam_timer ""
        }
    }

    proc roam_tick {} {
        variable roam_active
        variable roam_moving
        variable roam_sector
        variable roam_timer
        variable roam_tick_ms
        variable roam_last_us
        variable roam_hold_us
        variable roam_pi

        # RESCHEDULE FIRST, so that roam_stop_timer is the only thing that
        # ends the walk. Rearming on the last line makes every early return
        # -- including one added later, or an unexpected error -- a silent
        # freeze of the agent with nothing to say why. ess_dial learned this
        # the hard way; there is no reason to learn it twice.
        set roam_timer [dservAfter $roam_tick_ms ::ess::roam_tick]

        if { !$roam_active || !$roam_moving } { roam_stop_timer; return }
        if { $roam_sector < 0 } { roam_stop_timer; return }

        set t  [now]
        set dt [expr {($t - $roam_last_us)/1.0e6}]
        set roam_last_us $t
        # A gap this large means the interp was busy elsewhere; integrating
        # across it would teleport the agent.
        if { $dt <= 0 || $dt > 0.25 } { return }

        set held  [expr {($t - $roam_hold_us)/1.0e6}]
        set speed [roam_rate_now $held]
        set a     [expr {(90.0 - 45.0*$roam_sector)*$roam_pi/180.0}]

        roam_step [expr {$speed*cos($a)}] [expr {$speed*sin($a)}] $dt $t
    }

    ########################################################################
    # rate source -- the deflection vector is the velocity
    ########################################################################

    proc roam_sample { dpoint data } {
        variable roam_active
        variable roam_moving
        variable roam_scale
        variable roam_deadzone
        variable roam_expo
        variable roam_rate
        variable roam_last_ts
        variable roam_max_dt
        variable roam_n_dup
        variable roam_n_gap

        if { !$roam_active || !$roam_moving } return
        if { $roam_scale <= 0 } return

        lassign $data sx sy
        if { $sx eq "" } return
        if { $sy eq "" } { set sy 0.0 }

        # dt from the DATAPOINT, which carries the box's stamp, not a Tcl
        # clock -- so the travel rate does not wander with scheduler jitter.
        set ts [dservTimestamp $dpoint]
        if { $roam_last_ts == 0 } { set roam_last_ts $ts; return }
        set dt [expr {($ts - $roam_last_ts)/1.0e6}]
        set roam_last_ts $ts

        # Counted rather than silently skipped: "the box duplicates
        # timestamps" and "the box stalled" are claims that should be
        # checkable from the rig (roam_tune reports both).
        if { $dt <= 0 } { incr roam_n_dup; return }
        if { $dt > $roam_max_dt } { incr roam_n_gap; set dt $roam_max_dt }

        # Speed from the MAGNITUDE, direction from the raw vector -- never
        # per-axis. ::ess::stick_velocity is shared with the dial's two
        # analog sources; see its comment for why axis-wise shaping bows the
        # path away from where the hand is pointing.
        lassign [stick_velocity $sx $sy $roam_scale $roam_deadzone \
                     $roam_expo $roam_rate] vx vy
        if { $vx == 0.0 && $vy == 0.0 } return    ;# at rest: agent holds

        roam_step $vx $vy $dt $ts
    }

    ########################################################################
    # lifecycle
    ########################################################################

    proc roam_init { args } {
        variable roam_sources
        variable roam_valid_sources
        variable roam_arena
        variable roam_edge
        variable roam_pose_dpoint
        variable roam_pointer_dpoint
        variable roam_pose_layout
        variable roam_rate
        variable roam_accel
        variable roam_rate_max
        variable roam_tick_ms
        variable roam_scale
        variable roam_deadzone
        variable roam_expo
        variable roam_max_dt
        variable roam_pub_ms
        variable roam_min_step
        variable roam_active

        # Defaults on every init, so a protocol that omits an option gets the
        # documented value rather than whatever the previous protocol set.
        # Every namespace variable assigned below MUST be declared above --
        # without the declaration `set` makes a LOCAL and the option is
        # silently ignored, which is a bug ess_dial shipped more than once.
        set roam_sources        {sectors}
        set roam_arena          {}
        set roam_edge           clamp
        set roam_pose_dpoint    ess/roam/pos
        set roam_pointer_dpoint ess/dial/pointer
        set roam_rate           8.0
        set roam_accel          0.0
        set roam_rate_max       0.0
        set roam_tick_ms        8
        set roam_scale          0.0
        set roam_deadzone       0.08
        set roam_expo           2.0
        set roam_max_dt         0.05
        set roam_pub_ms         0
        set roam_min_step       0.002

        set arena_args {}
        foreach { k v } $args {
            switch -- $k {
                -sources   { set roam_sources $v }
                -arena     { set arena_args $v }
                -edge      { set roam_edge $v }
                -rate      { set roam_rate      [expr {double($v)}] }
                -accel     { set roam_accel     [expr {double($v)}] }
                -rate_max  { set roam_rate_max  [expr {double($v)}] }
                -tick_ms   { set roam_tick_ms   [expr {int($v)}] }
                -scale     { set roam_scale     [expr {double($v)}] }
                -deadzone  { set roam_deadzone  [expr {double($v)}] }
                -expo      { set roam_expo      [expr {double($v)}] }
                -max_dt    { set roam_max_dt    [expr {double($v)}] }
                -pub_ms    { set roam_pub_ms    [expr {int($v)}] }
                -min_step  { set roam_min_step  [expr {double($v)}] }
                -pose_dpoint    { set roam_pose_dpoint $v }
                -pointer_dpoint { set roam_pointer_dpoint $v }
                default { error "::ess::roam_init: unknown option '$k'" }
            }
        }

        # The dial accepts `dpad`/`astick` forever because protocols were
        # written against them. This module is new, so it takes only the
        # strategy words -- but it says so, rather than reporting "unknown
        # source" to someone reasonably carrying a habit over.
        # (docs/input_vocabulary.md carries the device/strategy rule.)
        set roam_sources [lmap s $roam_sources {
            switch -exact -- $s {
                dpad   { error "::ess::roam_init: `dpad` is a DEVICE word;\
                                this module takes reading strategies -- use\
                                `sectors`" }
                astick { error "::ess::roam_init: `astick` is a DEVICE word;\
                                this module takes reading strategies -- use\
                                `rate`" }
                default { set s }
            }
        }]
        foreach s $roam_sources {
            if { $s ni $roam_valid_sources } {
                error "::ess::roam_init: unknown source '$s'\
                       (want [join $roam_valid_sources |])"
            }
        }
        # Both read one hand. Two of them is one hand steering two ways at
        # once, which reads on screen as an agent that will not track.
        if { [llength $roam_sources] > 1 } {
            error "::ess::roam_init: {$roam_sources} are alternative readings\
                   of the same device -- choose one"
        }

        if { $roam_edge ne "clamp" } {
            error "::ess::roam_init: -edge $roam_edge is not implemented.\
                   clamp is the honest default (a wall is a wall); wrap makes\
                   an infinite plane and breaks path-length interpretation,\
                   and bounce teaches the subject a physics they did not ask\
                   to learn. See docs/ess_roam_sketch.md"
        }

        roam_deinit                 ;# idempotent; drops any previous wiring

        # A fresh roam starts at the origin. Without this the agent inherits
        # the last protocol's final position, which is invisible until a
        # protocol that forgets roam_place starts its first bout somewhere
        # nobody chose.
        variable roam_x; set roam_x 0.0
        variable roam_y; set roam_y 0.0
        # Regions go too: they describe a TRIAL, and a new roam has no trial.
        # Consequence worth knowing -- re-running roam_init from a live param
        # apply (a protocol switching cursor_mode mid-session) drops the
        # current trial's patches, so that trial can no longer be completed.
        # nexttrial rebuilds them, so the cost is one trial, and changing the
        # input device mid-trial is already a disruption.
        roam_regions_clear

        if { [llength $arena_args] } { roam_set_arena {*}$arena_args }

        if { "sectors" in $roam_sources } {
            dservAddExactMatch ess/joystick/dir
            dpointAddScript    ess/joystick/dir ::ess::roam_dir
        }
        if { "rate" in $roam_sources } {
            dservAddExactMatch slider/position
            dpointAddScript    slider/position ::ess::roam_sample
        }

        set roam_active 1
        dservSet ess/roam_active  1
        dservSet ess/roam/sources $roam_sources
        # The decode key for ess/roam/pos, in the file -- the same job
        # ess/ain/recorded does for the analog blocks. A reader that finds
        # it knows the payload carries its own time; one that does not is
        # holding a 2-float file and has to reconstruct.
        dservSet ess/roam/layout  $roam_pose_layout
        roam_publish_geometry
        roam_pointer_hide
        return
    }

    # Always publishes the gate, even when no roam was active: a gate
    # datapoint has to be ASSERTED, because absent and false look identical
    # to the publisher and completely different to a subscriber holding a
    # stale value. Every step is idempotent, so none of it needs guarding.
    proc roam_deinit {} {
        variable roam_active
        variable roam_moving
        catch { dpointRemoveScript ess/joystick/dir ::ess::roam_dir }
        catch { dpointRemoveScript slider/position  ::ess::roam_sample }
        roam_stop_timer
        set roam_moving 0
        set roam_active 0
        dservSet ess/roam_active  0
        dservSet ess/roam/sources {}
        roam_pointer_hide
        return
    }

    # Begin moving. Everything a trajectory is measured against resets here:
    # the path integral, the wall count, the entry latch, and the source's
    # own dt anchor (so the first sample establishes dt rather than
    # integrating across the inter-trial gap).
    #
    # The agent is NOT moved -- use roam_place for that, before starting.
    # Where a bout begins is the protocol's business: at the center for a
    # reaching task, where the last one ended for a continuous forage.
    proc roam_start {} {
        variable roam_active
        variable roam_arena
        variable roam_moving
        variable roam_started_us
        variable roam_path
        variable roam_wall_hits
        variable roam_on_wall
        variable roam_sector
        variable roam_last_us
        variable roam_hold_us
        variable roam_last_ts
        variable roam_n_dup
        variable roam_n_gap
        variable roam_sources
        variable roam_scale

        if { !$roam_active } {
            error "::ess::roam_start: no roam is initialized"
        }
        if { ![llength $roam_arena] } {
            error "::ess::roam_start: no arena. A roam without a boundary is\
                   an agent that walks off the display -- set one with\
                   roam_init -arena or roam_set_arena"
        }
        # The same refusal dial_arm makes for an unscaled astick: a source
        # that cannot move should say so rather than sit motionless.
        if { "rate" in $roam_sources && $roam_scale <= 0 } {
            error "::ess::roam_start: -sources rate with no -scale, so a full\
                   push means nothing. Read it from slider/full_scale"
        }

        set roam_path       0.0
        set roam_wall_hits  0
        set roam_on_wall    0
        set roam_sector     -1
        set roam_last_us    [now]
        set roam_hold_us    [now]
        set roam_last_ts    0
        set roam_n_dup      0
        set roam_n_gap      0
        set roam_started_us [now]
        roam_clear_entry
        dservSet ess/roam/wall 0
        # Force the pointer publish below rather than trusting the throttle:
        # if the agent begins where the last bout ended, the position has not
        # CHANGED, and a subscriber that saw the hide would keep showing
        # nothing for the whole first push.
        variable roam_shown; set roam_shown 0

        set roam_moving 1

        # Publish the starting pose and show the agent. The first sample of
        # the bout belongs in the trajectory: without it the record begins
        # wherever the first push happened to reach, and time-to-first-move
        # is unrecoverable.
        # roam_started_us, not [now]: it makes the seed's payload t exactly
        # 0.0 rather than the few microseconds since the line above, so the
        # bout's time axis starts where it says it does.
        roam_publish_pose $roam_started_us
        roam_publish_pointer [now]
        # The arena, republished INSIDE the obs period. roam_set_arena is
        # called from nexttrial, which runs BETWEEN obs, so the record it
        # produces lands under the PREVIOUS obs -- geometry for trial N
        # filed under trial N-1. Publishing here puts one correct record in
        # every obs, and lets the logger match be obs_limited so the stale
        # one is never written at all.
        roam_publish_geometry
        # Seed the region states from where the agent actually IS. Placing it
        # inside a patch and then starting would otherwise register as an
        # ENTRY on the first step, latching a response nobody made.
        roam_test_regions [now] 0

        # Adopt whatever the stick is ALREADY saying. ess/joystick/dir
        # publishes on change, so a stick held across roam_start would
        # otherwise move nothing until the subject released and pushed again
        # -- an agent that ignores a hand already asking it to go. Protocols
        # that gate on a centered stick first (the letgo rung) never reach
        # this, which is precisely why it would have gone unnoticed.
        if { "sectors" in $roam_sources && [dservExists ess/joystick/dir] } {
            catch { roam_dir ess/joystick/dir [dservGet ess/joystick/dir] }
        }
        return
    }

    proc roam_stop {} {
        variable roam_moving
        set roam_moving 0
        roam_stop_timer
        return
    }

    # Put the agent somewhere. Clamped, so a protocol cannot place it outside
    # its own arena; silent about it, because the caller asked for a position
    # and the arena is the authority on which ones exist.
    proc roam_place { x y } {
        variable roam_x
        variable roam_y
        variable roam_shown
        lassign [roam_clamp [expr {double($x)}] [expr {double($y)}]] nx ny hit
        set roam_x $nx
        set roam_y $ny
        set roam_shown 0            ;# force the next pointer publish
        return [list $roam_x $roam_y]
    }

    proc roam_pos {} {
        variable roam_x
        variable roam_y
        return [list $roam_x $roam_y]
    }

    proc roam_path_length {} {
        variable roam_path
        return $roam_path
    }

    proc roam_wall_contacts {} {
        variable roam_wall_hits
        return $roam_wall_hits
    }

    proc roam_is_moving {} {
        variable roam_moving
        return $roam_moving
    }

    # Live adjustment, mirroring dial_dpad_tune / dial_astick_tune: rate and
    # expo are found by watching a subject, so they want to be reachable from
    # an add_live_param without a protocol reload. Called with no arguments
    # it REPORTS -- including the two sample-health counters, which is how a
    # box-side stall is told apart from a steering problem.
    proc roam_tune { args } {
        variable roam_rate
        variable roam_accel
        variable roam_rate_max
        variable roam_scale
        variable roam_deadzone
        variable roam_expo
        variable roam_max_dt
        variable roam_pub_ms
        variable roam_min_step
        variable roam_n_dup
        variable roam_n_gap
        foreach { k v } $args {
            switch -- $k {
                -rate     { set roam_rate     [expr {double($v)}] }
                -accel    { set roam_accel    [expr {double($v)}] }
                -rate_max { set roam_rate_max [expr {double($v)}] }
                -scale    { set roam_scale    [expr {double($v)}] }
                -deadzone { set roam_deadzone [expr {double($v)}] }
                -expo     { set roam_expo     [expr {double($v)}] }
                -max_dt   { set roam_max_dt   [expr {double($v)}] }
                -pub_ms   { set roam_pub_ms   [expr {int($v)}] }
                -min_step { set roam_min_step [expr {double($v)}] }
                default   { error "::ess::roam_tune: unknown option '$k'" }
            }
        }
        if { $roam_deadzone < 0 || $roam_deadzone >= 1.0 } {
            error "::ess::roam_tune: deadzone is a FRACTION of full scale,\
                   in \[0,1)"
        }
        return [list rate $roam_rate accel $roam_accel \
                    rate_max $roam_rate_max scale $roam_scale \
                    deadzone $roam_deadzone expo $roam_expo \
                    max_dt $roam_max_dt dup_stamps $roam_n_dup \
                    gaps $roam_n_gap]
    }

    ########################################################################
    # operator / headless driving
    ########################################################################

    # Move the agent as if a source had. Drives the SAME integrator as real
    # hardware -- arena clamp, wall counting, region tests, publishing -- so
    # a simulated forage validates end to end (the companion to
    # joystick_simulate).
    proc roam_simulate { vx vy dt } {
        roam_step [expr {double($vx)}] [expr {double($vy)}] \
            [expr {double($dt)}] [now]
    }

    namespace export roam_init roam_deinit roam_start roam_stop \
        roam_place roam_pos roam_path_length roam_wall_contacts \
        roam_is_moving roam_set_arena roam_tune roam_simulate \
        roam_region_set roam_patch_set roam_region_on roam_region_off \
        roam_regions_clear roam_in_region roam_regions \
        roam_entered roam_entered_time roam_clear_entry roam_pointer_hide
}

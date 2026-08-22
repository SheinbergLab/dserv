#
# slider subprocess - calibrated analog control input for "steer" and
# other pursuit/navigation paradigms.
#
# Mirrors the emconf.tcl structural pattern:
#   - settings dict with scale/center/deadzone/invert/limit per axis
#   - one processor proc per input source (hardware, virtual)
#   - all paths converge on slider/position (binary float pair)
#
# Input sources:
#   ain/vals         - MCP3204 packed uint16 samples produced by the ain
#                      subprocess; channels selected via chan_x / chan_y
#                      settings. Primary hardware path.
#   mtouch/trackpad  - absolute trackpad coords from the input subprocess,
#                      with mtouch/trackpad/range giving the device's own
#                      axis extents. Normalized to the same 0..4095 space
#                      as ain, so ain-style calibration transfers.
#   extio ain group  - a thumbstick digitized by an extio box, published as
#                      extio/<box>/state/ain/stick. Same 0..4095 space as the
#                      other hardware paths, so calibration transfers. Paired
#                      with a one-pin DI group `stick_select` (the stick's own
#                      push-select) which COMMITS the swipe -- see below.
#   slider/virtual   - already-calibrated [x y] values from browser / sim.
#
# Output:
#   slider/position  - calibrated [x y] as binary float pair (DSERV_FLOAT)
#   slider/active    - 0/1 engagement signal. Pot (ain) always 1 when the
#                      ain path is the active source. Trackpad 1 between
#                      PRESS and RELEASE. Consumers that want to react to
#                      subject engagement subscribe here. Stick: 1 while
#                      deflected past swipe_threshold.
#   slider/raw       - raw [x y] as binary uint16 pair (DSERV_SHORT), in
#                      ADC counts; only emitted by the hardware path since
#                      the virtual path has no raw to report.
#   ess/slider_pos   - "x y" string for visualization/monitoring.
#   slider/settings  - current settings dict for UI introspection.
#
# Conventions:
#   - Output uses [x y] ordering (not the ain/vals legacy [y x] quirk).
#   - chan_y = -1 disables the y axis for a 1D slider setup (the default).
#

package require dlsh
# EACH SUBPROCESS GETS ITS OWN INTERPRETER, so the module path dsconf sets for
# the main interp is not inherited -- without this, `package require extio`
# fails with "can't find package extio" and the whole config script aborts,
# taking the slider subprocess with it. emconf.tcl does the same thing for the
# same reason.
tcl::tm::add $dspath/lib
package require extio   ;# decode extio state/ain blocks (thumbstick source)
package require settingsdb  ;# persist the MEASURED stick calibration
package require settings    ;# `slider source` is DECLARED, not learned

# Same store em's eye calibration lives in: one small db for the stable
# per-setup values a rig learns rather than declares.
if { ![info exists slider_caldb_path] } {
    set slider_caldb_path [file join $dspath db calibration.db]
}
settingsdb::init $slider_caldb_path

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

# enable error logging
errormon enable

namespace eval slider {

    # Linear calibration: out = scale * ((raw - center) minus deadzone),
    # then optional invert and symmetric clamp.
    #
    # source: which input path is active. Each processor publishes iff
    # source is its own name or "auto".
    #   "ain"      - only process_ain publishes
    #   "trackpad" - only process_trackpad publishes
    #   "virtual"  - only process_virtual publishes
    #   "auto"     - all paths publish; last writer wins
    #
    # continuity_mode: applies only to contact-based sources (trackpad,
    # virtual). Declared by each experimental system via
    # ::ess::slider_init -mode.
    #   "absolute"   - new PRESS sets cursor to the mapped absolute position
    #                  (hand-in-space / pointing paradigms)
    #   "continuous" - new PRESS holds cursor at its last value; DRAG moves
    #                  it by delta from the PRESS point (steering paradigm)
    #
    # release_behavior: what slider/position does after RELEASE
    #   "hold"     - keep publishing last value (default)
    #   "stop"     - stop updating; consumers key off slider/active
    #   "recenter" - return to (0, 0)
    #
    # chan_x / chan_y select which ain channels feed each axis. Set to -1
    # to disable that axis (output will be 0).
    #
    # limit_x / limit_y < 0 disables clamping on that axis.
    variable settings [dict create \
        source           "auto" \
        continuity_mode  "absolute" \
        release_behavior "hold" \
        chan_x            0 \
        chan_y           -1 \
        scale_x          1.0 \
        scale_y          1.0 \
        center_x      2048.0 \
        center_y      2048.0 \
        deadzone_x       0.0 \
        deadzone_y       0.0 \
        invert_x         0 \
        invert_y         0 \
        limit_x         -1.0 \
        limit_y         -1.0 \
        swipe_threshold  1.0]

    # Track current raw values for "set center" functionality
    variable current_raw_x 0.0
    variable current_raw_y 0.0

    # Last calibrated output
    variable last_x 0.0
    variable last_y 0.0

    # Last slider/active value (debounce repeat publishes)
    variable last_active -1

    # Trackpad state (populated from mtouch/trackpad/range at startup)
    variable trackpad_range_known 0
    variable trackpad_min_x 0
    variable trackpad_max_x 1
    variable trackpad_min_y 0
    variable trackpad_max_y 1

    # Trackpad contact-lifecycle state for continuous mode
    variable trackpad_press_raw_x 0
    variable trackpad_press_raw_y 0
    variable trackpad_press_out_x 0.0
    variable trackpad_press_out_y 0.0

    # Swipe-mode state. Engagement = magnitude crossed swipe_threshold
    # at any point during the current touch. last_engaged_{x,y} is the
    # position at the most recent engaged sample, used to compute the
    # committed angle on RELEASE (not the release position itself, in
    # case the subject pulled back toward center before lifting).
    variable swipe_engaged 0
    variable swipe_last_engaged_x 0.0
    variable swipe_last_engaged_y 0.0
    variable last_swipe_engaged_pub -1

    # ---- extio thumbstick state ----
    #
    # A STICK HAS NO TOUCH, so the trackpad's PRESS/DRAG/RELEASE has to be
    # rebuilt from what a stick does have:
    #
    #   origin  - the calibrated CENTER, not a press point. A stick is
    #             absolute-referenced and spring-returns, so center_{x,y}
    #             already IS the origin and calibrate_axis already reports
    #             displacement from it. Nothing to latch.
    #   engage  - radius crosses swipe_threshold (same threshold, same units
    #             as the trackpad path: post-calibration magnitude).
    #   commit  - THE SELECT BUTTON, not the return to center.
    #
    # That last one is the whole reason to wire the button. Committing on
    # "radius fell back below threshold" would sample the angle at the moment
    # the stick is nearest center -- where angle is least determined and most
    # contaminated by how the finger left the stick. The button commits while
    # the stick is still deflected, so swipe_last_engaged_{x,y} holds a large-
    # radius sample and the committed angle is the one the subject chose.
    variable stick_pressed 0

    proc update_settings {} {
        variable settings
        dservSet slider/settings $settings
    }

    # Set param value in our settings dict by name
    proc set_param {param_name value} {
        variable settings
        dict set settings $param_name $value
        update_settings
    }

    # Which input path is live is a rig DECLARATION -- the same shape as the
    # eye's source, and for the same reason. cal_keys already refuses to let
    # the calibration db carry it (see there); this gives it the place it was
    # missing. set_source stays the mechanism and becomes a RUNTIME override,
    # so flipping stick <-> trackpad while testing shows as `runtime` in the
    # settings gear and is forgotten at restart, while the gear persists the
    # rig's actual answer into local/rig.tcl.
    proc set_source     {s} { ::settings::put slider source $s }
    proc source_apply   {s} { set_param source $s }
    proc set_mode       {m} {
        if { $m ni {absolute continuous swipe} } {
            error "slider: invalid continuity_mode '$m'\
                (want absolute|continuous|swipe)"
        }
        set_param continuity_mode $m
    }
    proc set_release    {r} {
        if { $r ni {hold stop recenter} } {
            error "slider: invalid release_behavior '$r' (want hold|stop|recenter)"
        }
        set_param release_behavior $r
    }
    proc set_chan_x     {c} { set_param chan_x     $c }
    proc set_chan_y     {c} { set_param chan_y     $c }
    # The rig facts below go through settings, so local/slider.tcl's calls
    # land as RUNTIME overrides that the gear can see, persist and revert --
    # rather than mutating the dict behind the settings tree's back. The
    # MEASURED keys (chan_x/y, centres, invert) stay direct: they belong to
    # the calibration db, and cal_apply is their only author.
    proc set_scale_x    {s} { ::settings::put slider scale_x $s }
    proc set_scale_y    {s} { ::settings::put slider scale_y $s }
    proc scale_x_apply  {v} { set_param scale_x $v }
    proc scale_y_apply  {v} { set_param scale_y $v }
    proc set_center_x   {o} { set_param center_x   $o }
    proc set_center_y   {o} { set_param center_y   $o }
    proc set_deadzone_x {d} { set_param deadzone_x $d }
    proc set_deadzone_y {d} { set_param deadzone_y $d }
    proc set_invert_x   {o} { set_param invert_x   $o }
    proc set_invert_y   {o} { set_param invert_y   $o }
    proc set_limit_x    {l} { ::settings::put slider limit_x $l }
    proc set_limit_y    {l} { ::settings::put slider limit_y $l }
    proc limit_x_apply  {v} { set_param limit_x $v }
    proc limit_y_apply  {v} { set_param limit_y $v }
    proc set_swipe_threshold {t} { set_param swipe_threshold $t }

    # Set current raw position as center. Call this when the slider is
    # physically parked at the experimentally-neutral position.
    proc set_current_as_center {} {
        variable current_raw_x
        variable current_raw_y
        set_center_x $current_raw_x
        set_center_y $current_raw_y
    }

    ###########################################################################
    ####################### guided stick calibration ##########################
    ###########################################################################
    #
    # Retires the hand-editing. Center, orientation and throw are things the
    # system MEASURES, not things a human declares, and they were being written
    # into local/slider.tcl by hand -- three different sticks in one afternoon,
    # each with its own numbers, each transcribed by eye. That is the smell the
    # "humans declare -> files, system learns -> db" rule exists to catch, and
    # em's eye calibration has been on the right side of it all along.
    #
    # WHAT IS LEARNED (persisted here, in db/calibration.db):
    #   center_x/center_y   where this stick rests
    #   chan_x/chan_y       which block column is horizontal, which vertical
    #   invert_x/invert_y   the sign of each
    #   full_scale          usable throw, in output units
    #
    # WHAT STAYS DECLARED (local/slider.tcl, untouched by this):
    #   source, scale, deadzone, limit, continuity_mode, swipe_threshold
    #
    # ORIENTATION IS EIGHT CASES, NOT A FIT. swap x invert_x x invert_y
    # generates the symmetries of the square, and every mounting of a 2-axis
    # stick is one of them. TWO pushes pin it down exactly:
    #
    #   "up"    names the VERTICAL column and its sign
    #   "right" names the HORIZONTAL column and its sign
    #
    # One push is not enough, and that is not a theoretical worry -- a PSP
    # stick measured 2026-08-20 reported "up" as "right", which is equally
    # consistent with a 90-degree ROTATION and with a reflection. Only the
    # second, non-parallel push separates them. (It was a rotation, which a
    # bare column swap would NOT have fixed.)
    variable cal
    array set cal { active 0 stage "" n 0 msg "" }

    # A POSITION mark (rest/up/right) reads the RECENT window, not everything
    # since the previous mark. The distinction is not academic: marks are
    # driven by a human being asked to hold a stick, so the gap between them
    # is however long the conversation took -- measured at 107 s on a real
    # session. Averaging all of that blends the held position with the stick
    # sitting at rest beforehand and the transit out to it, and the answer
    # then depends on how chatty the operator was. The same push measured
    # -284 counts and -593 counts on two attempts for exactly that reason.
    #
    # 100 samples is 0.5 s at 200 Hz: long enough to average the noise down,
    # short enough that it is all "now".
    #
    # A SWEEP mark is the opposite question -- what did the stick REACH -- so
    # its envelope still accumulates over the whole stage. min/max are kept
    # since the last mark for that reason.
    variable cal_window 100

    # publish slider/cal/live every Nth sample (200 Hz -> 20 Hz)
    variable cal_live_every 10

    proc cal_reset_accum {} {
        variable cal
        set cal(n) 0
        array unset cal ring,*
        array unset cal mn,*
        array unset cal mx,*
    }

    # Fed the RAW COLUMN VECTOR by whichever processor is live, before any
    # chan_x/chan_y mapping -- calibration is what DECIDES that mapping, so it
    # cannot be expressed in terms of it.
    proc cal_feed { cols } {
        variable cal
        variable cal_window
        variable cal_live_every
        if { !$cal(active) } return
        set i 0
        foreach v $cols {
            set v [expr {double($v)}]
            if { ![info exists cal(mn,$i)] } {
                set cal(ring,$i) {} ; set cal(mn,$i) $v ; set cal(mx,$i) $v
            }
            # recent window, for the POSITION marks
            lappend cal(ring,$i) $v
            if { [llength $cal(ring,$i)] > $cal_window } {
                set cal(ring,$i) [lrange $cal(ring,$i) end-[expr {$cal_window-1}] end]
            }
            # envelope since the last mark, for the SWEEP
            if { $v < $cal(mn,$i) } { set cal(mn,$i) $v }
            if { $v > $cal(mx,$i) } { set cal(mx,$i) $v }
            incr i
        }
        incr cal(n)
        set cal(ncol) $i

        # A live view of the RAW COLUMNS while calibrating, for the wizard.
        #
        # slider/raw cannot serve: it carries chan_x/chan_y AFTER selection,
        # and which columns those are is precisely what calibration decides
        # -- so during it, the column that matters may be one slider/raw is
        # not publishing. This is the whole vector.
        #
        # It is also the honest answer to "is the stick streaming at all?",
        # the commonest failure here (an on-change ain group publishes
        # NOTHING at rest, so a rest mark has nothing to average). A sample
        # count that climbs says yes before any mark is attempted.
        #
        # Throttled 1-in-N: cal_feed is the 200 Hz+ path, and a datapoint per
        # sample would be a publish storm for a readout no eye can follow.
        if { $cal(n) % $cal_live_every == 0 } {
            dservSet slider/cal/live [list n $cal(n) cols $cols]
        }
    }

    proc cal_publish {} {
        variable cal
        dservSet slider/cal/status \
            [list active $cal(active) stage $cal(stage) samples $cal(n) \
                  msg $cal(msg)]
    }

    # Begin. Nothing is applied until cal_apply, and cal_cancel restores
    # whatever was in force -- a half-finished calibration must never leave the
    # rig in a state nobody chose.
    proc cal_begin {} {
        variable cal
        variable settings
        set cal(active) 1
        set cal(stage) rest
        set cal(msg) "hold the stick AT REST, then: slider::cal_mark rest"
        set cal(saved) $settings
        array unset cal m,*
        cal_reset_accum
        cal_publish
        return $cal(msg)
    }

    proc cal_cancel {} {
        variable cal
        variable settings
        if { !$cal(active) } { return "not calibrating" }
        if { [info exists cal(saved)] } { set settings $cal(saved) }
        set cal(active) 0
        set cal(stage) ""
        set cal(msg) "cancelled; previous settings restored"
        update_settings
        cal_publish
        return $cal(msg)
    }

    # Capture what has accumulated since the last mark as `stage`.
    #   rest  -> the center, and the noise floor everything else is judged against
    #   up    -> vertical column + sign
    #   right -> horizontal column + sign
    #   sweep -> the envelope, hence the throw
    proc cal_mark { stage } {
        variable cal
        if { !$cal(active) } { error "slider::cal_mark: call cal_begin first" }
        if { $stage ni {rest up right sweep} } {
            error "slider::cal_mark: want rest|up|right|sweep, got '$stage'"
        }
        if { $cal(n) < 20 } {
            error "slider::cal_mark: only $cal(n) samples -- is the stick\
                   streaming? (an on-change group publishes NOTHING at rest,\
                   so calibrate a `continuous` group)"
        }
        set ncol [expr {[info exists cal(ncol)] ? $cal(ncol) : 0}]
        if { $ncol < 2 } {
            error "slider::cal_mark: only $ncol column(s) in the stream --\
                   a stick needs two"
        }
        # The recent window for a position mark; the whole stage for a sweep.
        set n [llength $cal(ring,0)]
        for { set i 0 } { $i < $ncol } { incr i } {
            set s 0.0 ; set ss 0.0
            foreach v $cal(ring,$i) { set s [expr {$s+$v}]; set ss [expr {$ss+$v*$v}] }
            set mean [expr {$s/$n}]
            set var  [expr {$ss/$n - $mean*$mean}]
            set cal(m,$stage,$i)  $mean
            set cal(sd,$stage,$i) [expr {$var > 0 ? sqrt($var) : 0.0}]
            set cal(min,$stage,$i) $cal(mn,$i)
            set cal(max,$stage,$i) $cal(mx,$i)
        }
        # A position mark must be taken while the stick is STILL. A window
        # that spans the transit averages the journey with the destination,
        # and the result looks like a plausible reading of a place the stick
        # never was.
        if { $stage ne "sweep" } {
            set moving 0
            for { set i 0 } { $i < $ncol } { incr i } {
                if { $cal(sd,$stage,$i) > 8.0 } { set moving 1 }
            }
            if { $moving } {
                error [format "slider::cal_mark: the stick was still MOVING\
                       (sd %.1f/%.1f counts over the last %d samples). Hold it\
                       steady, then mark." \
                       $cal(sd,$stage,0) $cal(sd,$stage,1) $n]
            }
        }
        set cal(ncols) $ncol
        cal_reset_accum
        set cal(stage) $stage
        set cal(msg) "captured $stage ($n samples)"
        cal_publish
        return [list stage $stage samples $n \
                     means [lmap i [lrange {0 1 2 3 4 5 6 7} 0 [expr {$ncol-1}]] \
                                { format %.1f $cal(m,$stage,$i) }]]
    }

    # Derive, apply, persist. Refuses rather than guesses -- a calibration that
    # is wrong in a way nobody notices is worse than one that did not finish.
    proc cal_apply {} {
        variable cal
        variable settings
        if { !$cal(active) } { error "slider::cal_apply: call cal_begin first" }
        foreach need { rest up right sweep } {
            if { ![info exists cal(m,$need,0)] } {
                error "slider::cal_apply: '$need' was never marked"
            }
        }
        set ncol $cal(ncols)

        # deltas from the resting point
        for { set i 0 } { $i < $ncol } { incr i } {
            set du($i) [expr {$cal(m,up,$i)    - $cal(m,rest,$i)}]
            set dr($i) [expr {$cal(m,right,$i) - $cal(m,rest,$i)}]
        }

        # The dominant column for each push, and how clearly it dominates.
        # A sloppy diagonal push makes two columns move nearly equally, and
        # picking the larger would be a coin toss recorded as a fact.
        set vcol [cal_dominant du $ncol]
        set hcol [cal_dominant dr $ncol]
        lassign $vcol vcol vratio vmag
        lassign $hcol hcol hratio hmag

        if { $vcol == $hcol } {
            error "slider::cal_apply: 'up' and 'right' both moved column\
                   $vcol -- the two pushes were not perpendicular, or one\
                   axis is not reaching the box"
        }
        foreach { nm ratio mag } [list up $vratio $vmag right $hratio $hmag] {
            if { $ratio < 3.0 } {
                error [format "slider::cal_apply: the '%s' push was not clean\
                       -- its two columns moved by a ratio of only %.1f (want\
                       3 or more). Push straight %s and hold it." \
                       $nm $ratio $nm]
            }
            set floor [expr {10.0*$cal(sd,rest,0) + 5.0}]
            if { $mag < $floor } {
                error [format "slider::cal_apply: the '%s' push moved only\
                       %.1f counts, which is not clear of the resting noise\
                       (%.1f). Push to the stop and hold." $nm $mag $floor]
            }
        }

        # The throw, from the sweep: SHORTEST from center to a stop across both
        # live columns. A criterion the stick can only satisfy in one direction
        # is not a criterion.
        set throw 1e9
        foreach i [list $hcol $vcol] {
            set lo [expr {$cal(m,rest,$i) - $cal(min,sweep,$i)}]
            set hi [expr {$cal(max,sweep,$i) - $cal(m,rest,$i)}]
            foreach t [list $lo $hi] { if { $t < $throw } { set throw $t } }
        }
        if { $throw < 50 } {
            error [format "slider::cal_apply: swept throw is only %.0f counts\
                   -- sweep to every mechanical stop before applying" $throw]
        }

        # Pushing UP must give +y, pushing RIGHT must give +x.
        set inv_y [expr {$du($vcol) < 0 ? 1 : 0}]
        set inv_x [expr {$dr($hcol) < 0 ? 1 : 0}]

        dict set settings chan_x   $hcol
        dict set settings chan_y   $vcol
        dict set settings center_x [expr {double($cal(m,rest,$hcol))}]
        dict set settings center_y [expr {double($cal(m,rest,$vcol))}]
        dict set settings invert_x $inv_x
        dict set settings invert_y $inv_y

        set full [expr {$throw * [dict get $settings scale_x]}]
        dservSet slider/full_scale $full

        set cal(active) 0
        set cal(stage) done
        set cal(msg) "applied"
        update_settings
        cal_publish
        save_calibration

        return [list chan_x $hcol chan_y $vcol \
                     center_x [format %.1f $cal(m,rest,$hcol)] \
                     center_y [format %.1f $cal(m,rest,$vcol)] \
                     invert_x $inv_x invert_y $inv_y \
                     throw_counts [format %.0f $throw] \
                     full_scale [format %.2f $full] \
                     rest_noise_sd [format %.2f $cal(sd,rest,0)]]
    }

    # -> {column ratio magnitude}. ratio is primary/secondary, the measure of
    # how straight the push was.
    proc cal_dominant { arrname ncol } {
        upvar 1 $arrname d
        set best -1 ; set bmag 0.0 ; set second 0.0
        for { set i 0 } { $i < $ncol } { incr i } {
            set m [expr {abs($d($i))}]
            if { $m > $bmag } { set second $bmag ; set bmag $m ; set best $i } \
            elseif { $m > $second } { set second $m }
        }
        set ratio [expr {$second > 0 ? $bmag/$second : 1e9}]
        return [list $best $ratio $bmag]
    }

    # ---- persistence: LEARNED keys only ---------------------------------
    #
    # NOT the whole settings dict, unlike em. Persisting everything would let
    # the db silently override a DECLARED value -- `source` above all, which is
    # exactly the footgun em documents (a pinned source makes a rig read the
    # wrong input, days later, with no comment attached). Learned keys are the
    # ones a human should never be typing.
    variable cal_keys { chan_x chan_y center_x center_y invert_x invert_y }

    # WHICH stored calibration this rig is using.
    #
    # settingsdb is keyed by (subsystem, PROFILE), and one profile is right
    # for a rig with one analog input wired. It is wrong for a rig that
    # SWITCHES between two -- a dev box with both a trackpad and a stick, say
    # -- because the learned keys (chan_x/chan_y, centre, invert) describe a
    # particular device, and a single stored set would be forced onto both.
    # Flipping the input would then half-work: the file's mapping ignored,
    # the other device's centre in force, and nothing saying so.
    #
    # So a rig with more than one input names a profile per input, beside the
    # branch that selects it:
    #
    #     set slider::cal_profile stick        ;# in local/slider.tcl
    #     slider::set_source extio
    #
    # cal_apply saves to whatever is named here and load_calibration reads
    # the same, so the two devices keep separate measurements and neither
    # overwrites the other.
    variable cal_profile default

    proc set_cal_profile { p } {
        variable cal_profile
        if { [string trim $p] eq "" } {
            error "slider::set_cal_profile: a profile needs a name"
        }
        set cal_profile $p
        return $cal_profile
    }

    proc save_calibration {} {
        variable settings; variable cal_keys; variable cal_profile
        set d [dict create]
        foreach k $cal_keys { dict set d $k [dict get $settings $k] }
        if { [dservExists slider/full_scale] } {
            dict set d full_scale [dservGet slider/full_scale]
        }
        catch { ::settingsdb::save slider $d $cal_profile }
    }

    # Sourced AFTER local/slider.tcl, so a stored calibration wins over
    # hand-written values for LEARNED keys -- measurement beats a guess, and
    # retiring those guesses is the point. It says so at boot, because a value
    # that overrides the file someone is reading has to be visible somewhere.
    proc load_calibration {} {
        variable settings; variable cal_keys; variable cal_profile
        set stored [::settingsdb::load slider $cal_profile]
        if { $stored eq "" } return
        set applied {}
        foreach k $cal_keys {
            if { [dict exists $stored $k] } {
                dict set settings $k [dict get $stored $k]
                lappend applied $k
            }
        }
        if { [dict exists $stored full_scale] } {
            dservSet slider/full_scale [dict get $stored full_scale]
        }
        update_settings
        if { [llength $applied] } {
            puts "slider: calibration restored from db (profile $cal_profile):\
                  chan_x/[dict get $settings chan_x] chan_y/[dict get $settings chan_y]\
                  center [format %.1f [dict get $settings center_x]],[format %.1f [dict get $settings center_y]]\
                  invert [dict get $settings invert_x],[dict get $settings invert_y]\
                  -- these OVERRIDE local/slider.tcl; slider::forget_calibration\
                  drops back to the file"
        }
    }

    proc forget_calibration {} {
        variable cal_profile
        catch { ::settingsdb::forget slider $cal_profile }
        return "slider calibration dropped; local/slider.tcl values apply after a restart"
    }

    proc calibration {} {
        variable settings; variable cal_keys
        set d [dict create]
        foreach k $cal_keys { dict set d $k [dict get $settings $k] }
        if { [dservExists slider/full_scale] } {
            dict set d full_scale [dservGet slider/full_scale]
        }
        return $d
    }

    # Apply calibration to one raw axis value.
    # Factored so both axes run through identical math.
    proc calibrate_axis { raw center scale deadzone invert limit } {
        set d [expr {$raw - $center}]

        if { abs($d) < $deadzone } {
            set d 0.0
        } elseif { $d > 0 } {
            set d [expr {$d - $deadzone}]
        } else {
            set d [expr {$d + $deadzone}]
        }

        set v [expr {$scale * $d}]
        if { $invert } { set v [expr {-$v}] }

        if { $limit > 0 } {
            if { $v >  $limit } { set v  $limit }
            if { $v < -$limit } { set v [expr {-$limit}] }
        }
        return $v
    }

    # Publish calibrated x/y to slider/position + side outputs.
    # Used by all input paths.
    #
    # ts carries the SOURCE's timestamp when the source has one worth keeping.
    # The extio path does: the box stamps each scan on its own PTP-disciplined
    # grid, and that stamp is the only thing downstream that makes a reaction
    # time immune to subprocess dispatch and Tcl scheduling. Restamping with
    # [now] here threw it away one hop from the box -- silently, since a
    # host-stamped position looks identical to a box-stamped one.
    #
    # It was worth finding: ::ess::dial_stick_sample integrates dt from these
    # timestamps and documents them as the box's grid, and ::ess::joystick's
    # analog transport reports this stamp as the movement onset behind every
    # RT. Both were reading a host clock and saying otherwise.
    #
    # Default [now] for the paths with nothing better -- virtual has no real
    # source time, and the trackpad's own event time is not carried this far.
    proc publish { x y {ts 0} } {
        variable last_x
        variable last_y
        set last_x $x
        set last_y $y

        if { $ts <= 0 } { set ts [now] }
        set posvals [binary format ff $x $y]
        dservSetData slider/position $ts 2 $posvals ;# 2 = DSERV_FLOAT

        dservSet ess/slider_pos "$x $y"
    }

    # Publish slider/active engagement signal (0 or 1). Debounced so a
    # high-rate source (ain at kHz) doesn't spam the datapoint bus. Pot
    # path sets 1 whenever it publishes; trackpad path sets 1 on PRESS,
    # 0 on RELEASE.
    proc publish_active { v } {
        variable last_active
        if { $v == $last_active } return
        set last_active $v
        dservSet slider/active $v
    }

    # Debounced engagement signal for swipe mode. Edges:
    #   0 -> 1 when magnitude first crosses swipe_threshold during a touch
    #   1 -> 0 on RELEASE
    # Stays 0 for a touch that never exceeds threshold.
    proc publish_swipe_engaged { v } {
        variable last_swipe_engaged_pub
        if { $v == $last_swipe_engaged_pub } return
        set last_swipe_engaged_pub $v
        dservSet slider/swipe/engaged $v
    }

    # Source gate helper. Returns 1 iff this processor should publish
    # given the current source setting.
    proc source_allows { name } {
        variable settings
        set s [dict get $settings source]
        return [expr {$s eq $name || $s eq "auto"}]
    }

    # Cache the trackpad surface range published one-shot at input-
    # subprocess startup. Consumed by process_trackpad for normalization.
    proc set_trackpad_range { dpoint data } {
        variable trackpad_min_x
        variable trackpad_max_x
        variable trackpad_min_y
        variable trackpad_max_y
        variable trackpad_range_known
        lassign $data trackpad_min_x trackpad_max_x \
                      trackpad_min_y trackpad_max_y
        set trackpad_range_known 1
    }

    # Process raw ain/vals data.
    #
    # ain/vals is a packed array of uint16 samples, one per active channel,
    # produced by the ain subprocess via dserv_ain. dserv auto-decodes
    # DSERV_SHORT multi-element buffers into a Tcl list of ints before
    # dispatch to script callbacks, so $data is already "{v0 v1 ...}".
    # We pick chan_x / chan_y by index, apply per-axis calibration, and
    # publish slider/position.
    proc process_ain { dpoint data } {
        variable settings
        variable current_raw_x
        variable current_raw_y

        if { ![source_allows ain] } return

        set nchan [llength $data]
        if { $nchan == 0 } return

        # Calibration sees the RAW columns, before any chan_x/chan_y mapping:
        # deciding that mapping is what it is for.
        cal_feed $data

        dict with settings {
            # X axis
            if { $chan_x >= 0 && $chan_x < $nchan } {
                set raw_x [lindex $data $chan_x]
            } else {
                set raw_x 0
            }

            # Y axis (optional - chan_y < 0 disables)
            if { $chan_y >= 0 && $chan_y < $nchan } {
                set raw_y [lindex $data $chan_y]
            } else {
                set raw_y 0
            }

            set current_raw_x $raw_x
            set current_raw_y $raw_y

            # Publish raw ADC counts for calibration UIs. These are the
            # exact uint16 values from ain/vals for the selected channels,
            # so downstream readers can treat them identically to ain/vals.
            set rawvals [binary format ss $raw_x $raw_y]
            dservSetData slider/raw [now] 4 $rawvals ;# 4 = DSERV_SHORT

            set x [calibrate_axis $raw_x $center_x $scale_x \
                       $deadzone_x $invert_x $limit_x]

            if { $chan_y >= 0 } {
                set y [calibrate_axis $raw_y $center_y $scale_y \
                           $deadzone_y $invert_y $limit_y]
            } else {
                set y 0.0
            }
        }

        publish $x $y
        publish_active 1
    }

    # Process virtual slider input (already calibrated, in output units).
    # Mirrors em::process_virtual: straight passthrough so the browser /
    # simulator can drive slider/position without touching calibration.
    # No slider/raw publish here - the virtual path has no real raw value
    # to report, and conflating "fake raw" with true ADC counts would
    # confuse calibration UIs.
    # ---- extio thumbstick: analog pair + select button --------------------
    #
    # Subscribed by LABEL with a wildcard box (extio/*/state/ain/stick), the
    # same convention emconf uses for the analog eye source: define an ain
    # group called `stick` on any box and it comes alive, with no rig-side
    # binding to keep in step.
    #
    # source_allows() is the arbiter when a rig sets one -- but the DEFAULT IS
    # "auto", which permits every path. So a rig that has both a trackpad and a
    # `stick` group defined WILL have both publishing slider/position, last
    # writer wins, exactly the hazard emconf documents for eye sources. Set
    # slider::set_source explicitly in local/slider.tcl on any rig with more
    # than one slider input wired.
    #
    # chan_x / chan_y index the BLOCK's columns (which are in ascending channel
    # order), not the box's ADC channel numbers -- same meaning they have for
    # ain/vals. A two-channel `stick` group is therefore 0 and 1.
    proc process_stick { dpoint data } {
        variable settings
        variable current_raw_x
        variable current_raw_y
        variable swipe_engaged
        variable swipe_last_engaged_x
        variable swipe_last_engaged_y

        if { ![source_allows extio] } return

        # Newest scan only. This is a POSITION, not a trajectory to log: the
        # decoder documents ain_latest as exactly what a position controller
        # wants, and it stays correct if the group is ever batched.
        set col [::extio::ain_latest $data]
        set nchan [llength $col]
        if { $nchan == 0 } return

        # See process_ain: calibration works on the raw columns.
        cal_feed $col

        # The box's stamp for this block, carried through to slider/position.
        # With batch 1 (the rule for any source feeding a response) that IS
        # the scan's own time; batched, it is the block's, which is the same
        # approximation ain_latest already makes.
        set ts [dservTimestamp $dpoint]

        dict with settings {
            set raw_x [expr {($chan_x >= 0 && $chan_x < $nchan) ? [lindex $col $chan_x] : 0}]
            set raw_y [expr {($chan_y >= 0 && $chan_y < $nchan) ? [lindex $col $chan_y] : 0}]
            set current_raw_x $raw_x
            set current_raw_y $raw_y

            set rawvals [binary format ss $raw_x $raw_y]
            dservSetData slider/raw $ts 4 $rawvals ;# 4 = DSERV_SHORT

            # Displacement from the calibrated center -- which for a stick is
            # both the absolute position AND the swipe vector, so the same
            # publish serves every mode.
            set x [calibrate_axis $raw_x $center_x $scale_x \
                       $deadzone_x $invert_x $limit_x]
            if { $chan_y >= 0 } {
                set y [calibrate_axis $raw_y $center_y $scale_y \
                           $deadzone_y $invert_y $limit_y]
            } else {
                set y 0.0
            }
            publish $x $y $ts

            if { $continuity_mode eq "swipe" } {
                set mag [expr {sqrt($x*$x + $y*$y)}]
                if { $mag >= $swipe_threshold } {
                    set swipe_engaged 1
                    set swipe_last_engaged_x $x
                    set swipe_last_engaged_y $y
                    publish_swipe_engaged 1
                    publish_active 1
                } else {
                    # Back inside the deadband without committing: the subject
                    # abandoned the choice. Drop engagement but KEEP
                    # last_engaged, so a select arriving in the same few ms as
                    # the stick crossing back still commits the intended angle
                    # rather than nothing.
                    set swipe_engaged 0
                    publish_swipe_engaged 0
                    publish_active 0
                }
            } else {
                publish_active 1
            }
        }
    }

    # The stick's push-select, as a one-pin DI group (extio/*/state/group/
    # stick_select). `data` is the group's bitmask, so non-zero = pressed.
    #
    # Consumed HERE rather than bound as an ess button on purpose: the
    # protocol's contract is slider/swipe/{engaged,angle} and nothing else, so
    # committing here means a protocol written for the trackpad runs on a stick
    # unchanged. It is also not a layering stretch -- the trackpad's
    # PRESS/RELEASE is already a contact signal this file consumes, and select
    # is its exact analog for a stick.
    proc process_stick_select { dpoint data } {
        variable settings
        variable stick_pressed
        variable swipe_engaged
        variable swipe_last_engaged_x
        variable swipe_last_engaged_y

        if { ![source_allows extio] } return

        set down [expr {$data != 0}]
        if { $down == $stick_pressed } return      ;# level, not edge: ignore repeats
        set stick_pressed $down
        if { !$down } return                       ;# commit on PRESS, not release

        dict with settings {
            if { $continuity_mode ne "swipe" } return
            if { !$swipe_engaged } return          ;# not deflected: nothing chosen

            # Same commit payload and ORDER as the trackpad path: magnitude
            # first, so a consumer woken by the angle already sees a current
            # magnitude.
            set mag [expr {sqrt($swipe_last_engaged_x*$swipe_last_engaged_x + \
                                $swipe_last_engaged_y*$swipe_last_engaged_y)}]
            dservSetData slider/swipe/mag [now] 2 \
                [binary format f $mag] ;# 2 = DSERV_FLOAT
            set angle [expr {atan2($swipe_last_engaged_y, $swipe_last_engaged_x)}]
            if { $angle < 0 } {
                set angle [expr {$angle + 2.0*3.14159265358979}]
            }
            dservSetData slider/swipe/angle [now] 2 \
                [binary format f $angle] ;# 2 = DSERV_FLOAT

            set swipe_engaged 0
            publish_swipe_engaged 0
            publish_active 0
            switch $release_behavior {
                hold     { # last position stays as-is }
                stop     { # consumer keys off slider/active }
                recenter { publish 0.0 0.0 }
            }
        }
    }

    proc process_virtual { dpoint data } {
        if { ![source_allows virtual] } return

        lassign $data x y
        if { $x eq "" } { set x 0.0 }
        if { $y eq "" } { set y 0.0 }
        publish $x $y
        publish_active 1
    }

    # Process trackpad input from mtouch/trackpad. Three uint16s per
    # event: (x, y, event_type) where event_type is 0=PRESS, 1=DRAG,
    # 2=RELEASE. Surface coords are normalized into the same 0..4095
    # space ain uses so existing ain-style calibration (center_x,
    # scale_x, ...) applies unchanged. continuity_mode controls whether
    # PRESS jumps to the absolute mapped position or holds the last
    # output and DRAG delta-accumulates from the press point.
    proc process_trackpad { dpoint data } {
        variable settings
        variable trackpad_range_known
        variable trackpad_min_x
        variable trackpad_max_x
        variable trackpad_min_y
        variable trackpad_max_y
        variable trackpad_press_raw_x
        variable trackpad_press_raw_y
        variable trackpad_press_out_x
        variable trackpad_press_out_y
        variable swipe_engaged
        variable swipe_last_engaged_x
        variable swipe_last_engaged_y
        variable last_x
        variable last_y

        if { ![source_allows trackpad] } return
        if { !$trackpad_range_known }    return

        lassign $data raw_x raw_y event_type

        set rangex [expr {double($trackpad_max_x - $trackpad_min_x)}]
        set rangey [expr {double($trackpad_max_y - $trackpad_min_y)}]
        if { $rangex <= 0 || $rangey <= 0 } return

        # Normalize raw surface coords to 0..4095 (the ain unit space).
        set nx [expr {($raw_x - $trackpad_min_x) * 4095.0 / $rangex}]
        set ny [expr {($raw_y - $trackpad_min_y) * 4095.0 / $rangey}]

        dict with settings {
            switch $event_type {
                0 {
                    # PRESS. Remember the press point for continuous mode
                    # delta accumulation and swipe mode displacement.
                    set trackpad_press_raw_x $nx
                    set trackpad_press_raw_y $ny
                    set trackpad_press_out_x $last_x
                    set trackpad_press_out_y $last_y
                    publish_active 1

                    if { $continuity_mode eq "absolute" } {
                        # absolute: publish current mapped position now
                        set x [calibrate_axis $nx $center_x $scale_x \
                                   $deadzone_x $invert_x $limit_x]
                        if { $chan_y >= 0 } {
                            set y [calibrate_axis $ny $center_y $scale_y \
                                       $deadzone_y $invert_y $limit_y]
                        } else {
                            set y 0.0
                        }
                        publish $x $y
                    } elseif { $continuity_mode eq "swipe" } {
                        # swipe: published position is displacement from
                        # the press point so atan2(y, x) reflects swipe
                        # *direction* rather than absolute trackpad
                        # location. At PRESS displacement is (0, 0) by
                        # definition, so start the cursor at center.
                        publish 0.0 0.0
                        # Reset swipe engagement state for the new touch.
                        set swipe_engaged 0
                        set swipe_last_engaged_x 0.0
                        set swipe_last_engaged_y 0.0
                        publish_swipe_engaged 0
                    }
                    # continuous mode on PRESS: hold last output, no publish
                }
                1 {
                    # DRAG
                    if { $continuity_mode eq "absolute" } {
                        # absolute: publish current mapped position
                        set x [calibrate_axis $nx $center_x $scale_x \
                                   $deadzone_x $invert_x $limit_x]
                        if { $chan_y >= 0 } {
                            set y [calibrate_axis $ny $center_y $scale_y \
                                       $deadzone_y $invert_y $limit_y]
                        } else {
                            set y 0.0
                        }
                        publish $x $y
                    } elseif { $continuity_mode eq "swipe" } {
                        # swipe: publish displacement from press point.
                        # calibrate_axis with center=press_raw gives us
                        # scale_x * (nx - press_raw_x) with the standard
                        # deadzone/invert/limit applied. Engagement and
                        # commit-angle then both work in "swipe distance"
                        # / "swipe direction" terms, which is what the
                        # subject sees and what the threshold compares.
                        set dx [calibrate_axis $nx \
                                    $trackpad_press_raw_x $scale_x \
                                    $deadzone_x $invert_x $limit_x]
                        set dy [calibrate_axis $ny \
                                    $trackpad_press_raw_y $scale_y \
                                    $deadzone_y $invert_y $limit_y]
                        publish $dx $dy

                        set mag [expr {sqrt($dx*$dx + $dy*$dy)}]
                        if { $mag >= $swipe_threshold } {
                            if { !$swipe_engaged } {
                                set swipe_engaged 1
                                publish_swipe_engaged 1
                            }
                            # Remember the position so we can commit
                            # the right angle on RELEASE even if the
                            # subject pulls back toward press before
                            # lifting.
                            set swipe_last_engaged_x $dx
                            set swipe_last_engaged_y $dy
                        }
                    } else {
                        # continuous: out = out_at_press + scale * delta
                        set dx [expr {$scale_x * ($nx - $trackpad_press_raw_x)}]
                        if { $invert_x } { set dx [expr {-$dx}] }
                        set x [expr {$trackpad_press_out_x + $dx}]
                        if { $limit_x > 0 } {
                            if { $x >  $limit_x } { set x  $limit_x }
                            if { $x < -$limit_x } { set x [expr {-$limit_x}] }
                        }

                        if { $chan_y >= 0 } {
                            set dy [expr {$scale_y * ($ny - $trackpad_press_raw_y)}]
                            if { $invert_y } { set dy [expr {-$dy}] }
                            set y [expr {$trackpad_press_out_y + $dy}]
                            if { $limit_y > 0 } {
                                if { $y >  $limit_y } { set y  $limit_y }
                                if { $y < -$limit_y } { set y [expr {-$limit_y}] }
                            }
                        } else {
                            set y 0.0
                        }
                        publish $x $y
                    }
                }
                2 {
                    # RELEASE
                    if { $continuity_mode eq "swipe" && $swipe_engaged } {
                        # Commit: compute angle from last engaged position
                        # and publish slider/swipe/angle (radians, [0, 2π)).
                        #
                        # Also publish slider/swipe/mag (the commit swipe
                        # distance) as an additive side channel for
                        # magnitude-as-confidence readouts. Published
                        # BEFORE the angle so a consumer woken by the angle
                        # commit already sees a current magnitude. Nothing
                        # breaks if unread.
                        set mag [expr {sqrt($swipe_last_engaged_x*$swipe_last_engaged_x + \
                                            $swipe_last_engaged_y*$swipe_last_engaged_y)}]
                        dservSetData slider/swipe/mag [now] 2 \
                            [binary format f $mag] ;# 2 = DSERV_FLOAT
                        set angle [expr {atan2($swipe_last_engaged_y,\
                                               $swipe_last_engaged_x)}]
                        if { $angle < 0 } {
                            set angle [expr {$angle + 2.0*3.14159265358979}]
                        }
                        dservSetData slider/swipe/angle [now] 2 \
                            [binary format f $angle] ;# 2 = DSERV_FLOAT
                    }
                    if { $continuity_mode eq "swipe" } {
                        set swipe_engaged 0
                        publish_swipe_engaged 0
                    }
                    publish_active 0
                    switch $release_behavior {
                        hold     { # last position stays as-is }
                        stop     { # don't publish position; consumer keys off slider/active }
                        recenter { publish 0.0 0.0 }
                    }
                }
            }
        }
    }

    update_settings

    # Declared here, at the end of the namespace, so the procs it names
    # exist. local/slider.tcl is sourced LATER and its `slider::set_source`
    # now lands as a runtime override -- visible in the gear as `runtime`
    # rather than invisibly disagreeing with what the settings tree says.
    ::settings::declare slider source -default auto \
        -values {auto ain trackpad extio virtual} \
        -doc "which input path may publish slider/position. auto lets all of
them (last writer wins, fine while one is live); naming one
silences the rest. A rig with two analog inputs wired -- a
trackpad and a stick, say -- must name one or they fight.
Flipping this from the Slider panel is a session override;
persist it here to make it the rig's answer." \
        -apply {::slider::source_apply}

    catch { source_apply [::settings::get slider source] }
}

# Subscribe to the ain feed (primary hardware path)
dservAddExactMatch ain/vals
dpointSetScript    ain/vals slider::process_ain

# Subscribe to the extio thumbstick: analog pair by GROUP LABEL with a wildcard
# box, plus its select button. Matching on the label rather than a configured
# box name is what makes it self-activating -- define `stick` / `stick_select`
# on any box on the rig and it works, the same way emconf picks up `eye`.
#
# WHICH analog group is the stick is a rig declaration, not a constant --
# same shape as emconf's `eye ain_group`, and pickable/wiggle-fillable for
# the same reason. The select button's DI group follows the ain group's name
# (`<group>_select`), because they are two halves of one device and letting
# them drift apart is how a stick ends up with somebody else's button.
#
namespace eval slider {
    variable ain_dp ""
    variable sel_dp ""

    proc ain_group_apply { g } {
        variable ain_dp; variable sel_dp
        set g [string trim $g]
        if { $g eq "" } { return }
        set want     extio/*/state/ain/$g
        set want_sel extio/*/state/group/${g}_select
        if { $want eq $ain_dp } { return }
        if { $ain_dp ne "" } {
            catch { dpointRemoveScript $ain_dp slider::process_stick }
            catch { dservRemoveMatch   $ain_dp }
            catch { dpointRemoveScript $sel_dp slider::process_stick_select }
            catch { dservRemoveMatch   $sel_dp }
        }
        set ain_dp $want
        set sel_dp $want_sel
        dservAddMatch      $ain_dp
        dpointSetScript    $ain_dp slider::process_stick
        dservAddMatch      $sel_dp
        dpointSetScript    $sel_dp slider::process_stick_select
        puts "slider: analog stick group = $g ($ain_dp, select $sel_dp)"
        return
    }
}

# Rig facts about the pot/stick/trackpad wired to THIS rig: how counts become
# output units, whether that output is clamped, and how far it must move to
# count as engaged. NOT mode/release -- those are a loaded protocol's call
# (::ess::slider_init), and declaring them would fight whichever system runs.
foreach { _k _def _doc } {
    scale_x 1.0 "counts -> output units, X. 0.00488 maps a 0..4095 span to about +/-10."
    scale_y 1.0 "counts -> output units, Y."
    limit_x -1.0 "clamp on X output units; negative disables. A symmetric clamp on an asymmetric throw ROTATES the angle a 2-D reading reports, so leave it off for a stick."
    limit_y -1.0 "clamp on Y output units; negative disables."
    swipe_threshold 1.0 "deflection, in output units, that reads as engaged."
} {
    ::settings::declare slider $_k -default $_def -type double \
        -doc $_doc -apply [list ::slider::${_k}_apply]
    catch { ::slider::${_k}_apply [::settings::get slider $_k] }
}
unset -nocomplain _k _def _doc

::settings::declare slider ain_group -default stick \
    -candidates analog \
    -doc "the extio analog group carrying the stick, by LABEL --
extio/<any box>/state/ain/<this>, with its push-select read from
the DI group <this>_select. Wildcard box on purpose: define the
group on whichever box is present and it comes alive. Only
consulted when the slider source is extio (or auto)." \
    -apply {::slider::ain_group_apply}

::slider::ain_group_apply [::settings::get slider ain_group]

# Subscribe to the virtual path (browser / simulator)
dservAddExactMatch slider/virtual
dpointSetScript    slider/virtual slider::process_virtual

# Subscribe to the trackpad feed (input subprocess, mtouch/trackpad).
# Range is published one-shot at input startup; if input came up before
# us we'd otherwise miss it, so after subscribing we dservTouch to
# re-fire any existing value through the callback. Single code path for
# both "value arrives later" and "value already there" — and it doesn't
# leave a misleading "dpoint not found" trace in errorInfo when no
# trackpad is present (just a quiet no-op).
dservAddExactMatch mtouch/trackpad
dpointSetScript    mtouch/trackpad slider::process_trackpad

dservAddExactMatch mtouch/trackpad/range
dpointSetScript    mtouch/trackpad/range slider::set_trackpad_range
catch { dservTouch mtouch/trackpad/range }

# Local deployment overrides (which ain channel the slider is wired to,
# per-rig calibration: center / scale / deadzone / invert / limit). Not
# tracked in git - each deployment owns its own local/slider.tcl. See
# local/slider.tcl.EXAMPLE for the template.
if { [file exists $dspath/local/slider.tcl] } {
    source $dspath/local/slider.tcl
}

# AFTER the local file, deliberately. The db holds only what the rig MEASURED
# (center, orientation, throw) and the file holds what a human DECLARED
# (source, scale, limits) -- so on the learned keys the measurement wins,
# which is the whole point of retiring them from the file. It announces itself
# at boot, because a value that overrides the file someone is reading must be
# visible somewhere. slider::forget_calibration drops back to the file.
slider::load_calibration

puts "slider subprocessor started"

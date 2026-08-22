# -*- mode: tcl -*-
#
# ess_transports-1.0.tm
#
# Experiment-facing input routing: the directional joystick, button
# channels, analog stick response shaping, and slider support -- plus the
# input_transport registry that maps a rig's declared routes (settings:
# `joystick transport`, `button N`) onto resolvers.
#
# Extracted from ess-2.0.tm; docs/ess_transports_extraction.md is the work
# order. Same `namespace eval ess`, so every cross-reference resolves at
# call time; ess-2.0.tm requires this module at load, and this module must
# never require ess back. What stayed behind: em_windows, touch_windows,
# sound, juicer -- subsystems, not transports -- and the dial, a response
# MODE composed from these transports (ess_dial-1.0.tm).

package provide ess_transports 1.0
package require settings


###############################################################################
################################### joystick ##################################
###############################################################################
namespace eval ess {

    ########################################################################
    # Directional joystick. Two sources share one protocol-facing API:
    #
    #   legacy -- ::joystick_init (dsconf.tcl) publishes joystick/value; its
    #             bits fan out to button channels bound with
    #             `button_init N {} joystick <bit>` (unchanged behavior)
    #   box    -- an extio box publishes a chord GROUP datapoint
    #             (extio/<dev>/state/group/<label>, int bitmask) that the
    #             firmware chord-settles and stamps at the FIRST switch edge.
    #             We decode it to an 8-way sector and run a first-crossing
    #             response latch whose RT is that on-box onset timestamp --
    #             immune to the settle window, network jitter, and Tcl
    #             scheduling.
    #
    # Protocols read only:
    #   joystick_dir            current sector: -1 center, 0-7 clockwise from up
    #   joystick_dir_name       up|up_right|...|center
    #   joystick_centered       1 while at rest (letgo gating)
    #   joystick_reset          arm the latch (call alongside respwin_on)
    #   joystick_response       first settled dir since reset: -1 or 0-7
    #   joystick_response_time  dserv-clock us of that first crossing
    # plus the legacy joystick_value / joystick_active.
    ########################################################################
    variable joystick
    array set joystick {
        source ""  dp ""  mask 0  dir -1
        armed 0  response -1  response_time 0  map_override ""
    }

    # canonical nibble up=1 down=2 left=4 right=8 -> sector 0-7 clockwise from
    # up; anything else (center, impossible chords) -> -1. Impossible chords
    # are unreachable through a settled box group but a legacy value could
    # carry them.
    variable joystick_dirmap
    array set joystick_dirmap { 1 0  9 1  8 2  10 3  2 4  6 5  4 6  5 7 }
    variable joystick_dir_names { up up_right right down_right down down_left left up_left }

    # The inverse of joystick_dirmap: sector 0-7 -> canonical nibble. Needed
    # by every source that knows a DIRECTION and has to say it as switches --
    # joystick_simulate and the analog transport both do.
    variable joystick_sector_nibbles { 1 9 8 10 2 6 4 5 }

    ########################################################################
    # analog transport state (see joystick_process_analog)
    #
    # An analog stick reports a bearing and a magnitude; this decodes them
    # into the SAME eight sectors a four-switch d-pad expresses, so the
    # transport is invisible above joystick_ingest.
    #
    # sector is the last sector EMITTED, not the last one computed: the
    # difference is the whole point. slider/position arrives at the box's
    # sample rate (200 Hz on the rigs this was written for) and
    # joystick_ingest calls do_update, so ingesting every sample would spin
    # the state machine at the sample rate for a stick that is not moving.
    # Only a change reaches ingest.
    ########################################################################
    variable joystick_analog
    array set joystick_analog {
        dpoint "" sector -1 step 1 threshold 4.0 release 2.5 margin 8.0
    }

    # Cut a box string datapoint at the first NUL. Firmware before the
    # dserv_msg_string strlen fix sent strlen+1, so labels/pins arrive with a
    # trailing "\x00" -- which silently breaks an exact `switch` match ("up\0"
    # != "up") and datapoint-name lookups ("...state/label/9\0"). Defensive so
    # the auto-map works against any box firmware, not just reflashed ones.
    proc joystick_denul {s} { return [lindex [split $s \x00] 0] }

    # Normalize a pin label to a canonical direction (up|down|left|right), or ""
    # if it isn't directional. Accepts full words and single-letter forms, case-
    # insensitively -- so both `label 7 up` and `label 7 U` self-configure the
    # manifest-derived map. (An explicit `map` on joystick_init still overrides.)
    # One word -> a canonical direction, or "". Full words and the single-letter
    # forms, case-insensitively.
    proc joystick_dir_word {w} {
        switch -nocase -- $w {
            u - up    { return up }
            d - down  { return down }
            l - left  { return left }
            r - right { return right }
        }
        return ""
    }

    # A pin label -> a canonical direction, or "" if it names no direction.
    #
    # EXACT FIRST, THEN PER TOKEN, matching what button_group_bit already does
    # for buttons (`foreach tok [split $lab "_-"]`). Buttons have token-matched
    # for a long time, so `btn_left` binds as `left`; the joystick was
    # exact-only, so `joy_up` did NOT canonicalize -- and labelling a joystick
    # consistently with the buttons beside it is the obvious thing to do.
    #
    # The failure was silent and symmetric: with no canonical label,
    # joystick_map_for falls back to POSITIONAL bits 0-3 in ascending pin
    # order, which on a real box (joy_down/joy_up/joy_right/joy_left on pins
    # 2,3,4,6) reads as a clean up<->down AND left<->right flip. That cost a
    # rig session once, with the ess decode blameless, and nearly cost another
    # on 2026-08-21 -- the trap was documented rather than removed. This
    # removes it.
    #
    # TOKENS MUST BE FULL WORDS. The single-letter forms are honoured only as
    # a WHOLE label, because as one token among several a lone letter is far
    # more often an abbreviation of something else: `d_pad` means d-pad, not
    # down. `label 7 d` still works; `d_pad` correctly names no direction.
    proc joystick_dir_canon {label} {
        set l [joystick_denul $label]
        set d [joystick_dir_word $l]
        if { $d ne "" } { return $d }
        foreach tok [split $l "_-"] {
            if { [string length $tok] < 2 } continue   ;# see above
            set d [joystick_dir_word $tok]
            if { $d ne "" } { return $d }
        }
        return ""
    }

    # Rig-level source override (populate in local/post-pins.tcl), same
    # contract as button_bind: when set it WINS over whatever a protocol
    # passes to joystick_init, so a rig points "the joystick" at its box
    # group without protocol edits. Persists across systems.
    #   joystick_bind box {* joystick}      ;# (leading {} placeholder optional)
    #   joystick_bind analog {threshold 5.0}  ;# a rig whose stick is analog
    #
    # Called with NO arguments this REPORTS the current binding rather than
    # setting one -- the same contract as ::ess::dial_bind.
    #
    # It used to fall through and store the empty args, so `joystick_bind`
    # typed to ask "what is bound?" silently UNBOUND the joystick. That is
    # a read destroying the thing it reads, and nothing announces it: the
    # d-pad simply stops answering at the next joystick_init. It cost a
    # live rig its routing while checking exactly that.
    #
    # To clear deliberately, pass an explicit empty binding:
    #   joystick_bind {}
    variable joystick_binding {}
    proc joystick_bind {args} {
        variable joystick_binding
        if { [llength $args] == 0 } { return $joystick_binding }
        if { [llength $args] == 1 && [lindex $args 0] eq "" } {
            set joystick_binding {}
            catch { joystick_publish_status }
            return $joystick_binding
        }
        # `stick`/`dpad` are the device words the settings API and the docs
        # now use; `analog`/`box` are what the resolver registry is keyed by
        # and what rig files already say. Accept either here and speak the
        # registry's language downstream, so a rig can be typed either way.
        set kind [lindex $args 0]
        if { $kind eq "stick" } { set args [lreplace $args 0 0 analog] }
        if { $kind eq "dpad" }  { set args [lreplace $args 0 0 box] }
        if { [lindex $args 0] in {box analog} } { set args [linsert $args 0 {}] }
        set joystick_binding $args
        catch { joystick_publish_status }
        return $joystick_binding
    }

    ########################################################################
    # The binding, DECLARED
    #
    # joystick_bind above is the mechanism, and calling it from
    # local/post-input.tcl means the routing lives in hand-written Tcl: not
    # visible to a page, not validated, not changeable without editing a
    # file. Declaring it instead puts it on the settings API, which publishes
    # settings/<sub>/<key>/schema -- the type, the allowed values and the doc
    # -- so a GUI renders the right control without hardcoding any of it, and
    # settings::put ... -persist writes local/rig.tcl surgically.
    #
    # The declaration is the source of truth; joystick_bind stays the
    # mechanism it drives. `none` is the default and a NO-OP, so a rig with a
    # hand-written joystick_bind and no setting keeps working exactly as it
    # did -- this adds a way to say it, it does not take the old one away.
    #
    # See config/juicerconf.tcl for the same shape on `juicer destination`.
    # The transport names a DEVICE KIND, and there are two words for that in
    # this codebase and no others: `stick` (2-axis analog) and `dpad` (four
    # switches). `analog` and `box_group` said how the value TRAVELS, which
    # is not what a rig is choosing between; both are still accepted and
    # normalized, so no rig file or GUI has to change. See docs/input_vocabulary.md
    # for the device / contract / strategy split this belongs to.
    # analog/box_group/box are the pre-2026-08-22 spellings. NB: no `;#`
    # inside the switch list -- a braced switch body is a LIST, not a
    # script, so a comment there becomes another pattern and the whole
    # switch either errors ("extra switch pattern with no body") or, with an
    # even word count, silently matches on the comment's words.
    proc joystick_transport_norm { v } {
        set v [string trim $v]
        switch -exact -- $v {
            analog    { return stick }
            box_group { return dpad }
            box       { return dpad }
        }
        return $v
    }

    settings::declare joystick transport -default none \
        -values {none stick dpad} \
        -validate ::ess::joystick_transport_norm \
        -doc "which DEVICE answers joystick_dir: `stick` reads the calibrated\
              analog stick (slider/position) and quantizes it to eight\
              sectors; `dpad` reads four switches as an extio chord group;\
              `none` leaves the choice to whatever a protocol asks for.\
              (`analog` and `box_group` are the old names for stick and dpad\
              and still work.)" \
        -apply {::ess::joystick_bind_from_settings}

    # Engage at this fraction of the stick's MEASURED travel
    # (slider/full_scale, which slider::cal_* measures). A fraction rather
    # than an absolute threshold because "2.0" only means something against a
    # particular stick: the right value was 2.0, 3.9 and 3.8 on three sticks
    # in one afternoon, and any of them written here would go stale the
    # moment that stick was recalibrated.
    settings::declare joystick threshold_frac -default 0.40 -type double \
        -doc "stick transport: deflection past this fraction of measured\
              travel reads as deflected (release is 0.6 of it)" \
        -apply {::ess::joystick_bind_from_settings}

    settings::declare joystick box_device -default * \
        -doc "dpad transport: which box, or * to follow whichever is\
              present (hot-swap transparent)" \
        -apply {::ess::joystick_bind_from_settings}

    settings::declare joystick box_group -default joystick \
        -doc "dpad transport: the chord group's label on the box, whose\
              members are labelled up/down/left/right" \
        -apply {::ess::joystick_bind_from_settings}

    # Reads every key rather than just the one that changed: the transport
    # decides which of the others even apply, so a change to any of them
    # rebuilds the same way.
    proc joystick_bind_from_settings { args } {
        if { [catch { ::settings::get joystick transport } t] } { return }
        switch -exact -- [joystick_transport_norm $t] {
            none {
                # Deliberately NOT joystick_bind {} -- that would CLEAR an
                # imperative binding a rig may still be relying on. `none`
                # means "this rig does not declare one", not "there is none".
                return
            }
            stick {
                set cfg {}
                catch {
                    set tf [::settings::get joystick threshold_frac]
                    if { $tf > 0 } { lappend cfg threshold_frac $tf }
                }
                joystick_bind analog $cfg
            }
            dpad {
                set dev * ; set grp joystick
                catch { set dev [::settings::get joystick box_device] }
                catch { set grp [::settings::get joystick box_group] }
                joystick_bind box [list $dev $grp]
            }
        }
        return
    }

    # Apply whatever the rig declared, once, at load. `get` lazy-loads the
    # file, so there is no boot ordering to get wrong; catch because a bare
    # interp with no rig file is a perfectly normal way to load this module.
    catch { joystick_bind_from_settings }

    # fan a bitmask out to any button channels bound with `joystick <bit>`
    # (shared by the legacy value stream and the box-group ingest, so
    # lever-style protocols work from either source)
    proc joystick_fan_buttons {v} {
        variable buttons
        if {![info exists buttons(n_channels)]} return
        set v [expr {int($v)}]
        for {set i 0} {$i < $buttons(n_channels)} {incr i} {
            if {[info exists buttons(joy_source,$i)]} {
                set val [expr {($v & $buttons(joy_source,$i)) != 0}]
                if {$val != $buttons(state,$i)} {
                    set buttons(state,$i) $val
                    dservSet ess/button/$i $val
                }
            }
        }
    }

    ########################################################################
    # legacy HID stream (dsconf.tcl): raw value -> ess/joystick/value +
    # button-channel fan-out. Bit meanings are rig-specific, so no sector
    # decode here -- the box path owns joystick_dir/response.
    ########################################################################
    proc joystick_process_value {dpoint data} {
	dservSet ess/joystick/value $data
	joystick_fan_buttons $data
	do_update
    }

    proc joystick_process_button {dpoint data} {
        dservSet ess/joystick/button $data
        do_update
    }

    ########################################################################
    # shared ingest: canonical nibble -> live datapoints, button fan-out,
    # and the first-crossing latch. ts = event time in dserv-clock us (the
    # box group's onset stamp via dservTimestamp, or [now] for simulate).
    # The latch honors an open respwin's min_rt floor (respwin_active); with
    # no window open, arming alone is enough -- protocols that don't use
    # respwin still get first-crossing semantics.
    ########################################################################
    proc joystick_ingest {nib ts} {
        variable joystick
        variable joystick_dirmap
        set nib [expr {int($nib)}]
        set dir [expr {[info exists joystick_dirmap($nib)] ? $joystick_dirmap($nib) : -1}]
        set joystick(mask) $nib
        set joystick(dir) $dir
        dservSet ess/joystick/value $nib
        dservSet ess/joystick/dir $dir
        joystick_fan_buttons $nib
        if { $joystick(armed) && $dir >= 0 && $joystick(response) < 0 } {
            set ok 1
            if { [dservExists ess/respwin] && [dservGet ess/respwin] } {
                set ok [respwin_active]
            }
            if { $ok } {
                set joystick(response) $dir
                set joystick(response_time) $ts
                dservSet ess/joystick/response $dir
            }
        }
        do_update
    }

    proc joystick_process_group {dpoint data} {
        joystick_ingest [joystick_nibble $data [joystick_map_for $dpoint]] \
            [dservTimestamp $dpoint]
    }

    ########################################################################
    # analog transport: a thumbstick's x/y -> the same eight sectors.
    #
    # WHY QUANTIZE AT ALL, when the stick can express a continuous bearing.
    # Because the thing above this line is a d-pad contract, and the systems
    # built on it -- the joystick training ladder above all -- are built out
    # of sectors end to end: joystick_centered gates letgo, joystick_dir
    # highlights the pointed-at target, joystick_response latches one of
    # eight, and ::ess::dial's dpad source walks its cursor along a SPOKE
    # because "the cursor is only ever on a spoke" is what makes the reach
    # legible. Feeding those a continuous bearing would not enrich them, it
    # would bypass them. A dial that wants the true bearing already has one
    # (::ess::dial's `stick` source); this is the other question.
    #
    # Reads slider/position rather than the box block directly, which buys
    # three things: the rig's measured center (a stick rests where its pots
    # rest, never at 2048, and "centered" is a LIE without it -- the letgo
    # gate would never open or never close), one calibration shared with
    # every other analog consumer, and scale/invert/swap already applied.
    # The cost is a dependency on sliderconf's `source` being pointed at the
    # stick, which is exactly what resolve_joystick_analog reports on.
    ########################################################################

    # Circular difference in SECTOR units, period 8, result in (-4, 4].
    proc joystick_analog_sdiff { a b } {
        return [expr {fmod($a - $b + 12.0, 8.0) - 4.0}]
    }

    # Calibrated x/y -> sector 0-7, or -1 for centered.
    proc joystick_analog_sector { x y } {
        variable joystick_analog
        set cur $joystick_analog(sector)

        # Radial gate, hysteretic. A single threshold has a stick resting
        # near it crossing back and forth at the sample rate, and every
        # crossing is a fresh "first deflection" for the latch -- so the
        # response would be whichever way it happened to wobble.
        set mag [expr {sqrt($x*$x + $y*$y)}]
        if { $cur < 0 } {
            if { $mag < $joystick_analog(threshold) } { return -1 }
        } else {
            if { $mag < $joystick_analog(release) } { return -1 }
        }

        # Screen angles run counter-clockwise from +x; sectors run clockwise
        # from up. sector = (90 - deg)/45, the same identity the dial uses to
        # convert its committed angle back (targets.tcl `responded`).
        set deg [expr {atan2($y, $x)*180.0/3.14159265358979}]
        set s   [expr {(90.0 - $deg)/45.0}]

        set step $joystick_analog(step)
        set k [expr {int(round($s/double($step)))*$step}]
        set k [expr {(($k % 8) + 8) % 8}]

        if { $cur < 0 || $k == $cur } { return $k }

        # Angular hysteresis. Already on a spoke, the bearing must clear the
        # boundary by `margin` degrees before the sector changes -- otherwise
        # a hand holding a diagonal sits on the boundary and alternates
        # between two sectors 200 times a second.
        set need [expr {$step/2.0 + $joystick_analog(margin)/45.0}]
        if { abs([joystick_analog_sdiff $s $cur]) <= $need } { return $cur }
        return $k
    }

    proc joystick_process_analog { dpoint data } {
        variable joystick_analog
        variable joystick_sector_nibbles

        lassign $data x y
        if { $x eq "" } return
        if { $y eq "" } { set y 0.0 }

        set k [joystick_analog_sector $x $y]
        if { $k == $joystick_analog(sector) } return    ;# see the state comment
        set joystick_analog(sector) $k

        # The box's stamp, carried through sliderconf's publish: movement
        # onset on the box's own grid rather than whenever Tcl got round to
        # it. Quantized to the sample interval (5 ms at 200 Hz), which is
        # the honest limit of an analog source -- a debounced switch edge is
        # sharper, and that difference is real.
        joystick_ingest \
            [expr {$k < 0 ? 0 : [lindex $joystick_sector_nibbles $k]}] \
            [dservTimestamp $dpoint]
    }

    # Live adjustment, mirroring ::ess::dial_dpad_tune: threshold and margin
    # are things you find by watching a subject, not by reading a spec, so
    # they want to be reachable from an add_live_param without a reload.
    #
    #   joystick_analog_tune -threshold 5.0 -release 3.0 -margin 10.0
    #   joystick_analog_tune -divisions 4        ;# cardinals only
    proc joystick_analog_tune { args } {
        variable joystick_analog
        set given {}
        foreach { k v } $args {
            switch -- $k {
                -threshold {
                    if { $v <= 0 } {
                        error "joystick_analog_tune: threshold must be > 0"
                    }
                    set joystick_analog(threshold) [expr {double($v)}]
                }
                -release { set joystick_analog(release) [expr {double($v)}] }
                -margin  { set joystick_analog(margin)  [expr {double($v)}] }
                -divisions {
                    if { $v ni {4 8} } {
                        error "joystick_analog_tune: divisions must be 4 or 8"
                    }
                    # 4 divisions snaps a sloppy 45-degree push to the nearer
                    # CARDINAL, which is not the same as leaving the diagonals
                    # off a dial_dpad_sectors list: there an off-list push is
                    # treated as centered and buys the animal nothing. Early in
                    # shaping you usually want the snap.
                    set joystick_analog(step) [expr {8/$v}]
                }
                default { error "joystick_analog_tune: unknown option '$k'" }
            }
            lappend given $k
        }

        # Moving the threshold alone RE-DERIVES the release, so the
        # hysteresis band keeps its shape. The alternative -- leaving release
        # where it was -- means raising the threshold silently widens the
        # band, and a stick that engages at 10 but only releases at 2.5 reads
        # as deflected nearly all the way home. That is the letgo gate's
        # failure mode, and it would arrive by turning an unrelated knob.
        #
        # Passing -release in the SAME call is how you say you meant it.
        if { "-threshold" in $given && "-release" ni $given } {
            set joystick_analog(release) [expr {0.6*$joystick_analog(threshold)}]
        }

        if { $joystick_analog(release) >= $joystick_analog(threshold) } {
            error "joystick_analog_tune: release ($joystick_analog(release)) must be\
                   below threshold ($joystick_analog(threshold))"
        }
        return [array get joystick_analog]
    }

    # incoming group bitmask -> canonical nibble via a {dir bitidx ...} map
    proc joystick_nibble {data map} {
        set data [expr {int($data)}]
        set nib 0
        foreach {dir bit} $map {
            if { ($data >> $bit) & 1 } {
                switch $dir {
                    up    { incr nib 1 }
                    down  { incr nib 2 }
                    left  { incr nib 4 }
                    right { incr nib 8 }
                }
            }
        }
        return $nib
    }

    # bit->direction map for a bound box group. An explicit `map` option to
    # joystick_init wins; else derive from the box's announced manifest --
    # group pins (ascending = bit order) whose labels are up/down/left/right
    # (the plug-and-announce payoff); else assume bits 0-3 = up,down,left,
    # right and log it. Cached per device; joystick_init clears the cache.
    variable joystick_maps
    array set joystick_maps {}

    proc joystick_map_for {dpoint} {
        variable joystick
        variable joystick_maps
        if { $joystick(map_override) ne "" } { return $joystick(map_override) }
        set parts [split $dpoint /]         ;# <io_class>/<dev>/state/group/<label>
        set io  [lindex $parts 0]
        set dev [lindex $parts 1]
        set glabel [lindex $parts end]
        set key $dev/$glabel
        if { [info exists joystick_maps($key)] } { return $joystick_maps($key) }
        set map {}
        set pinsdp $io/$dev/state/group/$glabel/pins
        if { [dservExists $pinsdp] } {
            set i 0
            foreach p [split [joystick_denul [dservGet $pinsdp]] ,] {
                set ldp $io/$dev/state/label/$p
                if { [dservExists $ldp] } {
                    set c [joystick_dir_canon [dservGet $ldp]]
                    # TWO MEMBERS CLAIMING ONE DIRECTION used to be resolved
                    # silently by last-writer-wins, leaving a map that looks
                    # fine and steers wrongly. More reachable now that labels
                    # token-match, so say it rather than pick.
                    if { $c ne "" && [dict exists $map $c] } {
                        ess_warning "joystick: $dev/$glabel has two members\
                            naming '$c' (bits [dict get $map $c] and $i);\
                            keeping the first -- relabel one of them" "joystick"
                    } elseif { $c ne "" } {
                        dict set map $c $i
                    }
                }
                incr i
            }
        }
        # Any non-empty label-derived map is trusted -- a 2-choice left/right pad
        # (or a 3-way, or a full 4-way d-pad) each announce only the directions
        # they wire, and joystick_nibble only reads the entries present. Fall back
        # to positional bits 0-3 ONLY when no member pin carried a canon label.
        if { [dict size $map] == 0 } {
            ess_warning "joystick: no canon labels for $dev/$glabel; assuming bits 0-3 = up,down,left,right" "joystick"
            set map {up 0 down 1 left 2 right 3}
        }
        set joystick_maps($key) $map
        return $map
    }

    proc joystick_state_reset {} {
        variable joystick
        variable joystick_maps
        variable joystick_analog
        array unset joystick_maps
        array set joystick_maps {}
        # The analog decoder's last EMITTED sector has to go back with the
        # rest of it. Left behind, a stick still held at the moment of a
        # reset matches its own stale sector and emits nothing, so
        # ess/joystick/dir stays -1 while the stick is plainly deflected --
        # and the letgo gate would wave the trial through on a held stick,
        # which is the one thing it exists to prevent.
        set joystick_analog(sector) -1
        set joystick(mask) 0
        set joystick(dir) -1
        set joystick(armed) 0
        set joystick(response) -1
        set joystick(response_time) 0
        set joystick(map_override) ""
        dservSet ess/joystick/value 0
        dservSet ess/joystick/dir -1
        dservSet ess/joystick/response -1
    }

    ########################################################################
    # joystick_init: bind the directional joystick to a source. Mirrors
    # button_init's shape; the protocol never knows which source is wired.
    #
    #   joystick_init                        ;# legacy HID (joystick/value)
    #   joystick_init {} box {* joystick}    ;# any box's group "joystick"
    #                                        ;#   (glob = hot-swap transparent)
    #   joystick_init {} box {office hat} map {up 0 down 1 left 2 right 3}
    #   joystick_init {} analog {}           ;# an analog stick, via the slider
    #   joystick_init {} analog {threshold 5.0 divisions 4}
    #
    # analog options, all optional:
    #   threshold  magnitude (in slider output units) that reads as deflected
    #   release    magnitude that reads as centered again; default 0.6*threshold
    #   margin     degrees past a sector boundary needed to change sector
    #   divisions  8 (default, full d-pad) or 4 (cardinals; a diagonal push
    #              snaps to the nearer cardinal instead of falling between)
    #   dpoint     source; default slider/position
    ########################################################################
    proc joystick_init { {pin {}} args } {
        variable joystick
        variable joystick_binding
        variable io_class

        # a rig-level override (from post-pins) wins over the protocol's request
        variable joystick_req
        set joystick_req [list $pin {*}$args]
        catch { joystick_publish_status }
        if { [llength $joystick_binding] } {
            set pin  [lindex $joystick_binding 0]
            set args [lrange $joystick_binding 1 end]
        }
        set opts $args                      ;# flat option dict, pass AS a dict

        joystick_state_reset

        if { [dict exists $opts box] } {
            # bind to an extio chord group: extio/<dev>/state/group/<label>.
            # A glob <dev> follows whatever box is present at publish time.
            lassign [dict get $opts box] dev glabel
            if { $glabel eq "" } { set glabel joystick }
            if { [dict exists $opts map] } {
                set joystick(map_override) [dict get $opts map]
            }
            set dp $io_class/$dev/state/group/$glabel
            set joystick(source) box
            set joystick(dp) $dp
            if { [string first * $dev] >= 0 } {
                dservAddMatch $dp
                dpointSetScript $dp ::ess::joystick_process_group
                foreach k [dservKeys] {     ;# initial state from a present box
                    if { [string match $dp $k] } {
                        joystick_process_group $k [dservGet $k]
                        break
                    }
                }
            } else {
                dservAddExactMatch $dp
                dpointSetScript $dp ::ess::joystick_process_group
                if { [dservExists $dp] } { joystick_process_group $dp [dservGet $dp] }
            }
        } elseif { [dict exists $opts analog] } {
            # an analog stick, decoded from the calibrated slider stream
            variable joystick_analog
            set cfg [dict get $opts analog]
            set dp [expr {[dict exists $cfg dpoint] ?
                          [dict get $cfg dpoint] : "slider/position"}]

            # Defaults first, THEN the caller's, so a second joystick_init
            # with fewer options does not inherit the first one's tuning.
            array set joystick_analog {
                sector -1 step 1 threshold 4.0 release 2.5 margin 8.0
            }
            set joystick_analog(dpoint) $dp
            # One tune call, so its threshold/release rule applies here too:
            # a protocol naming a threshold and no release gets a band, not
            # the default release paired with an unrelated threshold.
            set tune {}
            foreach k { divisions threshold release margin } {
                if { [dict exists $cfg $k] } {
                    lappend tune -$k [dict get $cfg $k]
                }
            }

            # A threshold in output units is a MEASURED quantity in disguise:
            # "2.0" only means anything against a particular stick's travel,
            # and on the three sticks calibrated in one afternoon the right
            # value was 2.0, 3.9 and 3.8. Written into a rig file it is a
            # hand-copied measurement that goes stale the moment the stick is
            # recalibrated -- the exact thing local/slider.tcl was just
            # cleaned of.
            #
            # So the DEFAULT is a fraction of the measured travel, read from
            # slider/full_scale, and what a rig declares is the intent
            # ("engage at 40% of travel") rather than a number that depends on
            # hardware. An explicit `threshold` still wins, for a rig that
            # genuinely wants an absolute one.
            #
            # 0.40 engage / 0.24 release keeps the 0.6 ratio the tuner uses
            # when it re-derives a release on its own.
            if { ![dict exists $cfg threshold] } {
                set tf [expr {[dict exists $cfg threshold_frac] ?
                              [dict get $cfg threshold_frac] : 0.40}]
                set rf [expr {[dict exists $cfg release_frac] ?
                              [dict get $cfg release_frac] : 0.6*$tf}]
                if { ![dservExists slider/full_scale] } {
                    error "joystick_init analog: no explicit threshold, and\
                           this rig does not declare slider/full_scale -- so\
                           there is nothing to take a fraction OF. Run\
                           slider::cal_* to measure the stick's travel, or\
                           pass an absolute {threshold ... release ...}."
                }
                set full [dservGet slider/full_scale]
                lappend tune -threshold [expr {$tf*$full}] \
                             -release   [expr {$rf*$full}]
            }
            if { [llength $tune] } { joystick_analog_tune {*}$tune }

            set joystick(source) analog
            set joystick(dp) $dp

            # AddScript, not SetScript: ::ess::slider_process and the dial's
            # own observers live on slider/position too, and SetScript would
            # silently delete whichever registered first. append() dedupes,
            # so a second joystick_init still registers once.
            dservAddExactMatch $dp
            dpointAddScript    $dp ::ess::joystick_process_analog

            # SEED from the current position. slider/position is a level, not
            # an event: a stick held at init publishes nothing new until it
            # moves, so without this joystick_centered would report a held
            # stick as centered until the subject happened to shift it -- and
            # the letgo gate would pass on exactly the state it guards.
            if { [dservExists $dp] } {
                catch { joystick_process_analog $dp [dservGet $dp] }
            }
        } else {
            # legacy HID path (dsconf.tcl) - unchanged
            set joystick(source) legacy
            ::joystick_init

            dservAddExactMatch joystick/value
            dpointSetScript joystick/value ::ess::joystick_process_value

            dservAddExactMatch joystick/button
            dpointSetScript joystick/button ::ess::joystick_process_button
            dservSet ess/joystick/button 0
        }

        # publish so a frontend can show the virtual D-pad
        dservSet ess/joystick_active 1
    }

    # Clean up joystick subscriptions
    proc joystick_deinit {} {
	variable joystick
	if { $joystick(source) eq "box" && $joystick(dp) ne "" } {
	    catch { dpointSetScript $joystick(dp) {} }
	    catch { dservRemoveMatch $joystick(dp) }
	}
	if { $joystick(source) eq "analog" && $joystick(dp) ne "" } {
	    # Remove only OUR handler, and do NOT drop the match: slider/position
	    # is shared ground -- ::ess::slider_process and the dial observe it
	    # too, and removing the match would silence them along with us.
	    catch { dpointRemoveScript $joystick(dp) ::ess::joystick_process_analog }
	}
	if {[dservExists joystick/value]} {
	    dservRemoveMatch joystick/value
	}
	if {[dservExists joystick/button]} {
	    dservRemoveMatch joystick/button
	}
	set joystick(source) ""
	set joystick(dp) ""
	variable joystick_req; set joystick_req {}
	joystick_state_reset
	dservSet ess/joystick_active 0
	catch { joystick_publish_status }
    }

    ########################################################################
    # joystick query helpers
    ########################################################################

    # Return current joystick value (raw integer from ess/joystick/value)
    proc joystick_value {} {
	if {[dservExists ess/joystick/value]} {
	    return [dservGet ess/joystick/value]
	}
	return 0
    }

    # Return 1 if joystick is deflected in any direction
    proc joystick_active {} {
	return [expr {[joystick_value] != 0}]
    }

    # Return 1 while the stick is at rest (reads well in letgo gating)
    proc joystick_centered {} {
	return [expr {[joystick_value] == 0}]
    }

    # Current sector (-1 center, 0-7 clockwise from up) and its name
    proc joystick_dir {} {
	variable joystick
	return $joystick(dir)
    }
    proc joystick_dir_name {} {
	variable joystick
	variable joystick_dir_names
	if { $joystick(dir) < 0 } { return center }
	return [lindex $joystick_dir_names $joystick(dir)]
    }

    ########################################################################
    # response latch: joystick_reset arms it (call at response-window open,
    # alongside respwin_on); the FIRST settled non-center direction after
    # that is captured with its onset timestamp and held until the next
    # reset, so transitions read the response at their leisure.
    ########################################################################
    proc joystick_reset {} {
	variable joystick
	set joystick(armed) 1
	set joystick(response) -1
	set joystick(response_time) 0
	dservSet ess/joystick/response -1
    }
    proc joystick_response {} {
	variable joystick
	return $joystick(response)
    }
    proc joystick_response_time {} {
	variable joystick
	return $joystick(response_time)
    }

    # Simulate a deflection without hardware (companion to button_simulate):
    # a direction name (up, up_right, ... or center) or a sector 0-7 / -1.
    # Drives the SAME ingest path as a real box event -- latch, datapoints,
    # button fan-out, state-machine wake -- so a simulated 8-way task
    # validates end-to-end.
    proc joystick_simulate {what} {
	variable joystick_dir_names
	variable joystick_sector_nibbles
	set sector_nibbles $joystick_sector_nibbles
	if { $what eq "center" || $what eq "-1" } {
	    set nib 0
	} elseif { [string is integer -strict $what] } {
	    if { $what < 0 || $what > 7 } {
		error "joystick_simulate: sector 0-7, direction name, or center"
	    }
	    set nib [lindex $sector_nibbles $what]
	} else {
	    set i [lsearch -exact $joystick_dir_names $what]
	    if { $i < 0 } { error "joystick_simulate: unknown direction '$what'" }
	    set nib [lindex $sector_nibbles $i]
	}
	joystick_ingest $nib [now]
    }

}

###############################################################################
################################### buttons ###################################
###############################################################################

namespace eval ess {
    variable buttons
    array set buttons {
	n_channels 0
    }

    # Namespace class for external I/O boxes: their datapoints live under
    # <io_class>/<device>/... (matches BOX_CLASS in the box firmware).
    variable io_class "extio"

    # Rig-level source override table (populated in local/post-pins.tcl). Maps a
    # logical button channel -> its physical source, in exactly the words that
    # would follow `chan` in a button_init call -- passed VERBATIM (no extra
    # braces). Valid source forms:
    #
    #   <pin>                        local GPIO line         (e.g. 24)
    #   {} box {<dev> <pin>}         box DI line by pin      -> extio/<dev>/state/di/<pin>
    #   {} box {<dev> <grp> <label>} box group member by LABEL: board-independent --
    #                                the pin is resolved per-board from the box's
    #                                announced manifest (state/group/<grp>/pins +
    #                                state/label/<n>), so rigs that wire the same
    #                                button to different pins share ONE binding.
    #   {} joystick <bit>            a joystick direction bit (up=1 down=2 left=4 right=8)
    #
    # Group-member label matching (box {dev grp label}) is, in priority order:
    #   exact  -- <label> == a member's label            (fast path)
    #   glob   -- <label> has * ? [  -> string match      (e.g. {* response *left})
    #   token  -- <label> == any _/- delimited token of a member label, so a bind
    #             for "left" also finds a pin labeled "btn_left" or "joy_left".
    # Group scoping (buttons in one group, joystick dirs in another) keeps a token
    # match unique; if two members still match, the first wins and a warning fires.
    #
    # When an entry exists it WINS over whatever a protocol passes to button_init --
    # so a rig can point a channel at a box (or joystick) without editing any
    # protocol; the protocol still only declares/reads ess/button/<chan>. Persists
    # across protocol/system loads.
    #
    #   button_bind 0 24                        ;# local GPIO pin 24
    #   button_bind 0 {} box {office 14}        ;# extio/office/state/di/14
    #   button_bind 0 {} box {* response left}  ;# whichever pin is labeled "left" in
    #   button_bind 1 {} box {* response right} ;#   group "response" on ANY box (glob)
    #
    # BRACE LEVEL (common gotcha): the source words follow `chan` directly, exactly
    # as in button_init -- do NOT wrap them in an extra {...}. `button_bind 0 {{} box
    # {...}}` collapses them into one argument; button_init then reads that list as a
    # bogus `pin` and gpioLineRequestInput errors "expected integer but got a list".
    variable button_bindings
    array set button_bindings {}

    # `button_bind <chan>` with no source REPORTS that channel's binding
    # ("" if unbound); `button_bind` with no channel at all reports every
    # bound channel as a {chan binding} dict. Same contract as dial_bind and
    # joystick_bind: reading must never write.
    #
    # Without the first guard, asking what channel 0 was bound to UNBOUND
    # it -- and a status page rendering a list of channels would have
    # cleared every one of them as it drew.
    #
    # To clear deliberately, pass an explicit empty binding:
    #   button_bind 0 {}
    proc button_bind {args} {
	variable button_bindings
	if { [llength $args] == 0 } {
	    set d [dict create]
	    foreach c [lsort -integer [array names button_bindings]] {
		dict set d $c $button_bindings($c)
	    }
	    return $d
	}
	set chan [lindex $args 0]
	set args [lrange $args 1 end]
	if { [llength $args] == 0 } {
	    if { [info exists button_bindings($chan)] } {
		return $button_bindings($chan)
	    }
	    return ""
	}
	if { [llength $args] == 1 && [lindex $args 0] eq "" } {
	    unset -nocomplain button_bindings($chan)
	    catch { button_publish_status $chan }
	    return ""
	}
	set button_bindings($chan) $args
	# Publish the verdict as soon as the rig declares it, so a page can
	# show that a binding resolves to nothing BEFORE a system loads and
	# long before a subject presses.
	catch { button_publish_status $chan }
	return $button_bindings($chan)
    }

    ########################################################################
    # The button routing, DECLARED
    #
    # The third and last of the imperative routers (see `joystick transport`
    # and `dial sources`). Buttons differ from those two in being INDEXED --
    # a rig routes each channel separately -- so this is one key per channel
    # rather than one key for the subsystem:
    #
    #     setting button 0 box:*/response/left
    #     setting button 1 box:*/response/right
    #     setting button 2 gpio:17
    #
    # The value is a compact ROUTE rather than the flat option list
    # button_bind takes, because the settings file is meant to be read and
    # edited by a person and `{} box {* response left}` is four levels of
    # bracing to express three facts. The four forms map onto the four
    # resolvers registered for `button`:
    #
    #   box:<dev>/<group>/<label>  a labelled member of a chord group --
    #                              board-independent, and the recommended one
    #   box:<dev>/<pin>            a box DI pin by number
    #   gpio:<pin>                 a local GPIO line
    #   joystick:<bit>             a bit of the joystick nibble
    #   none                       this rig does not declare this channel
    #
    # <dev> may be * to follow whichever box is present.
    #
    # CHANNELS ARE DECLARED UP FRONT, 0..3. The runtime count grows as
    # protocols call button_init, so there is no number to ask for at load
    # time; four covers the rigs in service and an unused channel costs one
    # defaulted key that binds nothing. Raise button_nchan_declared to extend.
    variable button_nchan_declared 4

    # -> the canonical route string, or an error that names the four forms.
    # Rejecting at the door is the point: a typo'd route would otherwise
    # surface as a channel that silently never fires.
    proc button_route_norm { v } {
        set v [string trim $v]
        if { $v eq "" || $v eq "none" } { return none }
        set i [string first : $v]
        if { $i < 0 } {
            error "button route '$v': want none | box:<dev>/<group>/<label> |\
                   box:<dev>/<pin> | gpio:<pin> | joystick:<bit>"
        }
        set kind [string range $v 0 [expr {$i-1}]]
        set rest [string range $v [expr {$i+1}] end]
        switch -exact -- $kind {
            box {
                set parts [split $rest /]
                if { [llength $parts] ni {2 3} } {
                    error "button route 'box:$rest': want <dev>/<group>/<label>\
                           (by label) or <dev>/<pin> (by number)"
                }
                foreach p $parts {
                    if { $p eq "" } {
                        error "button route 'box:$rest': empty component"
                    }
                }
                if { [llength $parts] == 2 &&
                     ![string is integer -strict [lindex $parts 1]] } {
                    error "button route 'box:$rest': the two-part form is\
                           <dev>/<pin> and '[lindex $parts 1]' is not a pin\
                           number -- for a labelled member use\
                           <dev>/<group>/<label>"
                }
                return box:$rest
            }
            gpio - joystick {
                if { ![string is integer -strict $rest] } {
                    error "button route '$v': $kind: wants a number, got '$rest'"
                }
                return $kind:$rest
            }
            default {
                error "button route '$v': unknown kind '$kind' -- want box,\
                       gpio or joystick"
            }
        }
    }

    proc button_bind_from_settings { args } {
        variable button_nchan_declared
        for { set c 0 } { $c < $button_nchan_declared } { incr c } {
            if { [catch { ::settings::get button $c } v] } continue
            if { $v eq "none" } continue      ;# no-op; never CLEARS a binding
            set i [string first : $v]
            set kind [string range $v 0 [expr {$i-1}]]
            set rest [string range $v [expr {$i+1}] end]
            switch -exact -- $kind {
                box {
                    button_bind $c {} box [split $rest /]
                }
                gpio     { button_bind $c $rest }
                joystick { button_bind $c {} joystick $rest }
            }
        }
        return
    }

    for { set _c 0 } { $_c < $button_nchan_declared } { incr _c } {
        settings::declare button $_c -default none \
            -validate ::ess::button_route_norm \
            -doc "what drives ess/button/$_c: box:<dev>/<group>/<label> (a\
                  labelled chord-group member, board-independent),\
                  box:<dev>/<pin>, gpio:<pin>, joystick:<bit>, or none.\
                  <dev> may be * to follow whichever box is present" \
            -apply {::ess::button_bind_from_settings}
    }
    unset _c

    catch { button_bind_from_settings }

    ########################################################################
    # Label-keyed button GROUP source (button_init ... box {dev group label}).
    #
    # A box chord GROUP announces state/group/<g>/pins (ascending = bit order)
    # and per-pin state/label/<n>; the group EVENT state/group/<g> is a bitmask.
    # This binds a discrete button CHANNEL to the member carrying <label>, so
    # "left"/"right" (or ANY label -- go/stop/yes/no...) find their physical pin
    # from the manifest. Different boards wire different pins but announce the
    # same labels, so the rig binding is identical everywhere -- the pin
    # difference lives in each box's persisted labels, not in post-pins.
    #
    # Unlike the joystick path this does NOT canonicalize to directions: labels
    # map verbatim to bits. Many channels may bind one group; a single shared
    # dispatcher fans each group event to all of them (dpointSetScript is one
    # script per datapoint, so per-channel scripts would clobber each other).
    ########################################################################
    variable button_group_chans ;# group-dp (pattern) -> {chan label chan label ...}
    array set button_group_chans {}
    variable button_group_maps  ;# concrete group-dp -> {label bitidx ...} (cached once labels land)
    array set button_group_maps {}
    variable button_group_warned ;# "<group-dp>,<label>" -> 1 : a miss/ambiguity already logged
    array set button_group_warned {}

    # {label -> bit index} from a concrete group's announced pins + pin labels.
    # An empty result is NOT cached, so a bind that races ahead of the box's
    # manifest heals as soon as the labels are announced.
    proc button_group_map {dpoint} {
	variable button_group_maps
	if {[info exists button_group_maps($dpoint)]} { return $button_group_maps($dpoint) }
	set parts [split $dpoint /]                 ;# <io>/<dev>/state/group/<g>
	set io [lindex $parts 0]; set dev [lindex $parts 1]; set g [lindex $parts end]
	set map {}
	set pinsdp $io/$dev/state/group/$g/pins
	if {[dservExists $pinsdp]} {
	    set i 0
	    foreach p [split [joystick_denul [dservGet $pinsdp]] ,] {
		set ldp $io/$dev/state/label/[string trim $p]
		if {[dservExists $ldp]} {
		    set lab [string trim [joystick_denul [dservGet $ldp]]]
		    if {$lab ne ""} { dict set map $lab $i }
		}
		incr i
	    }
	}
	if {[dict size $map] > 0} { set button_group_maps($dpoint) $map }
	return $map
    }

    # one dispatcher on every bound group pattern -> fan to that pattern's channels
    proc button_group_dispatch {dpoint data} {
	variable button_group_chans
	foreach pat [array names button_group_chans] {
	    if {[string match $pat $dpoint]} { button_group_fan $pat $dpoint $data }
	}
    }

    # Resolve a requested label to a member bit within a group's {label -> bit}
    # map. Priority: (1) EXACT label match (fast path, backward compatible);
    # (2) GLOB if the request carries * ? [  -> string match the member labels;
    # (3) TOKEN -- request equals any _/- delimited token of a member label, so
    # "left" binds a pin labeled "btn_left" or "joy_left". Returns {bit nmatched};
    # bit -1 / nmatched 0 when nothing matches. Group scoping (buttons vs joystick
    # dirs in separate groups) usually makes a token match unique.
    proc button_group_bit {map request} {
	if {[dict exists $map $request]} { return [list [dict get $map $request] 1] }
	set glob [regexp {[*?\[]} $request]
	set hits {}
	dict for {lab bit} $map {
	    set m 0
	    if {$glob} {
		set m [string match $request $lab]
	    } else {
		foreach tok [split $lab "_-"] { if {$tok eq $request} { set m 1; break } }
	    }
	    if {$m} { lappend hits $bit }
	}
	if {![llength $hits]} { return {-1 0} }
	return [list [lindex $hits 0] [llength $hits]]
    }

    proc button_group_fan {pat dpoint data} {
	variable buttons
	variable button_group_chans
	variable button_group_warned
	set map [button_group_map $dpoint]
	set haveMap [expr {[dict size $map] > 0}]
	set data [expr {int($data)}]
	set changed 0
	foreach {chan lab} $button_group_chans($pat) {
	    set bit -1
	    if {$haveMap} {
		lassign [button_group_bit $map $lab] bit nmatch
		if {![info exists button_group_warned($dpoint,$lab)]} {
		    set dev [lindex [split $dpoint /] 1]
		    set g   [lindex [split $dpoint /] end]
		    if {$bit < 0} {
			set button_group_warned($dpoint,$lab) 1
			ess_warning "button: group $g on $dev has no member matching label '$lab' (chan $chan)" "button"
		    } elseif {$nmatch > 1} {
			set button_group_warned($dpoint,$lab) 1
			ess_warning "button: label '$lab' matches $nmatch members of group $g on $dev (chan $chan); using first" "button"
		    }
		}
	    }
	    set val [expr {($bit >= 0 && (($data >> $bit) & 1)) ? 1 : 0}]
	    if {![info exists buttons(state,$chan)] || $buttons(state,$chan) != $val} {
		set buttons(state,$chan) $val
		dservSet ess/button/$chan $val
		set changed 1
	    }
	}
	if {$changed} { do_update }
    }

    # button_init calls this for a `box {dev group label}` source.
    proc button_group_bind {chan dev grp lab} {
	variable buttons
	variable io_class
	variable button_group_chans
	set gdp $io_class/$dev/state/group/$grp
	set buttons(state,$chan) 0
	dservSet ess/button/$chan 0
	set buttons(grp,$chan) $gdp
	set fresh [expr {![info exists button_group_chans($gdp)]}]
	lappend button_group_chans($gdp) $chan $lab
	if {$fresh} {
	    if {[string first * $dev] >= 0} {
		dservAddMatch $gdp                     ;# glob dev: any present box, hot-swap
	    } else {
		dservAddExactMatch $gdp
	    }
	    dpointSetScript $gdp ::ess::button_group_dispatch
	}
	foreach k [dservKeys] {                        ;# initial level from an already-present box
	    if {[string match $gdp $k]} { button_group_fan $gdp $k [dservGet $k]; break }
	}
    }

    ########################################################################
    # box_schedule_pulse / box_schedule_timer: offload a precisely-timed box
    # output/marker to fire <delay_ms> from NOW.
    #
    # The box fires it on its OWN clock (deterministic, us), which beats
    # host-driving the output at fire time (that carries the network's fire-time
    # jitter). We anchor the schedule to the current beginobs -- a shared event
    # both sides captured -- so THIS command's network delay is absorbed: it just
    # has to arrive before the fire time. The box also posts state/timer/<n> at
    # the fire instant (accurately timestamped) so the state machine is notified.
    #
    # Requires: the box subscribes to ess/in_obs (clock sync) and we're IN an obs
    # (so [dservTimestamp ess/in_obs] == this obs's beginobs, matching the box's
    # anchor). Residual timing error is the sync-offset accuracy (sub-ms wired).
    #
    #   box_schedule_pulse office 5 200     ;# pulse box "office" pin 5, 200 ms from now
    #   box_schedule_timer office 3 500     ;# post extio/office/state/timer/3, 500 ms from now
    ########################################################################
    proc box_schedule_pulse {dev pin delay_ms} { box_schedule_ $dev do/$pin $delay_ms }
    proc box_schedule_timer {dev id  delay_ms} { box_schedule_ $dev timer/$id $delay_ms }

    proc box_schedule_ {dev key delay_ms} {
	variable io_class
	variable obs_pending
	if {$obs_pending} {
	    # +30 lead window: a scheduled onset is in flight and the box
	    # already holds the (provisional) epoch from the onset arming --
	    # the wire value is simply the offset from that epoch. This is
	    # the delivery-margin path: commands sent here arrive lead_ms
	    # before their earliest possible fire.
	    dservSet $io_class/$dev/cmd/$key/at [expr {int($delay_ms * 1000)}]
	    return 1
	}
	if {![dservExists ess/in_obs] || [dservGet ess/in_obs] == 0} {
	    ess_warning "box_schedule: not in an obs period (need the beginobs anchor)" "box"
	    return 0
	}
	# delta = (time already elapsed in this obs) + the requested delay, in us.
	set beginobs [dservTimestamp ess/in_obs]
	set delta [expr {([now] - $beginobs) + int($delay_ms * 1000)}]
	dservSet $io_class/$dev/cmd/$key/at $delta
	return 1
    }

    # Process a button GPIO event: update ess datapoint and wake state machine
    proc button_process {chan dpoint data} {
	variable buttons
	set val [expr {int($data) != 0}]
	set buttons(state,$chan) $val
	dservSet ess/button/$chan $val
	do_update
    }

    ########################################################################
    # button_init: bind a logical button channel to a source. The source is a
    # local GPIO pin, a joystick direction, or a remote I/O box DI line -- the
    # protocol only ever reads ess/button/<chan>, so it never knows which.
    #
    # Usage:
    #   button_init 0 24                  ;# local GPIO pin 24 -> channel 0
    #   button_init 0 {} joystick 4       ;# joystick value 4 -> channel 0
    #   button_init 1 {} joystick 8       ;# joystick value 8 -> channel 1
    #   button_init 0 {} box {office 14}  ;# box "office" DI pin 14 -> channel 0
    #                                     ;#   (extio/office/state/di/14)
    ########################################################################
    ########################################################################
    # Transport resolution: what does a binding point AT, right now?
    #
    # button_init has always answered this as a side effect of wiring -- the
    # dispatch and the dservAddMatch/gpioLineRequest live in one if/elseif
    # chain -- so the answer could not be obtained without activating a
    # channel, and could not be obtained AT ALL for a binding that resolves
    # to nothing. That is invisible in exactly the way that hurts: a
    # `box {* response left}` naming a group no live box announces looks
    # perfect in local/post-input.tcl and fails only when a subject presses.
    #
    # These procs are PURE -- they read datapoints and return a verdict, and
    # wire nothing. That is what makes them usable both by *_init and by a
    # status page, and testable without hardware.
    #
    # Resolution reuses button_group_map / button_group_bit, the same procs
    # the live path uses. A second implementation of label matching would
    # drift, and this file already carries two bugs that were exactly that.
    #
    # Adding a transport (a keyboard, a touchscreen region, an analog
    # threshold) is a registration here, not another arm of an if/elseif:
    #
    #   ::ess::input_transport key ::ess::resolve_key
    #
    # NOTE the split that remains: this registry decides what a binding
    # MEANS. Wiring it up is still button_init's if/elseif, so a NEW
    # transport needs a branch there too until that is table-driven as well.
    ########################################################################

    variable input_transports {}

    # Keyed by COMPONENT, not global. The same spec shape means different
    # things to different components: `box {office 14}` is a DI pin to a
    # button and `box {* joystick}` is a whole labelled GROUP to a joystick.
    # A single flat registry would have box_pin claim the joystick's binding
    # and resolve it to extio/*/state/di/joystick -- confidently wrong,
    # which is worse than unresolved.
    proc input_transport { kind name resolver } {
        variable input_transports
        dict set input_transports $kind $name $resolver
        return $name
    }

    proc input_transports { kind } {
        variable input_transports
        if { ![dict exists $input_transports $kind] } { return {} }
        return [dict keys [dict get $input_transports $kind]]
    }

    # A resolver takes {pin opts} and returns a dict, or "" to decline:
    #   status   ok | unresolved | ambiguous
    #   address  what it resolves to now (a datapoint, a pin, a bit)
    #   detail   human-readable, shown when status is not ok
    proc input_resolve { kind pin args } {
        variable input_transports
        set opts [expr {[llength $args] == 1 ? [lindex $args 0] : $args}]
        if { ![dict exists $input_transports $kind] } {
            return [dict create transport "" status unbound address "" \
                        detail "no transports registered for '$kind'"]
        }
        dict for { name resolver } [dict get $input_transports $kind] {
            set r [$resolver $pin $opts]
            if { $r ne "" } { return [dict merge {transport ""} $r [list transport $name]] }
        }
        return [dict create transport "" status unbound address "" \
                    detail "no transport claims this binding"]
    }

    # Expand a possibly-globbed box datapoint against what is LIVE. Returns
    # the concrete datapoint, or "" when no present box matches -- which is
    # the "declared but nothing answers it" case worth reporting.
    proc input_live_dpoint { pat } {
        if { [string first * $pat] < 0 } {
            return [expr {[dservExists $pat] ? $pat : ""}]
        }
        foreach k [dservKeys] { if { [string match $pat $k] } { return $k } }
        return ""
    }

    proc resolve_box_label { pin opts } {
        variable io_class
        if { ![dict exists $opts box] } { return "" }
        if { [llength [dict get $opts box]] < 3 } { return "" }
        lassign [dict get $opts box] dev grp lab
        # Resolve on the MANIFEST (state/group/<g>/pins), not on the group's
        # event datapoint. The event is a bitmask the box publishes when the
        # group CHANGES, so a live, correctly-configured box that nobody has
        # pressed since boot has not published one -- and resolving on it
        # would report a perfectly good binding as unresolved.
        set pat $io_class/$dev/state/group/$grp/pins
        set pinsdp [input_live_dpoint $pat]
        if { $pinsdp eq "" } {
            return [dict create status unresolved address "" \
                        detail "no live box announces group '$grp' (want $pat)"]
        }
        set gdp [string range $pinsdp 0 end-5]   ;# drop the trailing "/pins"
        set map [button_group_map $gdp]
        if { ![dict size $map] } {
            return [dict create status unresolved address $gdp \
                        detail "$gdp announces no labelled members yet"]
        }
        lassign [button_group_bit $map $lab] bit n
        if { $bit < 0 } {
            return [dict create status unresolved address $gdp \
                        detail "no member labelled '$lab' in {[dict keys $map]}"]
        }
        set st [expr {$n > 1 ? "ambiguous" : "ok"}]
        set d  [expr {$n > 1 ? "'$lab' matches $n members of {[dict keys $map]}" : ""}]
        return [dict create status $st address "$gdp bit $bit" detail $d]
    }

    proc resolve_box_pin { pin opts } {
        variable io_class
        if { ![dict exists $opts box] } { return "" }
        lassign [dict get $opts box] dev bpin
        set pat $io_class/$dev/state/di/$bpin
        set dp [input_live_dpoint $pat]
        if { $dp eq "" } {
            return [dict create status unresolved address "" \
                        detail "no live box publishes $pat"]
        }
        return [dict create status ok address $dp detail ""]
    }

    proc resolve_joystick_bit { pin opts } {
        if { ![dict exists $opts joystick] } { return "" }
        return [dict create status ok \
                    address "joystick bit [dict get $opts joystick]" detail ""]
    }

    proc resolve_gpio { pin opts } {
        if { $pin eq "" } { return "" }
        # A local line cannot be probed the way a box manifest can -- asking
        # the kernel would mean requesting it, which is the wiring we are
        # deliberately not doing. Declared is the honest verdict.
        return [dict create status ok address "gpio pin $pin" \
                    detail "declared, not probed"]
    }

    # A joystick binds the GROUP itself and derives its direction map from
    # the members' labels, so "does this resolve" means "does a live box
    # announce that group, with directions in it".
    proc resolve_box_group { pin opts } {
        variable io_class
        if { ![dict exists $opts box] } { return "" }
        lassign [dict get $opts box] dev grp
        if { $grp eq "" } { return "" }
        set pat $io_class/$dev/state/group/$grp/pins
        set pinsdp [input_live_dpoint $pat]
        if { $pinsdp eq "" } {
            return [dict create status unresolved address "" \
                        detail "no live box announces group '$grp' (want $pat)"]
        }
        set gdp [string range $pinsdp 0 end-5]
        set map [button_group_map $gdp]
        if { ![dict size $map] } {
            return [dict create status unresolved address $gdp \
                        detail "$gdp announces no labelled members yet"]
        }
        # a d-pad needs DIRECTIONS; a group of unrelated labels resolves but
        # will not steer, and saying so beats a joystick that never answers
        set dirs {}
        foreach lab [dict keys $map] {
            if { [joystick_dir_canon $lab] ne "" } { lappend dirs $lab }
        }
        if { ![llength $dirs] } {
            return [dict create status unresolved address $gdp \
                        detail "no direction labels in {[dict keys $map]}"]
        }
        return [dict create status ok address "$gdp dirs {$dirs}" detail ""]
    }

    # An analog joystick resolves through the SLIDER, so "does this resolve"
    # is really two questions: is anything publishing a position, and is the
    # slider subprocess pointed at the stick rather than at some other analog
    # input. The second is the one that bites -- sliderconf's `source` gate
    # makes process_stick return on its first line, so a perfectly configured
    # box streams at 200 Hz into a subprocess that drops every block, and
    # nothing anywhere says so. Name it here, where someone is already asking
    # why the joystick does not answer.
    proc resolve_joystick_analog { pin opts } {
        if { ![dict exists $opts analog] } { return "" }
        set cfg [dict get $opts analog]
        set dp [expr {[dict exists $cfg dpoint] ?
                      [dict get $cfg dpoint] : "slider/position"}]

        set src ""
        if { [dservExists slider/settings] } {
            catch { set src [dict get [dservGet slider/settings] source] }
        }
        if { $src ne "" && $src ni {extio auto} } {
            return [dict create status unresolved address $dp \
                        detail "slider source is '$src', so the extio stick path\
                                is gated off (want extio, or auto)"]
        }
        if { ![dservExists $dp] } {
            set hint [expr {$src eq "" ? "is the slider subprocess running?" :
                            "no stick group has published yet"}]
            return [dict create status unresolved address $dp \
                        detail "nothing publishes $dp -- $hint"]
        }
        return [dict create status ok address $dp \
                    detail [expr {$src eq "auto" ?
                        "slider source is 'auto'; set it explicitly on a rig\
                         with more than one analog input wired" : ""}]]
    }

    input_transport button   box_label    ::ess::resolve_box_label
    input_transport button   box_pin      ::ess::resolve_box_pin
    input_transport button   joystick_bit ::ess::resolve_joystick_bit
    input_transport button   gpio         ::ess::resolve_gpio
    input_transport joystick box_group    ::ess::resolve_box_group
    input_transport joystick analog       ::ess::resolve_joystick_analog

    # What a button CHANNEL resolves to, honouring the rig override exactly
    # as button_init does. Pure.
    proc button_resolve { chan } {
        variable button_bindings
        variable buttons
        if { [info exists button_bindings($chan)] } {
            set ov  $button_bindings($chan)
            set pin [lindex $ov 0]
            set opts [lrange $ov 1 end]
            set src rig
        } elseif { [info exists buttons(req,$chan)] } {
            lassign $buttons(req,$chan) pin opts
            set src protocol
        } else {
            return [dict create transport "" status unbound address "" \
                        detail "channel $chan is not bound" origin ""]
        }
        return [dict merge [input_resolve button $pin $opts] [list origin $src]]
    }

    proc joystick_resolve {} {
        variable joystick_binding
        variable joystick_req
        if { [llength $joystick_binding] } {
            set b $joystick_binding; set src rig
        } elseif { [info exists joystick_req] && [llength $joystick_req] } {
            set b $joystick_req; set src protocol
        } else {
            return [dict create transport "" status unbound address "" \
                        detail "no joystick binding" origin ""]
        }
        return [dict merge \
                    [input_resolve joystick [lindex $b 0] [lrange $b 1 end]] \
                    [list origin $src]]
    }

    proc input_publish_status { key r } {
        dservSet ess/inputs/$key \
            [list status [dict get $r status] \
                 transport [dict get $r transport] \
                 address [dict get $r address] \
                 origin [dict get $r origin] \
                 detail [dict get $r detail]]
        return $r
    }

    proc joystick_publish_status {} {
        return [input_publish_status joystick [joystick_resolve]]
    }

    # ess/inputs/button/<chan> -- what this channel points at and whether
    # anything answers it. Published on init so a page can show the rig's
    # wiring without pressing anything.
    proc button_publish_status { chan } {
        return [input_publish_status button/$chan [button_resolve $chan]]
    }

    # Every channel that has an opinion: initialised ones, plus any bound by
    # a rig before a protocol asked for them.
    proc button_publish_all {} {
        variable buttons
        variable button_bindings
        set chans {}
        if { [info exists buttons(n_channels)] } {
            for { set i 0 } { $i < $buttons(n_channels) } { incr i } { lappend chans $i }
        }
        foreach c [array names button_bindings] { lappend chans $c }
        foreach c [lsort -unique $chans] { catch { button_publish_status $c } }
        return
    }

    ########################################################################
    # A BOX RELABEL MUST INVALIDATE THE DERIVED MAPS
    #
    # button_group_map and joystick_map_for both cache label->bit maps, and
    # nothing cleared them when a box's labels changed. So after relabelling a
    # box, ess/inputs/* kept answering from the OLD labels -- and it answered
    # in the alarming direction, reporting
    #
    #     unresolved ... {no direction labels in {joy_down joy_up ...}}
    #
    # for a rig that was in fact fine, while joystick_map_for (whose own cache
    # joystick_init had cleared) already had the right map. Two caches, one
    # flushed, one not, disagreeing about the same box. Found on psychophysics
    # 2026-08-21 after swapping in a new box and relabelling it.
    #
    # extioconf already does this for its decoded-label map; the transports
    # did not. Same fix, same trigger: the box re-announces state/label/<pin>
    # and state/group/<g>/pins on any live change, so watch those.
    #
    # PURGE EVERYTHING rather than the one device that changed. The maps are
    # small and rebuilt lazily on the next lookup, a relabel is rare, and
    # scoping the purge would mean parsing the datapoint to find its device --
    # more code and another thing to get subtly wrong for no measurable gain.
    #
    # button_group_warned goes too: it suppresses repeat "no such label"
    # warnings, and a relabel is exactly when someone wants to hear them again.
    proc input_maps_invalidate { args } {
        variable button_group_maps
        variable button_group_warned
        variable joystick_maps
        array unset button_group_maps   ; array set button_group_maps   {}
        array unset button_group_warned ; array set button_group_warned {}
        array unset joystick_maps       ; array set joystick_maps       {}
        # Re-resolve now rather than at the next press: ess/inputs/* is what a
        # page and an operator read to answer "is this rig wired up", and a
        # stale answer there is the whole bug.
        catch { joystick_publish_status }
        catch { button_publish_all }
        return
    }

    # Registered at load. Glob matches, so a box appearing later is covered
    # without re-registering. dpointAddScript (not Set) so this cannot delete
    # another subsystem's handler on the same pattern.
    proc input_watch_labels {} {
        variable io_class
        foreach pat [list $io_class/*/state/label/* \
                          $io_class/*/state/group/*/pins] {
            catch { dservAddMatch   $pat }
            catch { dpointAddScript $pat ::ess::input_maps_invalidate }
        }
        return
    }
    catch { input_watch_labels }

    proc button_init {chan {pin {}} args} {
	variable buttons
	variable io_class
	variable button_bindings

	# a rig-level override (from post-pins) wins over the protocol's request,
	# so a box/joystick can stand in for a local pin without protocol edits.
	if {[info exists button_bindings($chan)]} {
	    set ov   $button_bindings($chan)
	    set pin  [lindex $ov 0]
	    set args [lrange $ov 1 end]
	}

	# remember what was ASKED for, so button_resolve can report a channel
	# whose binding resolves to nothing without re-deriving the override.
	set buttons(req,$chan) [list $pin $args]
	# publish the verdict for this channel now that we know what it asked
	# for; deferred to the end would miss the early-return paths below
	catch { button_publish_status $chan }

	# initialize channel state and datapoint
	set buttons(state,$chan) 0
	dservSet ess/button/$chan 0

	# parse options with defaults. $args is the flat option dict (e.g.
	# "joystick 4" or "debounce_us 5000 pull PULL_DOWN"); pass it AS a dict.
	# {*}$args would expand it into separate words, so dict merge would see
	# "joystick"/"4" as two dicts -> "missing value to go with key".
	set opts [dict merge \
		      {debounce_us 2500 pull PULL_UP active ACTIVE_LOW} \
		      $args]

	if {[dict exists $opts box] && [llength [dict get $opts box]] >= 3} {
	    # bind this channel to a labeled member of a box chord GROUP:
	    #   box {dev group label}  ->  the group member whose manifest label is
	    #   <label>, resolved per-board from state/group/<g>/pins + state/label/<n>.
	    #   e.g. button_init 0 {} box {* response left}   (any box, hot-swap)
	    lassign [dict get $opts box] dev grp lab
	    button_group_bind $chan $dev $grp $lab

	} elseif {[dict exists $opts box]} {
	    # bind this channel to a remote I/O box DI line:
	    #   <io_class>/<device>/state/di/<pin>   e.g. extio/office/state/di/14
	    # The box publishes LOGICAL levels (configure the box pin `active_low`
	    # so pressed=1), so button_process needs no inversion here.
	    #
	    # A GLOB <device> (contains '*', e.g. "{* 14}") binds to whatever box is
	    # present at PUBLISH time -> hot-swap-transparent: a box can be unplugged /
	    # renamed / replaced and its di/<pin> still drives this channel, with no
	    # re-binding. A literal <device> binds to exactly that box.
	    lassign [dict get $opts box] dev bpin
	    set dp $io_class/$dev/state/di/$bpin

	    proc ::ess::button_process_$chan {dpoint data} \
		"::ess::button_process $chan \$dpoint \$data"

	    if {[string first * $dev] >= 0} {
		dservAddMatch $dp                              ;# glob: any matching box, live
		dpointSetScript $dp ::ess::button_process_$chan
		foreach k [dservKeys] {                        ;# initial level from an already-present box
		    if {[string match $dp $k]} { button_process $chan $k [dservGet $k]; break }
		}
	    } else {
		dservAddExactMatch $dp
		dpointSetScript $dp ::ess::button_process_$chan
		if {[dservExists $dp]} { button_process $chan $dp [dservGet $dp] }
	    }

	    set buttons(box,$chan) $dp

	} elseif {[dict exists $opts joystick]} {
	    # bind this channel to a joystick direction value
	    set joy_val [dict get $opts joystick]
	    set buttons(joy_source,$chan) $joy_val

	} elseif {$pin ne {}} {
	    # existing GPIO path - unchanged
	    try {
		gpioLineRequestInput $pin BOTH \
		    [dict get $opts debounce_us] \
		    [dict get $opts pull] \
		    [dict get $opts active]

		proc ::ess::button_process_$chan {dpoint data} \
		    "::ess::button_process $chan \$dpoint \$data"

		dservAddExactMatch gpio/input/$pin
		dservTouch gpio/input/$pin
		dpointSetScript gpio/input/$pin ::ess::button_process_$chan

		set buttons(pin,$chan) $pin
		if {![dservExists gpio/input/$pin]} {
		    dservSet gpio/input/$pin 0
		}
	    } on error {msg} {
		ess_warning "button_init: no GPIO for pin $pin (simulation mode): $msg" "button"
	    }
	}

	# track channel count
	if {$chan >= $buttons(n_channels)} {
	    set buttons(n_channels) [expr {$chan + 1}]
	}

	# publish active channel list so UI can show virtual buttons
	button_publish_channels
    }

    # Publish list of initialized button channels to ess/buttons/channels
    proc button_publish_channels {} {
	variable buttons
	set channels {}
	for {set i 0} {$i < $buttons(n_channels)} {incr i} {
	    lappend channels $i
	}
	dservSet ess/buttons/channels $channels
    }

    # Check if a specific button channel is pressed
    proc button_pressed {chan} {
	variable buttons
	if {![info exists buttons(state,$chan)]} { return 0 }
	return $buttons(state,$chan)
    }

    # Return first active button channel, or -1 if none
    proc button_active {} {
	variable buttons
	for {set i 0} {$i < $buttons(n_channels)} {incr i} {
	    if {$buttons(state,$i)} { return $i }
	}
	return -1
    }

    # Simulate a button press/release for testing without hardware
    # Only triggers state machine on actual state change (edge-triggered)
    proc button_simulate {chan val} {
	variable buttons
	set val [expr {int($val) != 0}]
	if {[info exists buttons(state,$chan)] && $buttons(state,$chan) == $val} {
	    return
	}
	set buttons(state,$chan) $val
	dservSet ess/button/$chan $val
	do_update
    }

    ########################################################################
    # input simulation helpers (companions to button_simulate)
    #
    # Each owns the datapoint write + wire format and wakes the state
    # machine, so callers (simulators, agents, CLI tests) never hand-craft
    # binary blobs. Drive the SAME datapoints real hardware does, so the
    # full input path (processors, window membership, commit detection) is
    # exercised exactly as in a live session.
    ########################################################################

    # Analog swipe response: publish a commit (magnitude then angle, in
    # radians), matching the order sliderconf emits on a real trackpad
    # RELEASE. The angle is what response-detection keys off; mag must
    # exceed the system's swipe_threshold to count as engaged.
    proc swipe_simulate { angle_rad {mag 2.0} } {
	dservSetData slider/swipe/mag   [now] 2 [binary format f $mag]
	dservSetData slider/swipe/angle [now] 2 [binary format f $angle_rad]
	do_update
    }

    # Touchscreen press+release at a pixel coordinate, injected via
    # mtouch/event (the same path the web GUI uses) so the touch processor
    # computes window membership. event_type: 0=press, 2=release.
    proc touch_simulate { x y } {
	dservSetData mtouch/event [now] 4 [binary format s3 [list $x $y 0]]
	dservSetData mtouch/event [now] 4 [binary format s3 [list $x $y 2]]
	do_update
    }

    # Convenience: tap the center of touch window <win> (reads the window's
    # stored pixel center), so a caller can "respond in window N" without
    # knowing screen geometry.
    proc touch_win_simulate { win } {
	set cx [touchGetIndexedParam $win center_x]
	set cy [touchGetIndexedParam $win center_y]
	touch_simulate $cx $cy
    }

    # Eye position: delegate to the virtual_eye subprocess (the eye analog of
    # "the device"), which republishes the held position via its timer so a
    # fixation persists. h/v are in DEGREES, same space as em windows. Unlike
    # the transient touch/swipe helpers, this SETS A HELD position: call again
    # to saccade, or set outside all windows to break fixation. No do_update
    # here — the feed is async (timer -> em::process_virtual -> windows
    # processor) and em_window_process wakes the state machine on the actual
    # membership change.
    proc em_simulate { h v } {
	if { ![dservExists eyetracking/virtual_enabled] ||
	     ![dservGet eyetracking/virtual_enabled] } {
	    send virtual_eye start
	}
	send virtual_eye "set_eye $h $v"
    }

    # Convenience: move the (virtual) eye to the center of em window <win>,
    # e.g. to acquire a fixation or land a saccade on a choice window. Eye
    # windows and set_eye are both in degrees, so no conversion is needed.
    proc em_win_simulate { win } {
	set cx [ainGetIndexedParam $win center_x]
	set cy [ainGetIndexedParam $win center_y]
	em_simulate $cx $cy
    }

    ########################################################################
    # aggregate button query helpers
    ########################################################################
    
    # Return 1 if any button channel is currently pressed
    proc button_any_pressed {} {
	variable buttons
	for {set i 0} {$i < $buttons(n_channels)} {incr i} {
	    if {$buttons(state,$i)} { return 1 }
	}
	return 0
    }
    
    # Return 1 if no button channels are pressed
    proc button_none_pressed {} {
	return [expr {![button_any_pressed]}]
    }
    
    # Return 1 if all initialized button channels are pressed
    proc button_all_pressed {} {
	variable buttons
	if {$buttons(n_channels) == 0} { return 0 }
	for {set i 0} {$i < $buttons(n_channels)} {incr i} {
	    if {!$buttons(state,$i)} { return 0 }
	}
	return 1
    }
    
    # Return count of currently pressed button channels
    proc button_count {} {
	variable buttons
	set count 0
	for {set i 0} {$i < $buttons(n_channels)} {incr i} {
	    if {$buttons(state,$i)} { incr count }
	}
	return $count
    }
    
    # Return list of currently pressed channel indices
    proc button_active_list {} {
	variable buttons
	set result {}
	for {set i 0} {$i < $buttons(n_channels)} {incr i} {
	    if {$buttons(state,$i)} { lappend result $i }
	}
	return $result
    }

    ########################################################################
    # button_deinit: also clean up joystick-bound channels
    ########################################################################
    proc button_deinit {} {
	variable buttons
	variable button_group_chans
	variable button_group_maps
	variable button_group_warned
	for {set i 0} {$i < $buttons(n_channels)} {incr i} {
	    if {[info exists buttons(pin,$i)]} {
		dservRemoveMatch gpio/input/$buttons(pin,$i)
		unset buttons(pin,$i)
	    }
	    if {[info exists buttons(box,$i)]} {
		dservRemoveMatch $buttons(box,$i)
		unset buttons(box,$i)
	    }
	    if {[info exists buttons(grp,$i)]} {
		unset buttons(grp,$i)
	    }
	    if {[info exists buttons(joy_source,$i)]} {
		unset buttons(joy_source,$i)
	    }
	    if {[info exists buttons(state,$i)]} {
		unset buttons(state,$i)
	    }
	    # The protocol's request dies with the system; the RIG binding
	    # outlives it. Re-publishing rather than clearing means the page
	    # still shows what this rig would answer with, and shows it as
	    # origin "rig" rather than "protocol".
	    unset -nocomplain buttons(req,$i)
	    catch { button_publish_status $i }
	    dservSet ess/button/$i 0
	}
	# drop the shared group dispatchers (one per bound group pattern)
	foreach gdp [array names button_group_chans] {
	    catch { dpointSetScript $gdp {} }
	    catch { dservRemoveMatch $gdp }
	}
	array unset button_group_chans;  array set button_group_chans {}
	array unset button_group_maps;   array set button_group_maps {}
	array unset button_group_warned; array set button_group_warned {}
	set buttons(n_channels) 0
	dservSet ess/buttons/channels {}
    }
}

###############################################################################
########################### stick shaping + slider ############################
###############################################################################

namespace eval ess {

    ###########################################################################
    ######################## analog stick response shaping ####################
    ###########################################################################
    #
    # Turning a stick's deflection into a rate. Shared, because this had been
    # written twice already -- ::ess::dial's `stick` source (1-D, rotating a
    # bearing) and its `astick` source (2-D, driving a cursor) -- and a third
    # copy was about to go into the roaming mode.
    #
    # That is the exact failure ess_dial exists to have fixed: it was
    # extracted from two protocols carrying the same ~40-line machine, and
    # the two copies had DRIFTED into two separate bugs. Two copies of this
    # had not diverged yet. Three would have.
    #
    # Lives here rather than in ess_dial so a roaming mode can use it without
    # depending on the dial. Resolution is at call time, so ess_dial being
    # `package require`d at the top of this file is not an ordering problem.

    # Shape a deflection FRACTION (|deflection| / full_scale) into a 0..1
    # gain. Returns 0.0 inside the deadzone.
    #
    # The rescale past the deadzone is the part worth not re-deriving:
    # without it, clearing an 8% deadzone jumps straight to 8% of full rate,
    # so the fine-control region does not merely start small, it does not
    # exist. Matters more, not less, with a Hall-effect stick whose deadzone
    # can be tiny.
    #
    # expo shapes the curve: 1.0 linear; 2-3 gives a slow, fine region near
    # center with full speed still available at the edge -- one knob for
    # "gentle gain" and "fast gain" at once. Clamped BEFORE the exponent, so
    # over-range deflection cannot exceed full rate.
    proc stick_gain { f deadzone expo } {
        if { $f < $deadzone } { return 0.0 }
        set g [expr {($f - $deadzone)/(1.0 - $deadzone)}]
        if { $g <= 0.0 } { return 0.0 }
        if { $g > 1.0 } { set g 1.0 }
        if { $expo != 1.0 } { set g [expr {pow($g, $expo)}] }
        return $g
    }

    # A 2-D deflection -> a velocity vector in units/s. {0.0 0.0} at rest.
    #
    # Speed comes from the MAGNITUDE and direction from the raw vector, never
    # per-axis. Shaping x and y separately bends the path: with expo 2 a push
    # at 45 degrees has both components squared, which happens to preserve
    # the diagonal and NO other bearing, so the cursor bows away from where
    # the hand is actually pointing.
    proc stick_velocity { x y scale deadzone expo rate } {
        if { $scale <= 0.0 } { return {0.0 0.0} }
        set mag [expr {sqrt($x*$x + $y*$y)}]
        if { $mag <= 0.0 } { return {0.0 0.0} }
        set g [stick_gain [expr {$mag/$scale}] $deadzone $expo]
        if { $g <= 0.0 } { return {0.0 0.0} }
        set speed [expr {$rate*$g}]
        return [list [expr {$x/$mag*$speed}] [expr {$y/$mag*$speed}]]
    }

    ###########################################################################
    ############################## slider support ############################
    ###########################################################################
    #
    # Mirrors em_init: makes calibrated slider position available to state
    # machines as slider_x / slider_y / slider_pos without each protocol
    # having to subscribe manually. The slider subprocess (sliderconf.tcl)
    # owns calibration; this end just reads the processed values.
    #
    # slider/position is published by sliderconf as a DSERV_FLOAT pair, so
    # dserv auto-decodes $data into a 2-element Tcl list on dispatch.
    #
    # Tracks the mode passed to slider_init so slider_process knows
    # whether to wake the state machine on every position update. In
    # swipe mode we need the SM to tick on each sample so the cursor
    # follows the finger live; in other modes (ain pot at kHz) we
    # don't.
    variable slider_mode ""

    proc slider_process { dpoint data } {
        variable slider_x
        variable slider_y
        variable slider_mode
        lassign $data x y
        if { $x ne "" } { set slider_x $x }
        if { $y ne "" } { set slider_y $y }
        # NO wake here, in any mode.
        #
        # This used to wake the state machine on every sample in swipe mode,
        # because the live cursor was computed inside a protocol's
        # responded_swipe and the SM's tick was the only place that code
        # could run. That made the SM's update rate the DEVICE's rate: fine
        # for a 62.5 Hz trackpad, but 1000 updates a second the moment a
        # 1 kHz mouse feeds the slider -- the accidental-polling pathology.
        #
        # Cursor work now lives in ::ess::dial_slider_sample, its own
        # observer on slider/position (dpointAddScript), so it runs at the
        # device's rate without involving the state machine at all. The SM
        # is still woken by the swipe COMMIT and by engagement edges, which
        # are events it must actually decide something about.
        #
        # A protocol doing its own cursor work outside the dial should add
        # its own observer rather than reinstating this -- all three that
        # used to (motionpatch, mp_pulsed, ricochet) are now on the dial.
    }

    # swipe-mode side channels (only published when slider is in swipe mode).
    # slider/swipe/angle    : float, radians [0, 2π), one shot per commit
    # slider/swipe/mag      : float, commit swipe distance (confidence proxy)
    # slider/swipe/engaged  : 0/1 string, edges on engagement threshold
    variable slider_swipe_angle   0.0
    variable slider_swipe_mag     0.0
    variable slider_swipe_time    0
    variable slider_swipe_engaged 0

    proc slider_swipe_process { dpoint data } {
        variable slider_swipe_angle
        variable slider_swipe_time
        set v [lindex $data 0]
        if { $v ne "" } { set slider_swipe_angle $v }
        set slider_swipe_time [dservTimestamp $dpoint]
        # Wake the SM so responded_swipe can pick up the commit.
        do_update
    }

    proc slider_swipe_mag_process { dpoint data } {
        variable slider_swipe_mag
        set v [lindex $data 0]
        if { $v ne "" } { set slider_swipe_mag $v }
        # No do_update here: the paired slider/swipe/angle commit wakes
        # the SM, and sliderconf publishes mag just before angle so this
        # value is already current when responded_swipe reads it.
    }

    proc slider_swipe_engaged_process { dpoint data } {
        variable slider_swipe_engaged
        set slider_swipe_engaged $data
        # Wake the SM on engagement edges so the cursor show/hide
        # transitions don't have to wait for the next timer fire.
        do_update
    }

    # ::ess::slider_init -mode <absolute|continuous|swipe>
    #                    ?-release <hold|stop|recenter>?
    #                    ?-threshold <magnitude>?
    #
    # -mode is required. It tells the slider subprocess how to interpret
    # contact-based input (trackpad, virtual press_drag_release):
    #   absolute   - PRESS jumps to mapped position; DRAG follows it.
    #                (Hand-in-space / pointing paradigms.)
    #   continuous - PRESS holds last value; DRAG accumulates deltas
    #                from the press point. (Steering paradigm.)
    #   swipe      - Like absolute for live position, plus engagement
    #                tracking. On RELEASE, if magnitude ever crossed
    #                -threshold during the touch, publishes the
    #                committed angle to slider/swipe/angle (radians).
    #                Engagement edges go to slider/swipe/engaged so
    #                consumers can show/hide a cursor without recomputing
    #                magnitude themselves.
    #
    # On a pot-based rig (ain source) the mode setting has no effect —
    # pot is always continuous.
    #
    # -release defaults to "hold" (cursor stays at last position after
    # RELEASE). Set "recenter" to snap to (0, 0) or "stop" to freeze
    # publishing and let consumers key off slider/active.
    #
    # -threshold sets the swipe-mode engagement magnitude in calibrated
    # output units. Ignored in absolute / continuous modes.
    proc slider_init { args } {
        set mode ""
        set release "hold"
        set threshold ""
        for { set i 0 } { $i < [llength $args] } { incr i 2 } {
            set k [lindex $args $i]
            set v [lindex $args [expr {$i + 1}]]
            switch -- $k {
                -mode      { set mode $v }
                -release   { set release $v }
                -threshold { set threshold $v }
                default  {
                    error "::ess::slider_init: unknown option '$k'"
                }
            }
        }
        if { $mode eq "" } {
            error "::ess::slider_init requires -mode {absolute|continuous|swipe}"
        }

        # Forward semantics to the slider subprocess.
        send slider "slider::set_mode $mode"
        send slider "slider::set_release $release"
        if { $threshold ne "" } {
            send slider "slider::set_swipe_threshold $threshold"
        }

        # Record mode so slider_process knows whether to wake the SM.
        variable slider_mode
        set slider_mode $mode

        # set flag so data files automatically log slider data
        variable slider_active
        set slider_active 1

        # start from a known position rather than leaving whatever stale
        # values were left over from a previous protocol
        variable slider_x
        variable slider_y
        set slider_x 0.0
        set slider_y 0.0

        # reset swipe-mode side channels too
        variable slider_swipe_angle
        variable slider_swipe_mag
        variable slider_swipe_time
        variable slider_swipe_engaged
        set slider_swipe_angle   0.0
        set slider_swipe_mag     0.0
        set slider_swipe_time    0
        set slider_swipe_engaged 0

        # AddScript, not SetScript: this registers OUR handler and must not
        # clobber another subsystem's on the same datapoint. ::ess::dial
        # observes slider/position too, and with SetScript whichever
        # initialized second silently deleted the first -- which is exactly
        # what happened to ricochet, where dial_init runs before slider_init.
        # append() dedupes, so calling slider_init twice still registers once.
        dservAddExactMatch slider/position
        dpointAddScript    slider/position ::ess::slider_process

        # Subscribe to swipe-mode side channels. Harmless in other modes
        # (sliderconf never publishes them outside swipe).
        dservAddExactMatch slider/swipe/angle
        dpointAddScript    slider/swipe/angle ::ess::slider_swipe_process
        dservAddExactMatch slider/swipe/mag
        dpointAddScript    slider/swipe/mag ::ess::slider_swipe_mag_process
        dservAddExactMatch slider/swipe/engaged
        dpointAddScript    slider/swipe/engaged ::ess::slider_swipe_engaged_process

        # Publish so the frontend can conditionally show the slider panel
        dservSet ess/slider_active 1
    }

    proc slider_deinit {} {
        variable slider_active
        # Remove only OUR handlers, leaving any other subsystem's on the
        # same datapoints (the dial's, notably).
        catch { dpointRemoveScript slider/position ::ess::slider_process }
        catch { dpointRemoveScript slider/swipe/angle ::ess::slider_swipe_process }
        catch { dpointRemoveScript slider/swipe/mag ::ess::slider_swipe_mag_process }
        catch { dpointRemoveScript slider/swipe/engaged \
                    ::ess::slider_swipe_engaged_process }
        set slider_active 0
        dservSet ess/slider_active 0
    }

    proc slider_x {} {
        variable slider_x
        return $slider_x
    }

    proc slider_y {} {
        variable slider_y
        return $slider_y
    }

    proc slider_pos {} {
        variable slider_x
        variable slider_y
        return [list $slider_x $slider_y]
    }

    # swipe-mode accessors.
    #
    # slider_swipe_angle   - last committed swipe angle in radians [0, 2π).
    #                        Only meaningful when slider_swipe_time has
    #                        advanced since the protocol last consumed it
    #                        (e.g., after cue_on_time).
    # slider_swipe_time    - dservTimestamp (µs) of the last commit.
    #                        Protocol code compares this to its own
    #                        cue_on_time / last-processed marker to detect
    #                        a fresh commit.
    # slider_swipe_engaged - 0/1. 1 from the moment magnitude first crosses
    #                        the swipe threshold during a touch, back to 0
    #                        on RELEASE. Use for cursor show/hide.
    proc slider_swipe_angle {} {
        variable slider_swipe_angle
        return $slider_swipe_angle
    }
    # slider_swipe_mag - commit swipe distance for the last commit
    #                    (paired with slider_swipe_angle / _time). Confidence
    #                    proxy: larger = pushed further. Same freshness
    #                    semantics as slider_swipe_angle (gate on _time).
    proc slider_swipe_mag {} {
        variable slider_swipe_mag
        return $slider_swipe_mag
    }
    proc slider_swipe_time {} {
        variable slider_swipe_time
        return $slider_swipe_time
    }
    proc slider_swipe_engaged {} {
        variable slider_swipe_engaged
        return $slider_swipe_engaged
    }

    # Publish a one-shot virtual slider sample. Useful for yoked playback
    # where the state machine steps through a recorded trajectory on its
    # own schedule, or for edge cases that want to force a specific value
    # immediately. Values are in already-calibrated output units (same
    # coordinate system as slider/position).
    #
    # For *sustained* virtual input (browser UI drag, "hold position X
    # while I run a protocol", automated tests), prefer the virtual_slider
    # subprocess which emits a steady stream:
    #
    #     send virtual_slider "start 4"          ;# begin 250 Hz stream
    #     send virtual_slider "set_slider $x $y" ;# update held position
    #     send virtual_slider stop               ;# halt stream
    #
    # A steady stream structurally matches real hardware (which produces
    # samples continuously whether or not the subject moves), whereas
    # one-shots leave gaps that can confuse derivative computation or
    # "subject disengaged" heuristics.
    proc slider_virtual_set { x {y 0.0} } {
        dservSetData slider/virtual [now] 6 [list $x $y]
    }
}

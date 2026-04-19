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
#   slider/virtual   - already-calibrated [x y] values from browser / sim.
#
# Output:
#   slider/position  - calibrated [x y] as binary float pair (DSERV_FLOAT)
#   slider/active    - 0/1 engagement signal. Pot (ain) always 1 when the
#                      ain path is the active source. Trackpad 1 between
#                      PRESS and RELEASE. Consumers that want to react to
#                      subject engagement subscribe here.
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
        limit_y         -1.0]

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

    proc set_source     {s} { set_param source     $s }
    proc set_mode       {m} {
        if { $m ne "absolute" && $m ne "continuous" } {
            error "slider: invalid continuity_mode '$m' (want absolute|continuous)"
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
    proc set_scale_x    {s} { set_param scale_x    $s }
    proc set_scale_y    {s} { set_param scale_y    $s }
    proc set_center_x   {o} { set_param center_x   $o }
    proc set_center_y   {o} { set_param center_y   $o }
    proc set_deadzone_x {d} { set_param deadzone_x $d }
    proc set_deadzone_y {d} { set_param deadzone_y $d }
    proc set_invert_x   {o} { set_param invert_x   $o }
    proc set_invert_y   {o} { set_param invert_y   $o }
    proc set_limit_x    {l} { set_param limit_x    $l }
    proc set_limit_y    {l} { set_param limit_y    $l }

    # Set current raw position as center. Call this when the slider is
    # physically parked at the experimentally-neutral position.
    proc set_current_as_center {} {
        variable current_raw_x
        variable current_raw_y
        set_center_x $current_raw_x
        set_center_y $current_raw_y
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
    proc publish { x y } {
        variable last_x
        variable last_y
        set last_x $x
        set last_y $y

        set posvals [binary format ff $x $y]
        dservSetData slider/position [now] 2 $posvals ;# 2 = DSERV_FLOAT

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
                    # delta accumulation.
                    set trackpad_press_raw_x $nx
                    set trackpad_press_raw_y $ny
                    set trackpad_press_out_x $last_x
                    set trackpad_press_out_y $last_y
                    publish_active 1

                    if { $continuity_mode eq "absolute" } {
                        set x [calibrate_axis $nx $center_x $scale_x \
                                   $deadzone_x $invert_x $limit_x]
                        if { $chan_y >= 0 } {
                            set y [calibrate_axis $ny $center_y $scale_y \
                                       $deadzone_y $invert_y $limit_y]
                        } else {
                            set y 0.0
                        }
                        publish $x $y
                    }
                    # continuous mode on PRESS: hold last output, no publish
                }
                1 {
                    # DRAG
                    if { $continuity_mode eq "absolute" } {
                        set x [calibrate_axis $nx $center_x $scale_x \
                                   $deadzone_x $invert_x $limit_x]
                        if { $chan_y >= 0 } {
                            set y [calibrate_axis $ny $center_y $scale_y \
                                       $deadzone_y $invert_y $limit_y]
                        } else {
                            set y 0.0
                        }
                        publish $x $y
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
}

# Subscribe to the ain feed (primary hardware path)
dservAddExactMatch ain/vals
dpointSetScript    ain/vals slider::process_ain

# Subscribe to the virtual path (browser / simulator)
dservAddExactMatch slider/virtual
dpointSetScript    slider/virtual slider::process_virtual

# Subscribe to the trackpad feed (input subprocess, mtouch/trackpad).
# Range is published one-shot at input startup; cache it if it arrived
# before the slider subprocess came up, and subscribe for future updates
# (e.g., if the input subprocess restarts).
dservAddExactMatch mtouch/trackpad
dpointSetScript    mtouch/trackpad slider::process_trackpad

dservAddExactMatch mtouch/trackpad/range
dpointSetScript    mtouch/trackpad/range slider::set_trackpad_range

set existing_range [dservGet mtouch/trackpad/range]
if { [llength $existing_range] == 4 } {
    slider::set_trackpad_range mtouch/trackpad/range $existing_range
}

# Local deployment overrides (which ain channel the slider is wired to,
# per-rig calibration: center / scale / deadzone / invert / limit). Not
# tracked in git - each deployment owns its own local/slider.tcl. See
# local/slider.tcl.EXAMPLE for the template.
if { [file exists $dspath/local/slider.tcl] } {
    source $dspath/local/slider.tcl
}

puts "slider subprocessor started"

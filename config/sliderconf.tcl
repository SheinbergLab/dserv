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
#                      settings. This is the primary hardware path.
#   slider/virtual   - already-calibrated [x y] values from browser / sim
#
# Output:
#   slider/position  - calibrated [x y] as binary float pair (DSERV_FLOAT)
#   slider/raw       - raw [x y] as binary uint16 pair (DSERV_SHORT), in
#                      ADC counts; only emitted by the hardware path since
#                      the virtual path has no raw to report
#   ess/slider_pos   - "x y" string for visualization/monitoring
#   slider/settings  - current settings dict for UI introspection
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
    # chan_x / chan_y select which ain channels feed each axis. Set to -1
    # to disable that axis (output will be 0).
    #
    # limit_x / limit_y < 0 disables clamping on that axis.
    variable settings [dict create \
        chan_x        0 \
        chan_y       -1 \
        scale_x      1.0 \
        scale_y      1.0 \
        center_x  2048.0 \
        center_y  2048.0 \
        deadzone_x   0.0 \
        deadzone_y   0.0 \
        invert_x     0 \
        invert_y     0 \
        limit_x     -1.0 \
        limit_y     -1.0]

    # Track current raw values for "set center" functionality
    variable current_raw_x 0.0
    variable current_raw_y 0.0

    # Last calibrated output
    variable last_x 0.0
    variable last_y 0.0

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
    # Used by both hardware and virtual input paths.
    proc publish { x y } {
        variable last_x
        variable last_y
        set last_x $x
        set last_y $y

        set posvals [binary format ff $x $y]
        dservSetData slider/position [now] 2 $posvals ;# 2 = DSERV_FLOAT

        dservSet ess/slider_pos "$x $y"
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
    }

    # Process virtual slider input (already calibrated, in output units).
    # Mirrors em::process_virtual: straight passthrough so the browser /
    # simulator can drive slider/position without touching calibration.
    # No slider/raw publish here - the virtual path has no real raw value
    # to report, and conflating "fake raw" with true ADC counts would
    # confuse calibration UIs.
    proc process_virtual { dpoint data } {
        lassign $data x y
        if { $x eq "" } { set x 0.0 }
        if { $y eq "" } { set y 0.0 }
        publish $x $y
    }

    update_settings
}

# Subscribe to the ain feed (primary hardware path)
dservAddExactMatch ain/vals
dpointSetScript    ain/vals slider::process_ain

# Subscribe to the virtual path (browser / simulator)
dservAddExactMatch slider/virtual
dpointSetScript    slider/virtual slider::process_virtual

# Local deployment overrides (which ain channel the slider is wired to,
# per-rig calibration: center / scale / deadzone / invert / limit). Not
# tracked in git - each deployment owns its own local/slider.tcl. See
# local/slider.tcl.EXAMPLE for the template.
if { [file exists $dspath/local/slider.tcl] } {
    source $dspath/local/slider.tcl
}

puts "slider subprocessor started"

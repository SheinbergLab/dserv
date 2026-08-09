#
# extio-1.0.tm -- host-side helpers for interpreting extio box datapoints.
#
# One canonical home for the WIRE CONTRACTS an extio box publishes, so every
# consumer (virtual_eye, ess, analysis, ...) decodes them the same way and a
# firmware format change is a single-file edit rather than a hunt through every
# subprocess that inlined a `binary scan`. Pure interpretation + name building;
# no device policy -- what the numbers MEAN stays in each consumer.
#
# Companion to the firmware: block layout = wiznet-io/common/pico_ain_group.h,
# manifest/datapoint names = wiznet-io/pico/wizchip_dserv_config.c. BOX_CLASS
# ("extio") + <name> is the datapoint prefix (dserv_config.h dserv_cfg_prefix).
#

package provide extio 1.0

namespace eval ::extio {
    variable class extio    ;# BOX_CLASS

    # ---- datapoint name builders (read + config/adopt) ----
    proc state  {box leaf} { variable class; return $class/$box/state/$leaf }
    proc config {box key}  { variable class; return $class/$box/config/$key }
    proc cmd    {box key}  { variable class; return $class/$box/cmd/$key }

    # ---- self-describe column order ----
    # An announced "0,1" (analog channels) or "2,3,4,5" (DI group pins) string is
    # the authoritative column order for the packed datapoint -- decode columns
    # from the announced string alone (the same contract DI + analog groups share).
    proc columns {csv} { return [split $csv ,] }

    # ---- analog group block (state/ain/<label>) ----
    # 12-byte self-describing header + scan-major int16 samples in ascending
    # channel order -- see pico_ain_group.h:
    #   0 u8 ver | 1 u8 mask | 2 u8 nchan | 3 u8 count |
    #   4 u32 interval_us | 8 u16 flags | 10 u16 reserved | 12 int16[count*nchan]
    # Returns a dict; `samples` is the flat int16 list (count*nchan values).
    proc ain_decode {data} {
        binary scan $data cucucucuiusux2s* ver mask nchan count interval flags samples
        if {![info exists samples]} { set samples {} }
        return [dict create ver $ver mask $mask nchan $nchan count $count \
                    interval_us $interval flags $flags samples $samples]
    }

    # Flat samples -> list of per-scan rows {ch0 ch1 ...}, one row per instant.
    # Row k is sampled at (frame timestamp) + k*interval_us.
    proc ain_scans {d} {
        set n [dict get $d nchan]
        set s [dict get $d samples]
        set out {}
        if {$n <= 0} { return $out }
        for {set i 0} {$i+$n <= [llength $s]} {incr i $n} {
            lappend out [lrange $s $i [expr {$i + $n - 1}]]
        }
        return $out
    }

    # The newest scan's channel values (last row) -- what a position controller
    # (joystick -> eye) wants. {} if the block carried no samples.
    #
    # Does NOT go through ain_scans, deliberately. That builds EVERY row with an
    # lrange loop and this then discards all but the last -- O(count*nchan) work
    # for O(nchan) of data, on a path that runs at the eye sample rate (250 Hz
    # today). Measured in dserv's em interp on a Pi 5: 6.41 us at count=1 rising
    # to 16.97 us at count=16, against 2.32/6.32 us for this form.
    #
    # ALIGNMENT MATTERS: ain_scans emits only COMPLETE rows (its guard is
    # `$i+$n <= [llength $s]`), so a trailing partial sample is dropped. Taking
    # the last $n elements would straddle the boundary and silently return one
    # row shifted when the payload is not a whole multiple of nchan. Index the
    # last complete row instead.
    proc ain_latest {data} {
        set d   [ain_decode $data]
        set n   [dict get $d nchan]
        set s   [dict get $d samples]
        if {$n <= 0} { return {} }
        set rows [expr {[llength $s] / $n}]
        if {$rows < 1} { return {} }
        set start [expr {($rows - 1) * $n}]
        return [lrange $s $start [expr {$start + $n - 1}]]
    }

    # 1 iff the block's samples are boxcar-averaged (AIN_GROUP_FLAG_AVG).
    proc ain_averaged {d} { return [expr {[dict get $d flags] & 0x1}] }

    # ---- generic axis math: deadzone + gain about a center ----
    proc axis {raw center gain {deadzone 0}} {
        set delta [expr {$raw - $center}]
        if {abs($delta) <= $deadzone} { return 0.0 }
        return [expr {$delta * $gain}]
    }

    namespace export state config cmd columns \
        ain_decode ain_scans ain_latest ain_averaged axis
}

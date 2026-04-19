#
# input subprocess: kernel input devices (touchscreen, trackpad, ...)
#
# Loads dserv_input, seeds a known-device table of commonly attached
# hardware with sensible defaults, lets rigs override via local/input.tcl,
# runs one-shot autodiscover, and validates expectations.
#
# Default behavior on a fresh rig: plug in a known touchscreen and/or
# trackpad and restart dserv; both light up with no config. Unknown
# touchscreens need an `inputKnownDevice` or `inputConfigure` entry in
# local/input.tcl so their screen dimensions are known.
#
# Publishes:
#   mtouch/event           touchscreen  uint16[3] (x, y, event_type)
#                          byte-compatible with the previous dserv_touch.
#   mtouch/trackpad        trackpad     uint16[3] (x, y, event_type)
#                          raw trackpad-surface coords, primary contact only.
#   mtouch/trackpad/range  trackpad      int32[4] (min_x, max_x, min_y, max_y)
#                          one-shot at device open; slider subprocess maps
#                          surface coords into stimulus space using this.
#
# Event type: 0 = PRESS, 1 = DRAG, 2 = RELEASE (same semantics for both).
#
# Consumers (touch_windows processor, ess state systems via
# ::ess::touch_win_set, slider subprocess) see no change for existing
# paths. Trackpad consumption is opt-in by subscribing to mtouch/trackpad.
#
package require dlsh
package require qpcs

tcl::tm::add $dspath/lib

errormon enable
proc exit {args} { error "exit not available for this subprocess" }

load ${dspath}/modules/dserv_input[info sharedlibextension]

#
# Built-in known-device defaults. Matched against /dev/input/by-id/<name>
# or the libevdev-reported device name (Tcl glob). Touchscreens need
# screen dimensions declared somewhere; trackpads auto-enable by caps.
#
# local/input.tcl is sourced after these, so it can override any entry
# with the same pattern (last-match-wins) or add new ones.
#
inputKnownDevice touchscreen *wch.cn_USB2IIC_CTP_CONTROL* \
    -screen_w 1024 -screen_h 600
inputKnownDevice touchscreen *ILITEK_ILITEK-TP*           \
    -screen_w 1280 -screen_h 800
inputKnownDevice touchscreen *eGalax*                      \
    -screen_w 1280 -screen_h 800

# Per-rig overrides and expectations. Typical content:
#
#     # Force a non-default rotation for the touchscreen on this rig
#     inputKnownDevice touchscreen *ILITEK_ILITEK-TP* \
#         -screen_w 1280 -screen_h 800 -rotation 180
#
#     # Declare what this rig must have; startup fails if missing
#     inputExpect touchscreen
#     inputExpect trackpad -optional
#
set local_input [file join $dspath local input.tcl]
if { [file exists $local_input] } {
    source $local_input
}

# For debugging classification on a fresh rig. `inputProbe` enumerates
# every /dev/input/event* with identifiers, capability bits, and axis
# ranges — useful for picking inputKnownDevice patterns when adding new
# hardware, or diagnosing why a device isn't being picked up.
#
#   set DSERV_INPUT_PROBE=1 in the environment (or systemd unit) to dump
#   the probe report at startup. Otherwise invoke on demand:
#       % send input "inputProbe"
#
if { [info exists ::env(DSERV_INPUT_PROBE)] && $::env(DSERV_INPUT_PROBE) } {
    puts "input: probe report:"
    foreach d [inputProbe] {
        puts "  $d"
    }
}

# One-shot device enumeration. systemctl restart dserv to rescan.
set discovered [inputAutodiscover]
if { [dict size $discovered] > 0 } {
    puts "input: discovered $discovered"
}

# Fail startup loudly if any required class has no device.
inputValidateExpectations

puts "input subprocess configured"

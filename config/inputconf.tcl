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
#   mouse/event            mouse        uint16[3] (x, y, event_type)
#                          virtual cursor: relative counts integrated and
#                          clamped to the declared -screen_w/-screen_h.
#   mouse/event/range      mouse         int32[4] (min_x, max_x, min_y, max_y)
#                          the declared extent, at open and each reconnect.
#
# Event type: 0 = PRESS, 1 = DRAG, 2 = RELEASE (same semantics for all),
# plus 3 = MOVE for the mouse only — motion with no button held, which has
# no touch-device equivalent.
#
# Timestamps are the kernel's event time (evdev ev.time, switched to
# CLOCK_MONOTONIC at open), not the time the reader thread saw the event.
# See docs/input_layer.md, "Event timestamps".
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
#     # A dedicated subject mouse. Unlike touchscreen and trackpad, a
#     # mouse is NEVER adopted on capability match alone -- it needs an
#     # entry naming it, so the operator's desktop mouse can never be
#     # picked up by accident. Find the pattern with `inputProbe` (or
#     # `ls /dev/input/by-id`) and match the subject's device only.
#     #
#     #   -screen_w/-screen_h  extent the virtual cursor is clamped to
#     #   -gain                counts -> pixels, linear (no acceleration)
#     #   -grab                EVIOCGRAB: hide the device from the desktop
#     #   -move_every N        publish only every Nth MOTION sample. Button
#     #                        press/release are never decimated, so click
#     #                        timing keeps the device's full resolution.
#     #                        Use to cut fan-out from a 1kHz mouse without
#     #                        coarsening reaction times the way capping
#     #                        usbhid.mousepoll would.
#     #
#     inputKnownDevice mouse *Logitech_USB_Optical_Mouse* \
#         -screen_w 1920 -screen_h 1080 -gain 1.0 -grab 1
#
#     # Declare what this rig must have; startup fails if missing
#     inputExpect touchscreen
#     inputExpect trackpad -optional
#
# Guarded, for the same reason the generated file below is: a rig that
# comes up with NO INPUT DEVICES because of one bad line in a local file is
# far worse than one that warns and carries on. Before this guard, a single
# unknown command here aborted the whole config script -- so
# inputAutodiscover never ran, nothing was adopted, and the input page was
# simply empty with no indication why.
#
# NOTE this file is sourced in the INPUT subprocess, which has no `ess`
# package. ::ess::* commands (dial_bind, button_bind, joystick_bind) belong
# in a local/post-*.tcl file -- essconf.tcl sources those inside the ess
# interp. local/post-input.tcl is the one for input routing.
set local_input [file join $dspath local input.tcl]
if { [file exists $local_input] } {
    if { [catch { source $local_input } err] } {
        puts stderr "input: ERROR in $local_input: $err"
        puts stderr "input: continuing WITHOUT it -- devices below may be\
                     unconfigured. ::ess::* commands do not exist here;\
                     those belong in local/post-input.tcl."
    }
}

#
# Saved per-rig devices — the machine-owned half of the config.
#
# local/input.tcl is HAND-owned: comments, expectations, exotic overrides.
# local/input_devices.tcl is MACHINE-owned, written by inputSaveDevice
# (which the input.html page calls) and never hand-edited. Keeping them in
# separate files means saving from the page can rewrite its file wholesale
# without touching anything a human wrote.
#
# Sourced AFTER local/input.tcl so page-saved entries win on last-match:
# whatever you just adopted and saved is what comes back at next boot.
# Anything the page does not cover still belongs in local/input.tcl.
#
# Guarded by catch: a malformed generated file must not take the input
# subprocess down at boot. If it fails to load we warn, drop to whatever
# local/input.tcl declared, and carry on — a rig that boots with the wrong
# mouse is recoverable, one that does not boot is a trip to the rig.
#
set ::input_saved_file [file join $dspath local input_devices.tcl]
set ::input_saved_devices {}

if { [file exists $::input_saved_file] } {
    if { [catch { source $::input_saved_file } err] } {
        puts stderr "input: failed to load $::input_saved_file: $err"
        puts stderr "input: continuing without saved devices"
        set ::input_saved_devices {}
    }
}

foreach _e $::input_saved_devices {
    if { [catch {
        inputKnownDevice [dict get $_e class] [dict get $_e pattern] \
            {*}[dict get $_e opts]
    } err] } {
        puts stderr "input: bad saved entry ($_e): $err"
    }
}
unset -nocomplain _e

# Rewrite local/input_devices.tcl from ::input_saved_devices.
#
# Atomic: written to a temp file and renamed, so an interrupted save can
# never leave a half-written file for the next boot to source.
#
proc ::input_write_saved {} {
    set tmp $::input_saved_file.tmp
    set f [open $tmp w]
    puts $f "#"
    puts $f "# GENERATED by the input subprocess (inputSaveDevice)."
    puts $f "# Do not edit by hand — this file is rewritten wholesale."
    puts $f "# Hand-written per-rig config belongs in local/input.tcl."
    puts $f "#"
    puts $f "set ::input_saved_devices \{"
    foreach e $::input_saved_devices {
        puts $f "    [list $e]"
    }
    puts $f "\}"
    close $f
    file rename -force $tmp $::input_saved_file
    return $::input_saved_file
}

# inputSaveDevice class pattern ?-opt val ...?
#
# Records a device for this rig so it is adopted at next boot. Replaces
# any existing entry with the same class+pattern, so re-saving after a
# tweak updates rather than accumulating.
#
# Deliberately does NOT adopt the device — inputOpen does that, now and
# without persisting. The two are separate so a device can be tried and
# watched before anyone commits it to the rig's config.
#
proc ::inputSaveDevice {class pattern args} {
    set out {}
    foreach e $::input_saved_devices {
        if { [dict get $e class] eq $class &&
             [dict get $e pattern] eq $pattern } continue
        lappend out $e
    }
    lappend out [dict create class $class pattern $pattern opts $args]
    set ::input_saved_devices $out
    return [::input_write_saved]
}

# inputForgetDevice class pattern — drop a saved entry. Does not close a
# running device; the rig keeps working until the next restart.
proc ::inputForgetDevice {class pattern} {
    set out {}
    set found 0
    foreach e $::input_saved_devices {
        if { [dict get $e class] eq $class &&
             [dict get $e pattern] eq $pattern } { set found 1; continue }
        lappend out $e
    }
    set ::input_saved_devices $out
    ::input_write_saved
    return $found
}

# The saved table, for the page to display.
proc ::inputSavedDevices {} { return $::input_saved_devices }

# One round trip for everything input.html needs: registered classes,
# adopted devices, every enumerated node with its classification, and the
# saved per-rig table.
#
# JSON rather than the raw Tcl lists because the page would otherwise have
# to parse Tcl list quoting in JavaScript — and device names routinely
# contain spaces and braces, which is exactly where a hand-rolled parser
# gets it wrong.
#
# Host-level input configuration that silently changes what every device
# reports. usbhid.mousepoll overrides the polling interval for ALL HID
# pointing devices, so a rig can measure 62.5 Hz off a 1000 Hz mouse and
# conclude the hardware is slow — which is exactly what happened here on
# 2026-08-15, when this rig sat at 4294967295. Surfacing it means the next
# person reads the setting before believing a rate.
#
#   0        use each device's declared bInterval (the mainline default)
#   1..N     force N ms for every mouse
#   other    nonsense; on this rig it produced a 16 ms floor
#
# inputRequireHostPolling ?max_ms?
#
# Declare that this rig needs HID pointing devices polled at least this
# often. Checked at startup; a rig that does not meet it says so loudly
# instead of quietly reporting a rate that is the SETTING rather than the
# hardware.
#
# Why declare-and-check rather than set-and-fix: usbhid.mousepoll only
# affects devices probed AFTER it changes, and everything is enumerated
# long before dserv starts. Writing it here would look like it worked and
# change nothing until the next replug. The fix belongs on the kernel
# command line, where it lands before enumeration — and usbhid is built
# in on Raspberry Pi OS, so modprobe.d does not apply either.
#
# Raspberry Pi OS ships this capped at ~16 ms (62.5 Hz) to save CPU. On a
# mains-powered rig that trade is usually not the one you want: it costs
# every pointing device its report rate AND quantises button timestamps,
# which is what a reaction time is measured from.
#
set ::input_required_poll_ms 0

proc ::inputRequireHostPolling {{max_ms 1}} {
    set ::input_required_poll_ms $max_ms
}

proc ::inputCheckHostPolling {} {
    if { $::input_required_poll_ms <= 0 } return
    set hc [::inputHostConfig]
    if { ![dict exists $hc mousepoll] } return
    set mp [dict get $hc mousepoll]

    # 0 means "use each device's own bInterval", which is the best case.
    if { $mp == 0 } return
    if { $mp >= 1 && $mp <= $::input_required_poll_ms } return

    puts stderr "input: WARNING host HID polling does not meet this rig's requirement"
    puts stderr "input:   usbhid.mousepoll = $mp (need 0, or <= $::input_required_poll_ms)"
    if { $mp > 255 || $mp < 0 } {
        puts stderr "input:   that value is out of range — every pointing device is capped"
    } else {
        puts stderr "input:   every mouse is forced to ${mp}ms (max [expr {1000/$mp}] Hz)"
    }
    puts stderr "input:   fix: append usbhid.mousepoll=1 to /boot/firmware/cmdline.txt and reboot"
    puts stderr "input:   (usbhid is built in; modprobe.d will NOT apply)"
}

proc ::inputHostConfig {} {
    set f /sys/module/usbhid/parameters/mousepoll
    if { ![file readable $f] } { return {} }
    if { [catch { open $f r } fh] } { return {} }
    set v [string trim [read $fh]]
    close $fh
    if { ![string is entier -strict $v] } { return {} }
    return [dict create mousepoll $v]
}

proc ::inputStatusJson {} {
    package require yajltcl
    set j [yajl create #auto]
    $j map_open

    set hc [::inputHostConfig]
    $j string host map_open
    if { [dict exists $hc mousepoll] } {
        $j string mousepoll number [dict get $hc mousepoll]
    }
    $j string required_poll_ms number $::input_required_poll_ms
    $j map_close

    $j string classes array_open
    foreach c [inputClasses] {
        $j map_open
        $j string name      string [dict get $c name]
        $j string datapoint string [dict get $c datapoint]
        $j map_close
    }
    $j array_close

    $j string devices array_open
    foreach d [inputList] {
        $j map_open
        foreach {k v} $d {
            if { $k in {events kernel_ts grab grab_ok screen_w screen_h gain connected move_every} } {
                $j string $k number $v
            } else {
                $j string $k string $v
            }
        }
        $j map_close
    }
    $j array_close

    $j string probe array_open
    foreach p [inputProbe] {
        $j map_open
        foreach {k v} $p {
            switch -- $k {
                adopted { $j string $k number $v }
                caps {
                    $j string caps map_open
                    foreach {ck cv} $v { $j string $ck number $cv }
                    $j map_close
                }
                axes {
                    $j string axes map_open
                    foreach {ak av} $v {
                        $j string $ak array_open
                        foreach n $av { $j number $n }
                        $j array_close
                    }
                    $j map_close
                }
                default { $j string $k string $v }
            }
        }
        $j map_close
    }
    $j array_close

    $j string saved array_open
    foreach e $::input_saved_devices {
        $j map_open
        $j string class   string [dict get $e class]
        $j string pattern string [dict get $e pattern]
        # Option names are stored with their leading dash (they are passed
        # straight to inputKnownDevice); strip it for the page's benefit
        # and re-add on the way back.
        $j string opts map_open
        foreach {k v} [dict get $e opts] {
            $j string [string trimleft $k -] string $v
        }
        $j map_close
        $j map_close
    }
    $j array_close

    $j map_close
    set out [$j get]
    $j delete
    return $out
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

# Warn loudly if the host's HID polling does not meet a declared
# requirement. Not fatal: a capped rig still works, it just reports
# slower rates than its hardware can — and the point is that nobody
# should discover that by measuring and drawing the wrong conclusion.
inputCheckHostPolling

puts "input subprocess configured"

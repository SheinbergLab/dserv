# extioconf.tcl -- dedicated interpreter for USB extio box(es) (modules/usbio).
#
# Started from dsconf.tcl:
#     subprocess extio "source [file join $dspath config/extioconf.tcl]"
#
# The box speaks the 128-byte dserv frames over USB-CDC; usbio bridges them into the
# datapoint table from THIS isolated interp -- off the ess/main path, set up once at
# dserv startup (rig hardware, not per-experiment).
#
# It (a) forwards ess/in_obs TO the box so it snaps its clock / drives obs_pin,
# (b) AUTO-DISCOVERS each connected box from its telemetry and forwards that box's
# config/cmd (pin setup, box_schedule_*), (c) injects the box's telemetry/DI/state
# back into the datatable, and (d) hot-swaps the USB connection. No local config needed.
#
# NB: usbio keeps PER-INTERP state, so loading it here (and in essconf) is safe.

proc exit {args} { error "exit not available for this subprocess" }
errormon enable

foreach m { usbio timer } {
    load ${dspath}/modules/dserv_${m}[info sharedlibextension]
}

# The box exposes two CDCs: console (lower-numbered) + data (higher-numbered).
# Override in local/extio.tcl if the highest usbmodem/ttyACM isn't the box.
proc extio_find_data_port {} {
    if { $::tcl_platform(os) == "Darwin" } {
        set ports [lsort -dictionary [glob -nocomplain /dev/cu.usbmodem*]]
    } else {
        set ports [lsort -dictionary [glob -nocomplain /dev/ttyACM*]]
    }
    if {[llength $ports]} { return [lindex $ports end] }
    return ""
}

# ---- forwarding (wired once; survives hot-swap re-opens since usbioSendFrame uses
#      whatever fd is currently open) ----
proc usbio_forward {dp data} { usbioSendFrame $dp [dservTimestamp $dp] $data }

proc extio_forward_box {name} {             ;# a named box's config/cmd (pin setup, box_schedule_*)
    foreach pat [list extio/$name/config/* extio/$name/cmd/*] {
        dservAddMatch $pat
        dpointSetScript $pat usbio_forward
    }
}

# ---- auto-discovery: a box advertises its name in extio/<name>/state/* telemetry.
#      Scan the datatable (polled from extio_service) and wire each new box once.
#      Table-scan, not a match script, so it does not depend on self-notification. ----
array set ::extio_known {}
array set ::extio_wd {}
set ::extio_boxes_last ""
proc extio_discover {} {
    # (a) wire each newly-seen box's config/cmd forwards once (name-agnostic), and
    # (b) track which boxes are LIVE right now and publish the set. A vanished box's
    # last datapoints linger (dserv doesn't delete them), so key-existence can't tell
    # present from stale -- but its state/watchdog stops advancing, so we use that.
    set live {}
    foreach k [dservKeys] {
        if { [regexp {^extio/([^/]+)/state/} $k -> name] && ![info exists ::extio_known($name)] } {
            set ::extio_known($name) 1
            extio_forward_box $name
            puts "extio: discovered box '$name' -- forwarding config/cmd"
        }
        if { [regexp {^extio/([^/]+)/state/watchdog$} $k -> name] } {
            set wd [dservGet $k]
            if { ![info exists ::extio_wd($name)] || $wd ne $::extio_wd($name) } { dict set live $name 1 }
            set ::extio_wd($name) $wd
        }
    }
    # publish only on change (no per-tick churn). Consumers: button_bind {* pin} globs
    # (bind-by-glob, so this is just awareness), workbench UI, logging.
    #   extio/boxes    = list of currently-live box device names
    #   extio/primary  = the first of them (the "the box" for a single-box rig)
    set boxes [lsort [dict keys $live]]
    if { $boxes ne $::extio_boxes_last } {
        set ::extio_boxes_last $boxes
        dservSet extio/boxes   $boxes
        dservSet extio/primary [lindex $boxes 0]
        puts "extio: live boxes = {$boxes}  primary = [lindex $boxes 0]"
    }
}

proc extio_wire_common {} {                 ;# device-independent: sync + obs_pin
    dservAddMatch ess/in_obs
    dpointSetScript ess/in_obs usbio_forward
}

# ---- hot-swap + discovery: runs every 2 s. (Re)open when the box's data port
#      (re)appears, close when it vanishes, or when the reader thread has died while the
#      port stayed put; then pick up any newly-seen box. ----
set ::extio_port ""
proc extio_service {} {
    set want [extio_find_data_port]
    if { $::extio_port ne "" && ![file exists $::extio_port] } {
        catch { usbioClose }
        puts "extio: USB box disconnected ($::extio_port)"
        set ::extio_port ""
    }
    # A host sleep/wake can kill the reader thread with a transient POLLHUP while the
    # write fd stays valid -- so the port file never vanishes and the check above misses
    # it (obs_pin keeps toggling, but nothing is read back). Detect the dead reader and
    # reopen the same port. usbioOpen stops+joins any prior worker first, so this is safe.
    set reader_dead [expr { $::extio_port ne "" && ![usbioAlive] }]
    if { $want ne "" && ($want ne $::extio_port || $reader_dead) } {
        if { $reader_dead } {
            puts "extio: reader stopped on $::extio_port -- reopening (wake-from-sleep recovery)"
        }
        if { [catch { usbioOpen $want } err] } {
            puts stderr "extio: open $want failed: $err"
            set ::extio_port ""
        } else {
            set ::extio_port $want
            puts "extio: USB box connected on $want"
        }
    }
    extio_discover
}
proc extio_timer_cb {dpoint data} { extio_service }

proc init {} {
    extio_wire_common                       ;# forward ess/in_obs (persists across re-opens)
    extio_service                           ;# open now if plugged in + discover
    timerPrefix extioTimer                  ;# then poll (hot-swap + discovery) every 2 s
    dservAddExactMatch extioTimer/0
    dpointSetScript extioTimer/0 extio_timer_cb
    timerTickInterval 2000 2000
}

init

# optional rig-specific overrides (port pinning, extra forwards). Not needed for a
# single auto-discovered box.
if { [file exists $dspath/local/extio.tcl] } {
    source $dspath/local/extio.tcl
}

puts "extio initialized"

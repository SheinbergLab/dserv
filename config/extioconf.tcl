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
proc extio_discover {} {
    foreach k [dservKeys] {
        if { [regexp {^extio/([^/]+)/state/} $k -> name] && ![info exists ::extio_known($name)] } {
            set ::extio_known($name) 1
            extio_forward_box $name
            puts "extio: discovered box '$name' -- forwarding config/cmd"
        }
    }
}

proc extio_wire_common {} {                 ;# device-independent: sync + obs_pin
    dservAddMatch ess/in_obs
    dpointSetScript ess/in_obs usbio_forward
}

# ---- hot-swap + discovery: runs every 2 s. (Re)open when the box's data port
#      (re)appears, close when it vanishes; then pick up any newly-seen box. ----
set ::extio_port ""
proc extio_service {} {
    set want [extio_find_data_port]
    if { $::extio_port ne "" && ![file exists $::extio_port] } {
        catch { usbioClose }
        puts "extio: USB box disconnected ($::extio_port)"
        set ::extio_port ""
    }
    if { $want ne "" && $want ne $::extio_port } {
        if { [catch { usbioOpen $want } err] } {
            puts stderr "extio: open $want failed: $err"
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

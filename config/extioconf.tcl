# extioconf.tcl -- dedicated interpreter for USB extio box(es) (modules/usbio).
#
# Started from dsconf.tcl:
#     subprocess extio "source [file join $dspath config/extioconf.tcl]"
#
# The box speaks the 128-byte dserv frames over USB-CDC; the usbio module bridges
# them into the datapoint table from THIS isolated interp -- off the ess/main path,
# and set up ONCE at dserv startup (it is rig hardware, not per-experiment, so it
# does NOT belong in the ess post-pins that re-run on every system load).
#
# Forwarding: this interp registers matches (dservAddMatch) so the dataserver
# notifies it when ess/in_obs / config / cmd change (from the ess subprocess's
# begin_obs, etc.), and forwards them to the box -- which is what drives obs_pin
# and the clock sync. With autoreg firmware (USB_AUTOREG=1) the box self-declares
# those forwards and usbio auto-wires them here; otherwise wire them in local/extio.tcl.
#
# Hot-swap: a 2 s timer (re)opens the box's data port when it (re)appears and
# closes it when it vanishes -- so unplug/replug "just works" (usbioOpen/Close are
# crash-proof; on re-open the autoreg box re-declares its forwards and they re-wire).

proc exit {args} { error "exit not available for this subprocess" }
errormon enable

# modules: usbio (the box bridge) + timer (periodic hot-swap servicing)
foreach m {usbio timer} {
    load ${dspath}/modules/dserv_${m}[info sharedlibextension]
}

# The box exposes two CDCs: console (lower-numbered) + data (higher-numbered).
# We open the DATA port. Override extio_find_data_port in local/extio.tcl if you
# have other usbmodem/ttyACM devices and the highest one isn't the box.
proc extio_find_data_port {} {
    if { $::tcl_platform(os) == "Darwin" } {
        set ports [lsort -dictionary [glob -nocomplain /dev/cu.usbmodem*]]
    } else {
        set ports [lsort -dictionary [glob -nocomplain /dev/ttyACM*]]
    }
    if {[llength $ports]} { return [lindex $ports end] }
    return ""
}

set ::extio_port ""

# open when the box (re)appears; close when our port vanishes (unplugged).
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
}

proc extio_timer_cb {dpoint data} { extio_service }

proc init {} {
    extio_service                       ;# open now if the box is already plugged in
    timerPrefix extioTimer              ;# then poll for hot-swap every 2 s
    dservAddExactMatch extioTimer/0
    dpointSetScript extioTimer/0 extio_timer_cb
    timerTickInterval 2000 2000
}

init

# rig-specific overrides: explicit port, non-autoreg forwards, extra boxes, etc.
#   e.g. proc extio_find_data_port {} { return /dev/cu.usbmodem1103 }   ;# pin the port
#   e.g. proc usbio_forward {dp data} { usbioSendFrame $dp [dservTimestamp $dp] $data }
#        foreach pat {ess/in_obs extio/macbook/config/* extio/macbook/cmd/*} {
#            dservAddMatch $pat ; dpointSetScript $pat usbio_forward }   ;# non-autoreg box
if { [file exists $dspath/local/extio.tcl] } {
    source $dspath/local/extio.tcl
}

puts "extio initialized"

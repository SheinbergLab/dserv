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
array set ::extio_known {}   ;# name -> 1     (config/cmd forwards currently wired)
array set ::extio_wd    {}   ;# name -> last-seen state/watchdog value
array set ::extio_stale {}   ;# name -> consecutive ticks its watchdog did NOT advance
set ::extio_boxes_last ""

# Tear down what extio_forward_box wired (inverse). catch: harmless if already gone.
proc extio_unforward_box {name} {
    foreach pat [list extio/$name/config/* extio/$name/cmd/*] {
        catch { dpointSetScript $pat {} }
        catch { dservRemoveMatch $pat }
    }
}

proc extio_discover {} {
    # (a) wire each newly-seen box's config/cmd forwards once (name-agnostic); (b) track
    # liveness by state/watchdog freshness (a vanished box's datapoints LINGER in the
    # table -- dserv doesn't delete them -- but its watchdog stops advancing); (c) PRUNE
    # a box that's gone: tear down its forwards + forget it (so it re-wires cleanly if it
    # comes back); (d) publish the live set. Works for USB and Ethernet boxes alike.
    foreach k [dservKeys] {
        if { [regexp {^extio/([^/]+)/state/} $k -> name] && ![info exists ::extio_known($name)] } {
            set ::extio_known($name) 1
            set ::extio_stale($name) 0                        ;# assume live until proven stale
            extio_forward_box $name
            puts "extio: discovered box '$name' -- forwarding config/cmd"
        }
        if { [regexp {^extio/([^/]+)/state/watchdog$} $k -> name] } {
            set wd [dservGet $k]
            if { ![info exists ::extio_wd($name)] || $wd ne $::extio_wd($name) } {
                set ::extio_stale($name) 0                    ;# advancing -> live
            } elseif { [info exists ::extio_stale($name)] } {
                incr ::extio_stale($name)                     ;# frozen this tick
            }
            set ::extio_wd($name) $wd
        }
    }
    # classify + prune. Grace of 1 tick (stale<=1 still live) so a single missed heartbeat
    # window doesn't churn; pruned at stale>=2 (~2 ticks / ~4 s of a frozen watchdog).
    set live {}
    foreach name [array names ::extio_known] {
        set s [expr {[info exists ::extio_stale($name)] ? $::extio_stale($name) : 99}]
        if { $s <= 1 } {
            dict set live $name 1
        } else {
            extio_unforward_box $name
            unset -nocomplain ::extio_known($name) ::extio_wd($name) ::extio_stale($name)
            puts "extio: box '$name' vanished (watchdog stale) -- pruned forwards"
        }
    }
    # publish the live set on change (no per-tick churn). Consumers: button_bind {* pin}
    # globs (awareness, since the bind self-follows), workbench UI, logging.
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

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
array set ::extio_wd    {}   ;# name -> last-seen state/watchdog value (KEPT; drives advance-detection)
array set ::extio_stale {}   ;# name -> consecutive ticks a KNOWN box's watchdog did NOT advance
array set ::extio_known {}   ;# name -> 1  (config/cmd forwards wired = a currently-live box)
set ::extio_boxes_last ""

# Tear down what extio_forward_box wired (inverse). catch: harmless if already gone.
proc extio_unforward_box {name} {
    foreach pat [list extio/$name/config/* extio/$name/cmd/*] {
        catch { dpointSetScript $pat {} }
        catch { dservRemoveMatch $pat }
    }
}

# ---- MANUAL purge (deliberately NOT automatic) ----
# A vanished box's forwards are auto-dropped, but its state datapoints LINGER (dserv doesn't
# auto-delete them). These clear them on demand so a ghost box fully disappears from the table:
#     dservctl extio "extio_clear <name>"   -- purge one box (forwards + its state/* + tracking)
#     dservctl extio extio_clear_dead       -- purge every box NOT currently live (no forwards)
proc extio_clear {name} {
    catch { extio_unforward_box $name }
    set n 0
    foreach k [dservKeys] {
        if { [string match extio/$name/state/* $k] ||
             [string match extio/$name/decoded/* $k] } { catch { dservClear $k }; incr n }
    }
    foreach k [array names ::extio_gmap $name/*] { unset -nocomplain ::extio_gmap($k) }
    unset -nocomplain ::extio_known($name) ::extio_wd($name) ::extio_stale($name)
    puts "extio: cleared box '$name' ($n datapoints)"
    return "cleared $name ($n datapoints)"
}
proc extio_clear_dead {} {
    set dead {}
    foreach k [dservKeys] {
        if { [regexp {^extio/([^/]+)/state/} $k -> name] && ![info exists ::extio_known($name)] } {
            lappend dead $name
        }
    }
    set dead [lsort -unique $dead]
    foreach name $dead { extio_clear $name }
    return "cleared [llength $dead] dead box(es): {$dead}"
}

proc extio_discover {} {
    # Presence is judged by state/watchdog FRESHNESS, never by key-existence: a vanished box's
    # datapoints LINGER in the table (dserv doesn't delete them), so discovering off existence
    # would rediscover -> re-prune -> rediscover it forever. So: wire a box's config/cmd forwards
    # only when its watchdog actually ADVANCES (proof of life); unforward when a known box's
    # watchdog has been frozen past a short grace. ::extio_wd is KEPT for a pruned box, so a
    # still-frozen box is never re-forwarded -- only a genuine watchdog advance brings it back
    # (it reconnected / rebooted). Works for USB and Ethernet boxes alike.
    foreach k [dservKeys] {
        if { ![regexp {^extio/([^/]+)/state/watchdog$} $k -> name] } continue
        set wd [dservGet $k]
        if { [info exists ::extio_wd($name)] && $wd ne $::extio_wd($name) } {
            set ::extio_stale($name) 0                          ;# advancing -> alive
            if { ![info exists ::extio_known($name)] } {
                set ::extio_known($name) 1
                extio_forward_box $name
                puts "extio: box '$name' present -- forwarding config/cmd"
            }
        } elseif { [info exists ::extio_known($name)] } {
            incr ::extio_stale($name)                           ;# a live box's watchdog froze this tick
        }
        set ::extio_wd($name) $wd
    }
    # drop forwards for a known box frozen past grace (>2 ticks ~= 6 s). KEEP ::extio_wd so a
    # still-frozen box is not re-forwarded next tick (that's what caused the churn).
    foreach name [array names ::extio_known] {
        if { [info exists ::extio_stale($name)] && $::extio_stale($name) > 2 } {
            extio_unforward_box $name
            unset -nocomplain ::extio_known($name) ::extio_stale($name)
            puts "extio: box '$name' vanished (watchdog stale) -- pruned forwards"
        }
    }
    # publish the live set (= boxes with forwards wired) on change. Consumers: button_bind
    # {* pin} globs (awareness; the bind self-follows), workbench UI, logging.
    #   extio/boxes    = list of currently-live box device names
    #   extio/primary  = the first of them (the "the box" for a single-box rig)
    set boxes [lsort [array names ::extio_known]]
    if { $boxes ne $::extio_boxes_last } {
        set ::extio_boxes_last $boxes
        dservSet extio/boxes   $boxes
        dservSet extio/primary [lindex $boxes 0]
        puts "extio: live boxes = {$boxes}  primary = [lindex $boxes 0]"
    }
}

# ---- group decode (label algebra): a box announces group membership
# (state/group/<name>/pins, ascending = bit order) and per-pin roles
# (state/label/<n>); each group EVENT (state/group/<name>, int bitmask) then
# decodes to the active pins' labels joined with '_' -- "center" for 0,
# "up_right" for a NE chord -- published to extio/<box>/decoded/<name>.
# Dashboard/log sugar only: response semantics consume the raw bitmask (ess
# joystick API), so this never sits in the response path. Device-agnostic:
# a DIP bank or foot-switch group decodes with the same label algebra. ----
array set ::extio_gmap {}   ;# "<box>/<gname>" -> bit->label list (manifest cache)

proc extio_group_decode {dp data} {
    # events are exactly extio/<box>/state/group/<name>; anything deeper is
    # manifest (pins/settle_ms) -> refresh the cached map instead of decoding
    if { ![regexp {^extio/([^/]+)/state/group/([^/]+)$} $dp -> box gname] } {
        if { [regexp {^extio/([^/]+)/state/group/([^/]+)/} $dp -> box gname] } {
            unset -nocomplain ::extio_gmap($box/$gname)
        }
        return
    }
    set key $box/$gname
    if { ![info exists ::extio_gmap($key)] } {
        set pinsdp extio/$box/state/group/$gname/pins
        if { ![dservExists $pinsdp] } return    ;# manifest not seen yet; next event retries
        set map {}
        foreach p [split [dservGet $pinsdp] ,] {
            set ldp extio/$box/state/label/$p
            if { [dservExists $ldp] && [dservGet $ldp] ne "" } {
                lappend map [dservGet $ldp]
            } else {
                lappend map p$p                 ;# unlabeled member -> pin number stands in
            }
        }
        set ::extio_gmap($key) $map
    }
    set on {}
    set i 0
    foreach l $::extio_gmap($key) {
        if { ($data >> $i) & 1 } { lappend on $l }
        incr i
    }
    dservSet extio/$box/decoded/$gname [expr {[llength $on] ? [join $on _] : "center"}]
}

proc extio_label_invalidate {dp data} {      ;# a relabel stales every map for that box
    if { [regexp {^extio/([^/]+)/state/label/} $dp -> box] } {
        foreach k [array names ::extio_gmap $box/*] { unset -nocomplain ::extio_gmap($k) }
    }
}

proc extio_wire_common {} {                 ;# device-independent: sync + obs_pin
    dservAddMatch ess/in_obs
    dpointSetScript ess/in_obs usbio_forward
    dservAddMatch extio/*/state/group/*     ;# chord-group events + manifest -> decode
    dpointSetScript extio/*/state/group/* extio_group_decode
    dservAddMatch extio/*/state/label/*     ;# relabels invalidate cached maps
    dpointSetScript extio/*/state/label/* extio_label_invalidate
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

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

# LAN discovery (dserv_extiodisc): listen for the UDP :5011 beacons extio boxes
# broadcast, and publish the live set as `extio/discovered`. This is how a box
# that has an address but NO dserv target becomes visible at all -- it never
# registers, so nothing else here knows it exists.
#
# OPTIONAL ON PURPOSE. A rig runs experiments whether or not it can discover new
# hardware, so a missing or stale module must not take the extio subprocess down
# with it -- and it would: this file's failure would abort everything below,
# including the juicer and pin routing. The specific way that bites is a
# half-install (config newer than modules/), which is exactly what happens when
# someone copies a config file to a rig by hand.
#
# But the breadcrumb is not optional. A silently absent listener looks identical
# to a LAN with no boxes on it, so the failure is recorded where it can be read.
# ---- adoption: pointing a discovered box at a dserv -------------------------
#
# The sequence a BOX normally performs on itself at registration
# (box_uplink.c reg_thread_fn), performed by the host on its behalf -- which is
# the whole trick, since a box with no reachable dserv never registers and so
# nobody can be waiting for it to ask.
#
# THE ONE CONNECT-BACK SLOT IS THE SAFETY MODEL, and it is firmware, not policy.
# box_net_eth_server_service() accepts only while srv_conn < 0, so a box that is
# talking to someone cannot be reached by anyone else -- adoption is possible
# exactly when the box is stranded or free. The consequence is that a HEALTHY
# box can only be handed over by its current owner. That is why extio_release
# exists: it is the owner's half of a handshake, not a convenience.
#
# %reg/%match are commands on dserv's TCP command port with no Tcl binding
# (dservSendClients only LISTS), so we speak to a dserv over a socket. Core Tcl
# has TCP -- only UDP is missing -- and these are one-shot operator actions, not
# a hot path, so blocking briefly is fine.

# READ THE REPLY TO EACH LINE BEFORE SENDING THE NEXT, and do not close until
# they have all been answered. This is not politeness, it is the difference
# between an adoption that works and one that half-works.
#
# The original wrote every line into the socket and closed immediately. dserv
# answers each '%' command with "<rc> ...\n" and the close raced its reader:
# of the TWO %match lines this proc is called with, only the FIRST took effect.
# Measured on the rig 2026-08-14, straight after an adopt:
#
#     %getmatch <box> 5010  ->  1 { extio/box/config/* }
#
# with extio/box/cmd/* simply absent. The failure is silent and lopsided --
# config/* writes land so the box adopts, retargets and looks healthy, while
# every cmd/* leaf (save, announce, reboot) vanishes for the ~30 s until the
# box's own MATCH_REFRESH re-asserts its patterns through the firmware path
# that already spaces and retries them. A Save clicked inside that window is
# dropped, and (before the receipt below) reported as success.
#
# The firmware learned this exact lesson first -- box_net_eth_send_command()
# waits for dserv's reply for precisely this reason, with a comment saying so.
# This is the host side of the same rule.
#
# Bounded without an event loop: dserv's Tcl has no vwait/after-script (see
# dservAfter), so the read is a non-blocking poll against a deadline with a
# plain `after ms` sleep, which does work uninstalled. Returns the list of
# reply lines; a caller that cares can check them, and extio_adopt does.
proc extio_dserv_cmd { lines {host 127.0.0.1} {port 4620} {timeout_ms 600} } {
    set s [socket $host $port]
    fconfigure $s -translation binary -blocking 0
    set replies {}
    try {
        foreach l $lines {
            puts -nonewline $s "$l\n"
            flush $s
            lappend replies [extio_dserv__reply $s $timeout_ms]
        }
    } finally {
        close $s
    }
    return $replies
}

# Every reply must be dserv's "1 ..." acceptance. Raises naming the command
# that was refused, because the alternative -- returning a box that is half
# routed -- is the failure mode this whole file keeps re-learning.
proc extio_dserv__require { replies what } {
    set i 0
    foreach r $replies {
        if { ![string match "1*" $r] } {
            error "extio: dserv refused '$what' (line [incr i]): [expr {$r eq "" ? "no reply" : $r}]"
        }
        incr i
    }
    return 1
}

# One "<rc> ...\n" reply, or "" if the deadline passes with nothing complete.
proc extio_dserv__reply { s timeout_ms } {
    set deadline [expr {[clock milliseconds] + $timeout_ms}]
    set acc ""
    while { [clock milliseconds] < $deadline } {
        append acc [read $s]
        set nl [string first "\n" $acc]
        if { $nl >= 0 } { return [string trim [string range $acc 0 $nl]] }
        if { [eof $s] } break
        after 5
    }
    return [string trim $acc]
}

# Adopt <name> at <boxip>, pointing it at <via> -- the address that reaches US
# FROM IT (extiodisc's `via`), never a hostname and never a configured constant.
# dserv is multi-homed and a name resolves to whichever interface it likes; on
# the wrong one the box ends up configured with an address it cannot route to
# while looking perfectly healthy.
#
# DOES NOT SAVE. Adoption is the reversible act -- an unsaved adopt returns the
# box to its previous owner on its next reboot, which is what you want for "my
# rig is down, lend me this for an hour". Making it permanent is a deliberate
# separate step on the box's config page.
#
# DEMOTES obs/mode TO MIRROR, which is the non-obvious part. obs leader is a
# claim about a particular rig's wiring and clock discipline -- pin N is wired
# to THIS rig's obs line, and THIS host schedules at_abs on it. Carried across
# an adoption it is a static IP on a new subnet. Worse than merely stale:
# main.c suppresses box_gpio_obs_mirror while leader is set, because a
# leader-owned line must move only at scheduled instants. So a box that keeps
# leader on a host which never schedules at_abs neither leads NOR mirrors -- it
# goes silent, and looks configured while doing so. Mirror always works: it
# follows ess/in_obs. The new owner re-declares leader if its wiring supports it.
proc extio_adopt { boxip name via {port 4620} {demote_obs 1} } {
    # 1. make our dserv open a connect-back. The box gives %reg its own
    #    connection and settles before sending matches; copy that.
    extio_dserv__require [extio_dserv_cmd [list "%reg $boxip 5010 1"]] \
        "%reg $boxip 5010"
    after 200

    # 2. route the leaves the box actually consumes -- the same two patterns it
    #    asks for itself. Anything else never reaches its config handler.
    #
    #    BOTH are checked. Losing the second one silently is the bug this
    #    check exists to make impossible: config/* alone leaves the box
    #    adoptable, retargetable and completely deaf to cmd/*, which reads as
    #    a healthy adoption right up until the first Save disappears.
    extio_dserv__require [extio_dserv_cmd [list \
        "%match $boxip 5010 extio/$name/config/* 1" \
        "%match $boxip 5010 extio/$name/cmd/* 1"]] \
        "%match $name config/* + cmd/*"
    after 200

    if { $demote_obs } { dservSet extio/$name/config/obs/mode mirror }

    # 3. the retarget itself. Needs box fw >= 0.4.0+51 to take effect on a
    #    CONNECTED box: before that, config/dserv/ip had no handler and a live
    #    uplink kept publishing to its old host while every field reported the
    #    new one.
    dservSet extio/$name/config/dserv/ip   $via
    dservSet extio/$name/config/dserv/port $port

    # These are LIVE, UNSAVED edits exactly like an extio_cfg_set -- a power
    # cycle un-adopts the box -- but they were invisible to the dirty ledger
    # (raw dservSet, and extio_cfg_set's has-announced guard can't pass at
    # adopt time: the box hasn't published to US yet). Record them by hand, so
    # extio-config.html shows the unsaved banner and an ENABLED Save the moment
    # the adopted box is opened -- "adopt, open, Save" becomes the whole flow.
    lappend ::extio_cfg_dirty($name) dserv/ip dserv/port
    if { $demote_obs } { lappend ::extio_cfg_dirty($name) obs/mode }
    set ::extio_cfg_dirty($name) [lsort -unique $::extio_cfg_dirty($name)]

    # 4. ASK FOR THE MANIFEST, and this step is not optional.
    #
    # The box announces on a fresh CONNECT-BACK (srv_fresh -> BOX_NET_RESET),
    # and step 1 above is what triggers that -- while the box is still pointed
    # at its OLD host. So the whole manifest goes to the previous owner, and by
    # the time the box is publishing to US there is no new connect-back to fire
    # another burst.
    #
    # The symptom is nasty because it is partial: continuously-published state
    # (analog blocks, DI edges) appears at once, so the box looks adopted and
    # healthy, while everything announced ONCE -- pin labels, digital group
    # names and membership, obs role -- is simply absent. Observed on the rig:
    # an adopted box with live analog bars and no `response` button group,
    # which only appeared when opening the config page happened to force an
    # announce.
    #
    # The wait is for the retarget to complete: the burst must go out over the
    # NEW uplink, or we reproduce the same bug one step later.
    after 1500
    dservSet extio/$name/cmd/announce 1

    return "adopted $name ($boxip) -> $via:$port[expr {$demote_obs ? {, obs->mirror} : {}}] (not saved)"
}

# Release a box we own. Two intents, and they differ by one line:
#
#   extio_release <ip> <name>        hand-off / let go: free the connect-back
#                                    slot so another dserv can take the box.
#   extio_release <ip> <name> 1      release to the pool: also clear the target,
#                                    so the box reads `unclaimed` to everyone.
#
# ORDER MATTERS AND IS NOT INTERCHANGEABLE. %unreg closes the only channel we
# can send config on, so clearing the target must come FIRST. Doing it the other
# way leaves the box pointed at us with no way for us to correct it.
#
# %unreg closes dserv->box (the command channel) only -- NOT the box's outbound
# publish socket. Since +51 the box moves that itself when the target changes;
# on older firmware only a restart of this host would move it.
proc extio_release { boxip name {to_pool 0} } {
    if { $to_pool } {
        dservSet extio/$name/config/dserv/ip 0.0.0.0
        after 300
    }
    extio_dserv_cmd [list "%unreg $boxip 5010"]
    return "released $name ($boxip)[expr {$to_pool ? { -- now unclaimed} : {}}]"
}

if { [catch { load ${dspath}/modules/dserv_extiodisc[info sharedlibextension] } msg] } {
    dservSet extio/discover/error "module not loaded: $msg"
    puts stderr "extio: LAN discovery unavailable -- $msg"
} elseif { [catch { extioDiscoverStart } msg] } {
    # Port taken is the common one (another dserv, or extio-setup on this host).
    # SO_REUSEPORT means sharing normally works, so this is worth seeing.
    dservSet extio/discover/error "listener not started: $msg"
    puts stderr "extio: LAN discovery not listening -- $msg"
} else {
    dservSet extio/discover/error ""
}

# persisted rig settings (obs_autobind); same store essconf/dfconf use
tcl::tm::add $dspath/lib
package require settingsdb
settingsdb::init [file join $dspath db settings.db]

# The box exposes two CDCs: console + data. Prefer selecting the DATA CDC by the
# box's stable USB IDENTITY (descriptors: manufacturer "dserv", product "extio
# USB box", per-chip serial; data CDC = interface if02) so we can NEVER grab a
# co-resident CDC device -- a juicer pump, eye tracker, Arduino -- that happens
# to enumerate as a higher /dev/ttyACM*. On Linux that's /dev/serial/by-id/,
# which udev builds from those descriptors. The old "highest ttyACM/usbmodem"
# heuristic remains only as a fallback (older firmware, or if by-id is absent).
# Override in local/extio.tcl if needed.
proc extio_find_data_port {} {
    if { $::tcl_platform(os) eq "Darwin" } {
        # macOS has no /dev/serial/by-id. IDENTITY-FIRST via ioreg (2026-07-17:
        # the BLE handheld's dead data CDC outsorted the receiver's in the old
        # highest-cu.usbmodem heuristic, and extioconf read silence -- names
        # come from USB port TOPOLOGY, so plug order can't fix it). Walk the
        # USB tree: remember each device's Product Name, and when a serial
        # client (IODialinDevice) appears under it, accept only ttys belonging
        # to an "extio USB box" -- the handheld's product is "dserv handheld"
        # by design (no match). Data CDC = the *3 tty (console = *1).
        #
        # Cheap guard before the expensive one: ioreg costs ~40 ms here (10 ms
        # of ioreg plus fork/exec from a process with 20+ threads), and the 2 s
        # hot-swap poll pays it forever even with nothing plugged in -- 3.3% of
        # this interpreter's thread, in a 67 ms block that stalls anything
        # queued behind it. Every path below needs a /dev/cu.usbmodem* to exist
        # (ioreg reports the tty.* twin of exactly those), so an empty glob
        # means there is nothing to identify. Identity-first behaviour is
        # unchanged whenever a device is actually present. Linux is already
        # cheap (a by-id glob, no exec) and needs no equivalent.
        if { ![llength [glob -nocomplain /dev/cu.usbmodem*]] } { return "" }
        if { ![catch { exec ioreg -r -c IOUSBHostDevice -l -w0 } out] } {
            set product ""; set best ""
            foreach line [split $out \n] {
                if { [regexp {"USB Product Name" = "([^"]+)"} $line -> p] } {
                    set product $p
                } elseif { [regexp {"IODialinDevice" = "(/dev/tty\.usbmodem[^"]+)"} $line -> tty] } {
                    # ioreg reports the tty.* name; we open the cu.* twin
                    if { $product eq "extio USB box" && [string match {*3} $tty] } {
                        set best [string map {tty. cu.} $tty]
                    }
                }
            }
            if { $best ne "" } { return $best }
        }
        # NO FALLBACK, deliberately -- removed 2026-07-29 to match Linux below.
        #
        # This used to end with `lindex [lsort -dictionary [glob /dev/cu.usbmodem*]]
        # end` for "pre-identity firmware, or an ioreg hiccup", carrying its own
        # warning that it was identity-blind. That is the SAME heuristic whose Linux
        # twin opened a JUICER PUMP and corrupted a dispense (2026-07-09) -- the
        # comment below has never allowed it there, and macOS simply never got
        # updated.
        #
        # It became materially more dangerous with the MCXN947. That board's console
        # defaults to `console=uart` and its console UART IS the MCU-Link VCOM, which
        # enumerates as /dev/cu.usbmodem* -- so with no real box plugged in, the
        # highest-sorting candidate is THE BOX'S OWN CONSOLE. Opening it gives two
        # readers on one port: the CLI sees stolen replies and a human debugging the
        # box gets "device reports readiness to read but returned no data (multiple
        # access on port?)" with nothing to blame. On the RP2350 boxes the console was
        # a CDC on the box itself, so the same bug needed a second device to exist.
        #
        # Returning "" is the correct answer to "I cannot identify a box": the caller
        # already handles no-port (it is the normal state on a rig with no USB box),
        # whereas a wrong port is indistinguishable from a broken one. If a box's
        # identity is genuinely absent, pin its port by redefining this proc in
        # local/extio.tcl -- same escape hatch Linux has always had.
        return ""
    }
    # Linux: ONLY a device positively identified as an extio box (data CDC =
    # if02), by USB identity -- immune to enumeration order AND to any other CDC
    # device on the bus. There is DELIBERATELY no /dev/ttyACM* fallback: the old
    # "grab the highest ttyACM" heuristic opened a JUICER PUMP on a rig with no
    # USB box (two openers on one serial port -> stolen replies + corrupted
    # dispense -> dserv wedge + runaway juice, 2026-07-09). A box whose identity
    # is somehow absent: pin its port by redefining this proc in local/extio.tcl.
    foreach link [lsort [glob -nocomplain /dev/serial/by-id/*extio*if02*]] {
        if { ![catch { file readlink $link } tgt] } {
            return [file normalize [file join [file dirname $link] $tgt]]
        }
    }
    return ""
}

# ---- forwarding (wired once; survives hot-swap re-opens since usbioSendFrame uses
#      whatever fd is currently open) ----
proc usbio_forward {dp data} {
    # cmd/ota/pull is HOST-side (a network client asks us to pull from the shelf),
    # not a box command -- route it to the trigger instead of forwarding to the box.
    # (extio_forward_box wires cmd/* here per box, which would otherwise shadow the
    # global extio/*/cmd/ota/pull dpointSetScript.)
    if { [string match extio/*/cmd/ota/pull $dp] } { extio_ota_pull_trigger $dp $data; return }
    catch { usbioSendFrame $dp [dservTimestamp $dp] $data }
}
;# catch: a vanished device makes sends error (or short-write); forwards just drop --
;# same semantics as fd-not-open -- until extio_service's supervision reopens/clears.

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
    # EVERYTHING under extio/<name>/, not just state/ and decoded/.
    #
    # Leaving the rest is not harmless bookkeeping: `host/connects` and
    # `host/connect_last` are OUR OWN per-box datapoints, and www/extio.html
    # builds its presence set from keys matching state/ OR host/ -- so a box
    # cleared the old way stayed "present" forever on the strength of a connect
    # counter, and its ghosted card could never be pruned. The clear reported
    # success and the card did not move, which is the worst pairing.
    #
    # config/* and cmd/* go too. They are the host's intent for a box that is
    # being forgotten, and a retained config leaf outliving the box it
    # described is the same stale-record problem one level up.
    foreach k [dservKeys] {
        if { [string match extio/$name/* $k] } { catch { dservClear $k }; incr n }
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
    # The analog manifest tracks the same liveness. Recomputed every tick, not
    # only when the box set changes: a group can be created, relabelled or
    # tombstoned on a box that was already live, and the config-change hooks
    # below can only fire for leaves this host actually sees change.
    extio_ain_publish_manifest
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

# Column names for an analog group's block, in the block's own column order.
#
# The analog twin of the label map extio_group_decode builds for a chord: a
# group's `chans` are its columns in ascending order, and ain/label/<ch> names
# each one. Unlabelled channels fall back to ch<N>, so the result is always the
# right LENGTH and a consumer can index it against the decoded samples without
# checking.
#
# WHY THIS MATTERS BEYOND DISPLAY: it is what makes a logged block
# self-describing. Without it, which column is horizontal eye position lives in
# a rig-local setting (chan_x/chan_y) and never reaches the file -- so an old
# recording needs out-of-band knowledge to interpret. With it, the names travel
# with the manifest.
#
#   extio_ain_labels box01 eye   ->   {eye_x eye_y}
proc extio_ain_labels {box gname} {
    set chansdp extio/$box/state/ain/group/$gname/chans
    if { ![dservExists $chansdp] } { return {} }
    set csv [dservGet $chansdp]
    if { $csv eq "" } { return {} }              ;# tombstoned group
    set out {}
    foreach ch [split $csv ,] {
        set ldp extio/$box/state/ain/label/$ch
        if { [dservExists $ldp] && [dservGet $ldp] ne "" } {
            lappend out [dservGet $ldp]
        } else {
            lappend out "ch$ch"
        }
    }
    return $out
}

# ---- extio/ain/streams: what analog is AVAILABLE, and what its columns are --
#
# One datapoint answering "which analog streams exist right now, and what is
# each column called". The consumer is ::ess::file_open, deciding what to record
# and writing the decode key into the file.
#
# WHY A DATAPOINT rather than a query proc: file_open runs in the ess interp on
# the critical path of opening a recording. Asking extio for this would be a
# blocking `send`, which occupies the MAIN interp's serial queue; reading a
# datapoint costs nothing and cannot wedge the open.
#
# LIVE BOXES ONLY -- ::extio_known, the same watchdog-freshness test
# extio_discover uses, never key existence. A vanished box's ain keys LINGER
# (dserv retains datapoints and never deletes them), so a manifest built from
# what is in the table would offer file_open a stream from a box that is not
# there, and the recording would carry a channel that never updates. A stream
# missing from the manifest is honest; one that is present and silent is not.
#
# It names COLUMNS, not timing. interval_us and each block's t0 travel INSIDE
# the blocks, where they are measured rather than declared -- repeating them
# here would create a second source that can drift from the data it describes.
#
#   dict get [dservGet extio/ain/streams] box01/eye
#     -> {box box01 group eye dpoint extio/box01/state/ain/eye
#         chans {0 1} labels {eye_h eye_v} mode continuous batch 1
#         decimate 1 avg 0 rate_hz 250}
proc extio_ain_manifest {} {
    set m [dict create]
    foreach k [dservKeys] {
        if { ![regexp {^extio/([^/]+)/state/ain/group/([^/]+)/chans$} $k -> box gname] } continue
        if { ![info exists ::extio_known($box)] } continue     ;# not live -> not offered
        set csv [dservGet $k]
        if { $csv eq "" } continue                             ;# tombstoned group
        set chans {}
        foreach c [split $csv ,] {
            set c [string trim $c]
            if { $c ne "" } { lappend chans $c }
        }
        if { ![llength $chans] } continue
        set e [dict create box $box group $gname \
                   dpoint extio/$box/state/ain/$gname \
                   chans $chans labels [extio_ain_labels $box $gname]]
        # announced group config, when the firmware reports it (older images
        # announce nothing here -- the entry is still valid, just less described)
        foreach fld {mode deadband decimate batch avg} {
            set dp extio/$box/state/ain/group/$gname/$fld
            if { [dservExists $dp] } { dict set e $fld [dservGet $dp] }
        }
        foreach {fld dp} [list rate_hz extio/$box/state/ain/rate \
                               clk_ppm extio/$box/state/ain/clk_ppm] {
            if { [dservExists $dp] } { dict set e $fld [dservGet $dp] }
        }
        dict set m $box/$gname $e
    }
    return $m
}

set ::extio_ain_streams_last ""
proc extio_ain_publish_manifest {args} {
    set m [extio_ain_manifest]
    if { $m eq $::extio_ain_streams_last } return              ;# publish on CHANGE only
    set ::extio_ain_streams_last $m
    dservSet extio/ain/streams $m
    puts "extio: analog streams = {[dict keys $m]}"
}

proc extio_label_invalidate {dp data} {      ;# a relabel stales every map for that box
    if { [regexp {^extio/([^/]+)/state/label/} $dp -> box] } {
        foreach k [array names ::extio_gmap $box/*] { unset -nocomplain ::extio_gmap($k) }
    }
}

# ---- STAGE-1 OTA orchestrator (A/B + TBYB + rollback) ----------------------
# Push a firmware image to an (Ethernet) box. Transport = the box PULLS: we stage
# the raw image as a binary datapoint (dserv's `<` binary-get is uncapped +
# un-base64'd, unlike the 128B pub/sub push), then fire cmd/ota/begin. The box
# then: resolves its INACTIVE A/B slot, `<`-gets extio/<box>/ota/image and streams
# it straight into that slot, verifies sha256, and (on success) FLASH_UPDATE-reboots
# into it as a try-before-you-buy trial. The trial self-tests (transport/heartbeat)
# and rom_explicit_buy-commits, or the watchdog reverts to the previous image.
#
#   dservctl extio "extio_ota_push <box> /path/to/image.uf2"   ;# stage a TBYB image!
#
# The staged image MUST be a --tbyb build (its IMAGE_DEF carries the try flag) or
# the trial boot won't be buy-pending and can't roll back. The box reports
# extio/<box>/state/ota/{state=staging|verify|ok|armed|fail, progress, result};
# after "armed" the box reboots into the trial (drops off dserv ~seconds, then
# reconnects as the new -- or reverted -- image). Ethernet-only (box_net_get_binary
# stubs to -1 over USB); USB gets a chunk-push path later.
# Host-side ADVISORY size gate only: the box enforces its REAL slot cap at
# cmd/ota/begin (pico_ota_begin gets the PT's target_size and refuses oversize),
# so this just catches absurd files before streaming. 1MB = the radio boards'
# pt-pico2w.json slots; EVB boxes (512K slots) rely on the box-side check.
# (Radio images crossed 500K at v16 -- the old 512K gate was days from biting.)
set ::extio_ota_slot_max [expr {1024*1024}]
# dserv-agent firmware shelf the shelf-OTA path pulls from. Default is the public
# shelf; point it at a rig-local agent (e.g. http://localhost:8080) to OTA offline.
if { ![info exists ::extio_fw_shelf_url] } { set ::extio_fw_shelf_url "https://dserv.net" }

proc extio_ota_push {box file} {
    if { ![file exists $file] } { error "extio_ota_push: no such file: $file" }
    set size [file size $file]
    if { $size == 0 }                        { error "extio_ota_push: empty file: $file" }
    if { $size > $::extio_ota_slot_max }     { error "extio_ota_push: image $size B exceeds box A/B slot ($::extio_ota_slot_max B)" }

    set sha [sha256 -file $file]             ;# hex over the exact file bytes = what the box computes

    # Bytes are about to change what is IN the slot, so everything we believe
    # about the slot is now false. See extio_ota_evidence_clear.
    extio_ota_evidence_clear $box

    # A Zephyr box that announces state/ota/fetch_ok (fw +56, eth uplink live)
    # PULLS the image itself: stage it as a binary datapoint and fire
    # cmd/ota/fetch -- the box '<'-gets extio/<box>/ota/image straight into its
    # slot, self-paced by TCP backpressure. No chunks, no windows, no resends.
    # This is the RP2350 eth pull (bottom of this proc) arriving on the Zephyr
    # boards; the dp-chunk path below remains for USB carriers and older
    # firmware. The transport check matters because fetch_ok is per-connect
    # truth about the uplink it ANNOUNCED on -- a dual box that reconnected
    # over USB has no socket path even if an old eth announce is retained.
    if { [dservExists extio/$box/state/ota/fetch_ok]
         && [dservGet extio/$box/state/ota/fetch_ok] == 1
         && [dservExists extio/$box/state/transport]
         && [dservGet extio/$box/state/transport] eq "eth" } {
        return [extio_ota_push_fetch $box $file $sha $size]
    }

    # The pre-+56 Zephyr boxes (frdm_* and teensy*) do not implement the `<`-get
    # pull OR the 'D'-frame blast: they take the image as a sequence of
    # cmd/ota/chunk DATAPOINTS whatever the carrier -- over USB the chunks ride
    # usbio_forward exactly as they ride the socket push over eth (extio-zephyr
    # main.c: "a path that already works, unchanged, on both eth and USB"). This
    # check must come BEFORE the transport==usb branch: a Zephyr box in USB mode
    # otherwise gets the RP2350 'D'-frame blast it ignores (observed 2026-08-02,
    # first MCXN947-over-USB OTA -- ack never moved, host_io after 10 s).
    if { [dservExists extio/$box/state/board] &&
         ([string match "frdm_*" [dservGet extio/$box/state/board]] ||
          [string match "teensy*" [dservGet extio/$box/state/board]]) } {
        return [extio_ota_push_dp $box $file $sha $size]
    }

    # A USB box (RP2350 family) has no socket to pull over -> PUSH the image as
    # 'D' frames instead of staging it for a pull. Pure-usb and dual-in-USB-mode
    # both report "usb".
    if { [dservExists extio/$box/state/transport] && [dservGet extio/$box/state/transport] eq "usb" } {
        return [extio_ota_push_usb $box $file $sha $size]
    }

    set fp [open $file rb]                    ;# same bytes, staged raw (byte array -> Tcl_GetByteArrayFromObj)
    set bytes [read $fp]
    close $fp

    dservSetData extio/$box/ota/image 0 0 $bytes      ;# ts 0 = now, datatype 0 = DSERV_BYTE (uncapped)
    dservSet     extio/$box/cmd/ota/begin "$sha $size"

    puts "extio ota\[$box\]: staged $size B (sha $sha) -> extio/$box/ota/image; begin fired"
    return "ota begin $box: $size bytes, sha $sha"
}

# ---- USB OTA delivery: push the image as 'D' frames over usbio. The box
#      (cmd/ota/begin -> ota_usb_service_core1) writes each frame into the inactive
#      A/B slot and, on the last byte, verifies the sha + arms the TBYB trial --
#      same slot/verify/trial machinery as eth. No host-side ack polling: the box
#      stages in ~0.1s, then USB write_all backpressure paces the blast to the
#      box's flash rate, and the box's own sha-verify is the correctness gate. (A
#      polling wait from inside this blocking command can't reliably observe the
#      box's state anyway -- so we just give it a fixed moment to stage.) ----
set ::extio_ota_chunk 117                    ;# 'D' frame payload (128 - 11 header)

# begin (synchronous) + initial blast, then an ACK-DRIVEN tail-resender. The extio
# subprocess CANNOT observe the box's ack from inside a blocking command (usbio
# injects it on another thread; a mid-command dservGet never sees it), so we return
# right after the blast and let a dpointSetScript on state/ota/ack resend the tail
# whenever the box's cursor stalls. Debounced: a resend fires only after the cursor
# has been stuck ~400ms, so we don't resend on every 4KB ack while it's flowing.
proc extio_ota_push_usb {box file sha size} {
    # Vanished-device guard (2026-07-17: a J-Link rescue-reset dropped the box off
    # USB; a push against the dead fd wedged this subprocess -- and with it dserv's
    # whole command port -- for the length of the blast). Every write below is now
    # checked; any failure aborts the push in ~a second instead of grinding on.
    if { ![usbioAlive] } { error "extio_ota_push: usb device not connected (usbioAlive=0)" }

    set fp [open $file rb]; fconfigure $fp -translation binary
    set ::extio_ota_img($box)  [read $fp]; close $fp
    set ::extio_ota_size($box) $size
    extio_ota_cancel ::extio_ota_timer $box

    catch { dservClear extio/$box/state/ota/ack }
    catch { dservClear extio/$box/state/ota/state }
    catch { dservClear extio/$box/state/ota/result }
    dservAddMatch   extio/$box/state/ota/ack           ;# so the resender's script fires on each ack
    dpointSetScript extio/$box/state/ota/ack extio_ota_usb_on_ack

    # begin DIRECT (usbio_forward is event-loop deferred; a dservSet wouldn't reach
    # the box until this command returns -- after the blast). Synchronous = box
    # stages before the 'D' frames.
    if { ![extio_ota_usb_write128 { usbioSendFrame extio/$box/cmd/ota/begin 0 "$sha $size" }] } {
        extio_ota_usb_fail $box "cmd/ota/begin write failed (device gone?)"
        error "extio_ota_push: begin write failed -- aborted (state/ota/result=host_io)"
    }
    exec sleep 2                                       ;# fixed settle (box stages in ~0.1s)
    extio_ota_usb_blast $box 0
    # deadline armed AFTER the (synchronous, backpressure-paced ~30s+) blast: an
    # `after` armed before it would be long-expired when the event loop resumes
    # and could fire ahead of the queued ack events. Skip if the blast aborted.
    if { [info exists ::extio_ota_img($box)] } { extio_ota_usb_deadline $box }
    return "ota usb $box: streaming $size B (sha $sha); ack-driven tail-resend -- watch state/ota for armed"
}

# One checked 128-byte send, old-and-new usbio module compatible: the new module
# throws on a hard write error (device detached), the old one returns a short
# count on any failure. Either way != 128 means the frame did not go. Runs the
# send in the CALLER's frame so $box/$sha/$size resolve.
proc extio_ota_usb_write128 {sendcmd} {
    if { [catch { uplevel 1 $sendcmd } w] } { return 0 }
    return [expr {$w == 128}]
}

proc extio_ota_usb_blast {box from} {                  ;# write every chunk from $from to EOF; backpressure paces us
    if { ![info exists ::extio_ota_img($box)] } return
    set data $::extio_ota_img($box); set size $::extio_ota_size($box); set chunk $::extio_ota_chunk
    set fails 0
    for { set off $from } { $off < $size } { incr off $chunk } {
        set end [expr {min($off + $chunk, $size)}]
        if { [extio_ota_usb_write128 { usbioSendChunk $off [string range $data $off [expr {$end - 1}]] }] } {
            set fails 0
            continue
        }
        # Failed chunk: reader death = device gone (abort NOW); otherwise allow a
        # couple of retries for a transient >400ms stall (box mid-sector-erase),
        # then abort. Bound: worst case ~3 x 400ms parked, never the whole image.
        if { ![usbioAlive] || [incr fails] >= 3 } {
            extio_ota_usb_fail $box "chunk write failed at $off/$size (fails=$fails alive=[usbioAlive])"
            return
        }
        incr off -$chunk                               ;# retry this chunk
    }
}

# Cancel a pending dservAfter recorded in an array element, and forget it.
#
# The element is routinely ABSENT -- on the first ack of a push, on any push
# that never stalls (the resend timer is then never set at all), and on every
# repeat call of extio_ota_usb_cleanup, which runs at least three times per push
# (from dp_on_ack at cursor==size, then from on_state on `ok`, then on `armed`).
# So a missing element is the NORMAL path here, not an error path.
#
# `catch { dservAfterCancel $arr($box) }` gets that wrong, and in THIS file it is
# not merely inelegant: line 18 is `errormon enable`, which traces writes to
# ::errorInfo (src/ErrorMonitor.cpp). Tcl writes errorInfo when an error is
# RAISED -- catch stops it propagating, not from happening -- so every one of
# these guarded-but-failing lines still gets reported, and an OTA produced a
# stream of
#   can't unset "::extio_ota_dead(box3)": no such element in array
# for anyone watching the subprocess. `catch` is not silence here; use
# `info exists` / `unset -nocomplain` for conditions that are expected.
proc extio_ota_cancel {arr box} {
    upvar #0 $arr a
    if { [info exists a($box)] } { catch { dservAfterCancel $a($box) } }
    unset -nocomplain a($box)
}

proc extio_ota_usb_on_ack {dp data} {                  ;# box published a new contiguous cursor
    if { ![regexp {^extio/([^/]+)/state/ota/ack$} $dp -> box] } return
    if { ![info exists ::extio_ota_size($box)] } return
    if { $data >= $::extio_ota_size($box) } { extio_ota_usb_cleanup $box; return }   ;# all delivered
    extio_ota_usb_deadline $box                                                      ;# progress -> re-arm
    extio_ota_cancel ::extio_ota_timer $box                              ;# debounce: resend only
    set ::extio_ota_timer($box) [dservAfter 400 [list extio_ota_usb_blast $box $data]]    ;# when stuck ~400ms
}

# No-ack deadline: catches the SILENT death shape -- writes "succeed" into a
# doomed kernel buffer (device detached with room left), so the blast finishes
# but no ack ever comes and the ack-driven resender never fires. 10 s with no
# cursor progress (acks normally arrive every ~0.4 s) -> host-side abort. The
# box's own stall timeout (OTA_USB_TIMEOUT_US, 10 s) is the mirror-image guard.
# Progress-checked at fire time: a resend blast can outlive the timer, so an
# expired deadline racing queued ack events must re-arm, not kill the push.
# ---- Zephyr/datapoint OTA delivery -----------------------------------------
# The RP2350 has two carriers: eth boxes PULL (`<`-get of extio/<box>/ota/image)
# and USB boxes take pushed 'D' frames. The Zephyr boxes take a third: the image
# arrives as ordinary cmd/ota/chunk DATAPOINTS.
#
# Why not just reuse one of the other two. The 'D' frame is the efficient carrier
# (117 B payload) but it is raw bytes on the box's link, and on an ETHERNET box
# there is no host path that can inject those: dserv owns the box's single
# connect-back socket, and opening a second one displaces it -- killing the
# downlink for exactly the operation that needs it. The pull needs box-side socket
# code the Zephyr port does not have yet. A datapoint costs payload but works
# today over both carriers, unchanged.
#
# Budget: DSERV_MSG_MAX_PAYLOAD is 109 for varname+data, so the usable chunk is
# 109 - strlen("extio/<box>/cmd/ota/chunk") - 8 (seq u32 + crc32 u32). For a box
# named box1 that is 77 B. Computed per box rather than hardcoded -- a longer box
# name silently shrinks it, and a hardcoded 77 would overflow the frame builder
# and drop every chunk.
#
# Chunk layout matches the box's handler: [seq u32 LE][crc32 u32 LE][data].
# Strictly sequential; any reject re-acks the cursor, so resend is idempotent.
proc extio_ota_dp_chunk {box} {
    set n [expr {109 - [string length "extio/$box/cmd/ota/chunk"] - 8}]
    if { $n < 16 } { error "extio_ota_push_dp: box name '$box' leaves only $n B per chunk" }
    return $n
}

# ---- fetch (pull) delivery: stage extio/<box>/ota/image + fire cmd/ota/fetch.
# The box pulls, so there is NO host resender to wire: the box's own recv
# timeouts fail the transfer (it publishes state=fail + fetch_rc<0), and the
# lifecycle's 180 s transfer deadline is the outer net. state/ota/ack and
# progress arrive as plain telemetry. The staged image is freed by
# extio_ota_on_state at ok|armed|fail -- the same hook the RP2350 pull uses.
proc extio_ota_push_fetch {box file sha size} {
    set fp [open $file rb]; fconfigure $fp -translation binary
    set bytes [read $fp]; close $fp
    foreach k {ack state result} { catch { dservClear extio/$box/state/ota/$k } }
    dservSetData extio/$box/ota/image 0 0 $bytes
    dservSet extio/$box/cmd/ota/fetch "$sha $size"
    puts "extio ota\[$box\]: staged $size B (sha $sha) -> extio/$box/ota/image; fetch fired"
    return "ota fetch $box: $size B (sha $sha); box is pulling -- watch state/ota"
}

proc extio_ota_push_dp {box file sha size} {
    set fp [open $file rb]; fconfigure $fp -translation binary
    set ::extio_ota_img($box)  [read $fp]; close $fp
    set ::extio_ota_size($box)   $size
    set ::extio_ota_cursor($box) 0
    set ::extio_ota_head($box)   0
    extio_ota_cancel ::extio_ota_timer $box

    catch { dservClear extio/$box/state/ota/ack }
    catch { dservClear extio/$box/state/ota/state }
    catch { dservClear extio/$box/state/ota/result }
    dservAddMatch   extio/$box/state/ota/ack
    dpointSetScript extio/$box/state/ota/ack extio_ota_dp_on_ack

    dservSet extio/$box/cmd/ota/begin "$sha $size"
    exec sleep 1                                    ;# let the box open the slot
    extio_ota_dp_send $box
    if { [info exists ::extio_ota_img($box)] } { extio_ota_usb_deadline $box }
    return "ota dp $box: streaming $size B (sha $sha) in [extio_ota_dp_chunk $box] B chunks -- watch state/ota"
}

# ACK-CLOCKED WINDOW, not an open-loop drip. Measured 2026-08-10 (box02,
# MCXN947, eth, 285 KB image): the old 32-chunk/40 ms drip took 62.9 s at
# ~10x send redundancy. The box's eth inbound queue is 64 frames deep and
# LOSSY (k_msgq drop-on-overflow, box_net_eth.c inq_drop) with the chunk
# handler -- flash erases included -- draining it from the service loop, so
# TCP backpressure never engages. One overflow rejected everything in flight
# (chunks are strictly sequential), and the old stall-resend then re-blasted
# the ENTIRE remainder into the same queue: 38,609 acks for 3,751 chunks.
# The USB carrier has the same shape with an 8-frame CDC ring (2026-08-02:
# ~42 chunks arrived, rest dropped; the +19..+21 ring hunt). The cure is the
# same for both: never hold more than one queue's worth in flight.
#
#   - send at most [extio_ota_dp_win] chunks beyond the box's last-acked
#     cursor, then STOP;
#   - follow every burst with a PROBE (header-only chunk): the box re-acks
#     its cursor unconditionally on it (main.c datalen<=HDR branch; on
#     firmware without it the probe is either a dup -> same re-ack, or a
#     0-byte in-sequence no-op that the stall clock covers). The probe rides
#     FIFO behind the burst, so its re-ack reports the cursor AFTER the
#     burst drained -- our window clock. Needed because accepted-progress
#     acks only come every 8192 B (~108 chunks), a longer stride than any
#     queue-safe window;
#   - 400 ms with no ack at all means the tail after the cursor (data AND
#     probe) is gone: rewind and send ONE window from the cursor. Dups
#     re-ack instantly, so a lost frame costs one bounded round, never a
#     tail restart.
#
# Rate self-paces to the box's true drain (one window per round trip)
# instead of a tuned constant, so there is no fixed ceiling to fall from
# and no livelock to fall into.
set ::extio_ota_dp_window     48   ;# eth: in-flight budget, < the 64-slot inq
set ::extio_ota_dp_window_usb 32   ;# usb: the +21-validated CDC burst bound

proc extio_ota_dp_win {box} {
    if { [dservExists extio/$box/state/transport]
         && [dservGet extio/$box/state/transport] eq "usb" } {
        return $::extio_ota_dp_window_usb
    }
    return $::extio_ota_dp_window
}

# Header-only chunk = cursor probe (see the block comment above).
proc extio_ota_dp_probe {box} {
    dservSetData extio/$box/cmd/ota/chunk 0 0 [binary format ii 0 0]
}

# Top the window up to cursor+WINDOW, probe behind it, re-arm the stall
# clock. Called from every ack and from the stall path; a full window makes
# it a timer re-arm and nothing else.
proc extio_ota_dp_send {box} {
    if { ![info exists ::extio_ota_img($box)] } return
    set data   $::extio_ota_img($box)
    set size   $::extio_ota_size($box)
    set chunk  [extio_ota_dp_chunk $box]
    set head   $::extio_ota_head($box)
    set target [expr {min($::extio_ota_cursor($box) + [extio_ota_dp_win $box] * $chunk, $size)}]
    for { set off $head } { $off < $target } { incr off $chunk } {
        set end [expr {min($off + $chunk, $size)}]
        set d   [string range $data $off [expr {$end - 1}]]
        set crc [expr {[zlib crc32 $d] & 0xffffffff}]
        dservSetData extio/$box/cmd/ota/chunk 0 0 [binary format ii $off $crc]$d
    }
    if { $target > $head } {
        set ::extio_ota_head($box) $target
        extio_ota_dp_probe $box
    }
    extio_ota_dp_arm_stall $box
}

proc extio_ota_dp_arm_stall {box} {
    extio_ota_cancel ::extio_ota_timer $box
    set ::extio_ota_timer($box) [dservAfter 400 [list extio_ota_dp_stall $box]]
}

# True silence (no ack of any kind for 400 ms): rewind to the cursor and send
# one bounded window again. Every ack -- including no-progress reject re-acks
# -- re-arms the clock via dp_send, so this only fires when nothing at all is
# coming back, never as a second sender racing a live drain.
proc extio_ota_dp_stall {box} {
    if { ![info exists ::extio_ota_img($box)] } return
    set ::extio_ota_head($box) $::extio_ota_cursor($box)
    extio_ota_dp_send $box
}

# The box's cursor is the only truth about what landed: progress slides the
# window forward; a no-progress re-ack still proves the link is draining and
# re-arms the stall clock.
proc extio_ota_dp_on_ack {dp data} {
    if { ![regexp {^extio/([^/]+)/state/ota/ack$} $dp -> box] } return
    if { ![info exists ::extio_ota_size($box)] } return
    if { $data >= $::extio_ota_size($box) } { extio_ota_usb_cleanup $box; return }
    extio_ota_usb_deadline $box
    if { $data > $::extio_ota_cursor($box) } { set ::extio_ota_cursor($box) $data }
    extio_ota_dp_send $box
}

proc extio_ota_usb_deadline {box} {
    extio_ota_cancel ::extio_ota_dead $box
    set at -1
    catch { set at [dservGet extio/$box/state/ota/ack] }
    set ::extio_ota_dead($box) [dservAfter 10000 [list extio_ota_usb_deadcheck $box $at]]
}

proc extio_ota_usb_deadcheck {box armed_ack} {
    if { ![info exists ::extio_ota_img($box)] } return ;# push already delivered/failed
    set now -1
    catch { set now [dservGet extio/$box/state/ota/ack] }
    if { $now != $armed_ack } { extio_ota_usb_deadline $box; return }   ;# progress raced us
    extio_ota_usb_fail $box "no ack progress in 10s (cursor $now/$::extio_ota_size($box))"
}

# Host-side abort: free the image, stop the timers, and MARK the failure -- the
# box owns state/ota/* normally, but a host abort means the box may be
# unreachable, so the host publishes fail/host_io for the fleet page (the box
# republishes real state whenever it returns).
proc extio_ota_usb_fail {box why} {
    extio_ota_usb_cleanup $box
    dservSet extio/$box/state/ota/state  fail
    dservSet extio/$box/state/ota/result host_io
    puts "extio ota\[$box\]: push ABORTED -- $why"
}

# Stop resending (delivered, or state -> armed/fail). IDEMPOTENT ON PURPOSE --
# it runs three times per push, and only the first call finds anything to do.
# See extio_ota_cancel above for why that must not be spelled `catch {unset}`.
proc extio_ota_usb_cleanup {box} {
    extio_ota_cancel ::extio_ota_timer $box
    extio_ota_cancel ::extio_ota_dead  $box
    extio_ota_cancel ::extio_ota_drip  $box
    unset -nocomplain ::extio_ota_img($box) ::extio_ota_size($box) \
        ::extio_ota_cursor($box) ::extio_ota_head($box)
}

# Free a staged image by hand (auto-freed on ok|fail; this is for an aborted run).
proc extio_ota_clear {box} {
    catch { dservClear extio/$box/ota/image }
    return "cleared extio/$box/ota/image"
}

# extio_ota_push_shelf <box> ?channel? -- OTA a box straight from the firmware
# shelf: resolve the channel's latest version, pick the image whose `build`
# matches the box's baked build (with a board-compat guard), pull its sealed
# .bin binary-safe, verify sha256, then hand off to extio_ota_push (stage+begin).
# So the whole release loop is: build.sh <t> --tbyb --push  ->  one call per box.
#   dservctl extio "extio_ota_push_shelf <box>"            ;# latest on dev
#   dservctl extio "extio_ota_push_shelf <box> stable"
# Resolve what a box of build <bbuild> would update to on <channel>, or verify
# a pinned <version>. Returns a dict: version, img (the image entry), latest
# (the channel pointer, for reporting), pinned, manifest.
#
# SHARED between extio_ota_push_shelf and extio_fw_check deliberately. "What
# would this box update to?" and "what does the page show as available?" must
# be the same question answered by the same code -- a display that resolves
# versions its own way is a second implementation that will eventually disagree
# with the button next to it, and the disagreement would surface as a user
# pressing Update and getting a different image than the one they read.
#
# NOT simply `latest`, and that distinction became load-bearing the moment the
# shelf held two families. `latest` is a single per-channel pointer moved by
# whoever published most recently, but a version only contains the images that
# publish built -- so pushing an MCXN947 image makes `dev/latest` a version with
# no RP2350 image in it, and every Pico box asking for latest gets "no OTA
# (.bin) image for build 'dual'". Observed 2026-07-30, immediately, on the first
# MCXN947 publish: one family's release silently broke the other family's
# default update path.
#
# So walk the versions newest-first and take the first that HAS an image for
# this build. That is what "latest" means from a given box's point of view.
#
# AN EXPLICIT version ARGUMENT IS NEVER OVERRIDDEN -- if the caller pinned a
# version and it lacks the image, that is an error worth reporting, not
# something to silently substitute. Pinning exists to say "this exact one".
#
# Relies on the shelf listing `versions` newest-first, which is the order the
# agent writes and the API returns.
proc extio_shelf_pick {channel bbuild {version ""}} {
    package require yajltcl
    set base $::extio_fw_shelf_url
    set url "$base/api/firmware/extio/$channel"
    if { [catch { https_get $url -timeout 15000 } json] } {
        error "extio_shelf_pick: shelf fetch failed ($url): $json"
    }
    set d [::yajl::json2dict $json]

    set img ""; set entry {}; set pinned [expr {$version ne ""}]
    set tried {}
    foreach v [dict get $d versions] {
        if { ![dict exists $v version] } continue
        set vv [dict get $v version]
        if { $pinned && $vv ne $version } continue
        foreach im [dict get $v images] {
            if { ![dict exists $im build] || [dict get $im build] ne $bbuild } continue
            if { ![dict exists $im bin] || [dict get $im bin] eq "" } continue
            set img $im; set version $vv; set entry $v; break
        }
        if { $img ne "" } break
        lappend tried $vv
        if { $pinned } break
    }
    if { $img eq "" } {
        if { $pinned } {
            error "extio_shelf_pick: no OTA (.bin) image for build '$bbuild' in\
                   $channel/$version (version was pinned explicitly)"
        }
        error "extio_shelf_pick: no OTA (.bin) image for build '$bbuild' anywhere in\
               channel '$channel' -- searched [llength $tried] version(s): [join $tried {, }].\
               Has an image for this build ever been published? Check\
               $base/api/firmware/extio/$channel"
    }
    return [dict create version $version img $img entry $entry pinned $pinned manifest $d \
                        latest [expr {[dict exists $d latest] ? [dict get $d latest] : ""}] \
                        published [expr {[dict exists $entry publishedAt] \
                                         ? [dict get $entry publishedAt] : ""}]]
}

# ---- "what is on the shelf for this box?" -----------------------------------
#
# ON DEMAND, NEVER POLLED. The shelf is a public HTTPS endpoint and this runs on
# a rig that must not depend on it: a periodic fetch would put an internet round
# trip on a machine whose job is running experiments, and would fail loudly on
# every offline rig for no benefit. The page asks when a person asks.
#
# Keyed by CHANNEL+BUILD rather than by box, because the answer depends on
# nothing else -- ten boxes of the same build share one result, and one fetch
# serves them all.
#
# WHAT THIS DELIBERATELY DOES NOT DO IS SAY "UP TO DATE". The two version
# strings are from different namespaces and are not comparable:
#
#   box state/fw_ver  0.4.0+91              MCUboot header, from extio-zephyr/VERSION
#   shelf version     0.52.9-3-g08cea56c    git describe, from publish.sh
#
# and the shelf's per-image fields (build/board/variant/binSize/binSha256)
# carry no image version at all. state/fw is worse than useless here -- nothing
# defines BOX_FW_VERSION in the Zephyr builds, so it is the literal string
# "dev" on every box. Comparing any of these would produce a badge that is
# right by accident. Worse than absent: on 2026-08-14 the newest shelf image
# for this build was OLDER than the flashed box, so a naive "update available"
# would have invited a downgrade.
#
# So this reports FACTS -- what is on the shelf, and when we last asked -- and
# leaves the judgement to the person reading it. Making the comparison real
# needs the image version recorded on the shelf at publish time.
proc extio_fw_check {box} {
    if { ![dservExists extio/$box/state/build] } {
        error "extio_fw_check: box '$box' hasn't announced state/build yet (connected?)"
    }
    set bbuild [string map {/ _} [dservGet extio/$box/state/build]]
    set channel "dev"
    if { [dservExists extio/$box/state/channel] } {
        set c [dservGet extio/$box/state/channel]
        if { $c ne "" } { set channel $c }
    }
    set pfx "extio/shelf/$channel/$bbuild"

    if { [catch { extio_shelf_pick $channel $bbuild } pick] } {
        dservSet $pfx/error   $pick
        dservSet $pfx/checked [clock seconds]
        return "shelf check FAILED for $box ($channel/$bbuild): $pick"
    }
    set img [dict get $pick img]
    set pub [dict get $pick published]
    dservClear $pfx/error
    dservSet $pfx/version   [dict get $pick version]
    dservSet $pfx/published $pub
    dservSet $pfx/size      [expr {[dict exists $img binSize] ? [dict get $img binSize] : 0}]
    dservSet $pfx/sha       [expr {[dict exists $img binSha256] ? [dict get $img binSha256] : ""}]
    dservSet $pfx/checked   [clock seconds]

    # LEAD WITH THE PUBLISH DATE, because the version string is a `git describe`
    # of the dserv tree ("0.52.9-3-g08cea56c") and means nothing to anyone who
    # is not the person who built it. A date is the one part of a shelf entry a
    # reader can act on: they know roughly when their box was flashed, so they
    # can tell whether the shelf is ahead of it. The version still shows,
    # because it is what gets passed back to pin an update -- but it is the
    # identifier, not the information.
    set when $pub
    if { $when ne "" && ![catch { clock scan $pub -format {%Y-%m-%dT%H:%M:%SZ} -gmt 1 } t] } {
        set when [clock format $t -format {%Y-%m-%d %H:%M} -gmt 1]
        append when "Z"
    }
    if { $when eq "" } { return [dict get $pick version] }
    return "published $when · [dict get $pick version]"
}

proc extio_ota_push_shelf {box {channel dev} {version ""}} {
    package require yajltcl
    set base $::extio_fw_shelf_url

    # 1. box identity -- the shelf image must match its build; board is the hard filter.
    if { ![dservExists extio/$box/state/build] } {
        error "extio_ota_push_shelf: box '$box' hasn't announced state/build yet (connected?)"
    }
    set bbuild [dservGet extio/$box/state/build]
    set bboard ""
    if { [dservExists extio/$box/state/board] } { set bboard [dservGet extio/$box/state/board] }

    # FLATTEN SEPARATORS BEFORE MATCHING, and do it on the BOX side too.
    #
    # The shelf validates `build` against ^[a-z0-9][a-z0-9_-]*$ (no slash), while
    # every Zephyr board target has them ("frdm_mcxn947/mcxn947/cpu0"). Both the
    # publisher (extio-zephyr/publish.sh) and the firmware (box_announce.c
    # box_build_key()) therefore map / -> _, so new images and new firmware agree.
    #
    # Doing it HERE as well is what makes the transition OTA-able rather than
    # requiring a bench visit. A box still running pre-2026-07-29 firmware
    # announces the SLASHED target, which would match no shelf entry -- so the one
    # update that teaches it the new key could not itself be delivered over the
    # air. Flattening the announced value closes that bootstrap gap. It is a no-op
    # for the RP2350 targets, whose build names are flat words already.
    set bbuild [string map {/ _} $bbuild]

    # 2+3. resolve the version and image. SHARED with extio_fw_check -- see
    # extio_shelf_pick: "what would this box update to" has to be answered by
    # exactly the code that would do the updating, or the page and the button
    # disagree, which is worse than not showing it.
    set pick    [extio_shelf_pick $channel $bbuild $version]
    set version [dict get $pick version]
    set img     [dict get $pick img]
    set pinned  [dict get $pick pinned]
    set d       [dict get $pick manifest]

    if { !$pinned && [dict exists $d latest] && [dict get $d latest] ne $version } {
        puts "extio_ota_push_shelf: channel latest is [dict get $d latest] (no '$bbuild'\
              image); using $version, the newest that has one"
    }
    if { $bboard ne "" && [dict exists $img board] && [dict get $img board] ne "" \
         && [dict get $img board] ne $bboard } {
        error "extio_ota_push_shelf: board mismatch -- box '$bboard' vs shelf '[dict get $img board]', refusing"
    }
    set binfile [dict get $img bin]
    set binsha  [expr {[dict exists $img binSha256] ? [dict get $img binSha256] : ""}]

    # 4. pull the .bin binary-safe (-outfile bypasses Tcl UTF-8 re-encoding).
    set tmp [file join /tmp "extio_ota_${box}_${version}.bin"]
    set burl "$base/firmware/extio/$channel/$version/$binfile"
    if { [catch { https_get $burl -outfile $tmp -timeout 60000 } n] } {
        catch { file delete $tmp }
        error "extio_ota_push_shelf: bin fetch failed ($burl): $n"
    }

    # 5. verify against the manifest sha before we touch the box (extio_ota_push re-hashes too).
    set got [sha256 -file $tmp]
    if { $binsha ne "" && ![string equal -nocase $got $binsha] } {
        catch { file delete $tmp }
        error "extio_ota_push_shelf: sha mismatch -- shelf $binsha, downloaded $got"
    }
    puts "extio ota\[$box\]: pulled $channel/$version $binfile ([file size $tmp] B, sha $got) from shelf"

    # 6. stage + fire via the existing local-file path, then drop the temp (bytes now live in dserv).
    set r [extio_ota_push $box $tmp]
    catch { file delete $tmp }
    return $r
}

# ---- DOCKED handheld OTA (a BLE-transport box plugged in via USB) ------------
# The handheld's transport IS the radio, so multiframe 'D' OTA is excluded over
# it -- BUT its data CDC (CDC1, otherwise unused) carries an OTA when docked
# (firmware BOX_USB_OTA_DOCKED). The split: cmd/ota/begin + state/ota/ack + all
# state flow over the RADIO relay (already up, small frames), and ONLY the 'D'
# data stream goes over the handheld's own USB port. So the ack-driven resend
# machinery is reused verbatim (acks arrive as ordinary dserv datapoints); only
# the chunk WRITER differs -- a Tcl channel to the handheld's port instead of
# usbioSendChunk to the receiver's. One picotool flash bootstraps this; every
# update after is docked-USB, no picotool.
#
#   dservctl extio "extio_ota_push_shelf_docked hh1"             ;# latest on dev
#   dservctl extio "extio_ota_push_shelf_docked hh1 dev <ver>"

# A docked handheld's USB DATA port, by its "dserv handheld" identity -- the
# mirror of extio_find_data_port (which deliberately SKIPS that product so it
# never grabs the handheld). Data CDC = the *3 tty (console = *1).
proc extio_find_handheld_port {} {
    if { $::tcl_platform(os) eq "Darwin" } {
        # same cheap guard as extio_find_data_port (see there for why)
        if { ![llength [glob -nocomplain /dev/cu.usbmodem*]] } { return "" }
        if { ![catch { exec ioreg -r -c IOUSBHostDevice -l -w0 } out] } {
            set product ""; set best ""
            foreach line [split $out \n] {
                if { [regexp {"USB Product Name" = "([^"]+)"} $line -> p] } {
                    set product $p
                } elseif { [regexp {"IODialinDevice" = "(/dev/tty\.usbmodem[^"]+)"} $line -> tty] } {
                    if { $product eq "dserv handheld" && [string match {*3} $tty] } {
                        set best [string map {tty. cu.} $tty]
                    }
                }
            }
            if { $best ne "" } { return $best }
        }
        return ""
    }
    foreach link [lsort [glob -nocomplain /dev/serial/by-id/*handheld*if02*]] {
        if { ![catch { file readlink $link } tgt] } {
            return [file normalize [file join [file dirname $link] $tgt]]
        }
    }
    return ""
}

# Build one 128-byte 'D' OTA frame -- EXACTLY ota_data_frame() in the firmware:
# [0]='D' [1..4]=seq(u32 LE = byte offset) [5..6]=len(u16 LE) [7..10]=crc32(u32
# LE; pico_crc32 == IEEE == Tcl zlib crc32, verified) [11..]=payload, 0-padded.
proc extio_ota_dframe {seq data} {
    set len [string length $data]
    set crc [zlib crc32 $data]
    set f [binary format {a1 i s i} D $seq $len $crc]
    append f $data
    set pad [expr {128 - [string length $f]}]
    if { $pad > 0 } { append f [binary format "x$pad"] }
    return $f
}

# Fetch the shelf image (.bin) matching a box's build/board -> temp file path.
# Self-contained (does not disturb the validated extio_ota_push_shelf path).
proc extio_shelf_fetch_bin {box channel version} {
    package require yajltcl
    set base $::extio_fw_shelf_url
    if { ![dservExists extio/$box/state/build] } {
        error "extio_shelf_fetch_bin: box '$box' hasn't announced state/build (connected?)"
    }
    set bbuild [dservGet extio/$box/state/build]
    set bboard [expr {[dservExists extio/$box/state/board] ? [dservGet extio/$box/state/board] : ""}]

    # Same / -> _ flattening as extio_ota_push_shelf; see the long comment there.
    # This is a SECOND matcher on the same contract, which is exactly why it was
    # missed when the first was fixed: extio_ota_push_shelf worked, this silently
    # did not, and the paths that use it (extio_ota_push_shelf_docked,
    # extio_ota_pull_run) stayed broken for every Zephyr board. Found by running
    # the fetch in isolation, which failed with the slashed name still in the
    # error text -- if these two ever diverge again, that error message is the tell.
    set bbuild [string map {/ _} $bbuild]
    set url "$base/api/firmware/extio/$channel"
    if { [catch { https_get $url -timeout 15000 } json] } { error "shelf fetch failed ($url): $json" }
    set d [::yajl::json2dict $json]
    if { $version eq "" && [dict exists $d latest] } { set version [dict get $d latest] }
    if { $version eq "" } { error "channel '$channel' has no version" }
    set img ""
    foreach v [dict get $d versions] {
        if { ![dict exists $v version] || [dict get $v version] ne $version } continue
        foreach im [dict get $v images] {
            if { ![dict exists $im build] || [dict get $im build] ne $bbuild } continue
            if { ![dict exists $im bin] || [dict get $im bin] eq "" } continue
            set img $im; break
        }
    }
    if { $img eq "" } { error "no OTA (.bin) image for build '$bbuild' in $channel/$version" }
    if { $bboard ne "" && [dict exists $img board] && [dict get $img board] ne "" \
         && [dict get $img board] ne $bboard } {
        error "board mismatch -- box '$bboard' vs shelf '[dict get $img board]', refusing"
    }
    set binfile [dict get $img bin]
    set binsha  [expr {[dict exists $img binSha256] ? [dict get $img binSha256] : ""}]
    set tmp [file join /tmp "extio_ota_${box}_${version}.bin"]
    set burl "$base/firmware/extio/$channel/$version/$binfile"
    if { [catch { https_get $burl -outfile $tmp -timeout 60000 } n] } {
        catch { file delete $tmp }; error "bin fetch failed ($burl): $n"
    }
    set got [sha256 -file $tmp]
    if { $binsha ne "" && ![string equal -nocase $got $binsha] } {
        catch { file delete $tmp }; error "sha mismatch -- shelf $binsha, downloaded $got"
    }
    puts "extio ota\[$box\]: pulled $channel/$version $binfile ([file size $tmp] B, sha $got) from shelf"
    return $tmp
}

proc extio_ota_push_shelf_docked {box {channel dev} {version ""}} {
    set port [extio_find_handheld_port]
    if { $port eq "" } { error "extio_ota_push_shelf_docked: no docked handheld found (dserv handheld USB identity)" }
    set tmp [extio_shelf_fetch_bin $box $channel $version]
    set r [extio_ota_push_docked $box $tmp [sha256 -file $tmp] [file size $tmp] $port]
    catch { file delete $tmp }
    return $r
}

# Stream a local .bin to a docked handheld: begin over the radio relay, then the
# 'D' data stream over the handheld's own USB port. FULLY SYNCHRONOUS -- no event
# timers (the extio interp has no `after`, and usbio injects acks on a thread a
# blocking command can't observe): the blocking writes are paced by USB flow
# control (the box NAKs while it flash-writes -> no host->box drops), and the
# box's own sha256 verify is the correctness gate. A dservWhen reports the final
# result reactively. After this returns the box verifies + arms the TBYB trial.
proc extio_ota_push_docked {box file sha size port} {
    if { [catch { open $port r+ } ch] } { error "extio_ota_push_docked: cannot open $port: $ch" }
    # byte-transparent I/O: Tcl 9 removed "-encoding binary"; iso8859-1 maps 0..255
    # 1:1, and -translation binary suppresses EOL/EOF munging.
    fconfigure $ch -translation binary -encoding iso8859-1 -blocking 1 -buffering none
    set fp [open $file rb]; set data [read $fp]; close $fp
    foreach k {ack state result} { catch { dservClear extio/$box/state/ota/$k } }
    extio_ota_evidence_clear $box            ;# the slot is about to change; see there

    # Observe the outcome reactively (dservWhen: non-blocking, no `after`, fires
    # once when state/ota/result reaches a terminal value).
    catch { dservWhenCancel $::extio_ota_when($box) }
    set ::extio_ota_when($box) [dservWhen extio/$box/state/ota/result \
        {in {committed fail buy_failed host_io bad_args}} extio_ota_docked_done]

    # begin SYNCHRONOUSLY over the radio relay: usbioSendFrame -> receiver ->
    # (CFG_NONE) relay -> handheld. A dservSet would defer to the event loop
    # (i.e. until after this blocking command returns).
    usbioSendFrame extio/$box/cmd/ota/begin 0 "$sha $size"
    exec sleep 2                                       ;# box stages the inactive slot (~0.1s) + radio hop

    set chunk $::extio_ota_chunk
    for { set off 0 } { $off < $size } { incr off $chunk } {
        set end [expr {min($off + $chunk, $size)}]
        puts -nonewline $ch [extio_ota_dframe $off [string range $data $off [expr {$end - 1}]]]
    }
    flush $ch
    exec sleep 1                                       ;# drain the tail before dropping DTR
    close $ch
    return "ota docked $box: streamed $size B (sha $sha) over $port -- box verifying + arming; watch state/ota"
}

proc extio_ota_docked_done {dp value} {                ;# dservWhen callback: log the terminal result
    if { ![regexp {^extio/([^/]+)/state/ota/result$} $dp -> box] } return
    unset -nocomplain ::extio_ota_when($box)
    puts "extio ota\[$box\]: docked OTA result = $value"
}

# Datapoint trigger for a network client (extio-setup's dserv driver, which can
# only %set extio/<box>/cmd/... -- no Tcl eval) to kick off a shelf OTA. Value =
# "<channel> ?<version>?" (empty -> dev/latest). We DEFER the actual pull with
# `dservAfter 0` so the triggering %set returns its rc immediately: extio_ota_push_shelf
# blocks a few seconds on the .bin fetch, and the client's command socket would
# otherwise time out waiting for the reply. (Plain Tcl `after 0` would never fire
# -- dserv doesn't spin the Tcl event loop; dservAfter runs it on the process thread.)
proc extio_ota_pull_trigger {dp data} {
    if { ![regexp {^extio/([^/]+)/cmd/ota/pull$} $dp -> box] } return
    set toks [split [string trim $data]]
    set channel [expr {[llength $toks] >= 1 && [lindex $toks 0] ne "" ? [lindex $toks 0] : "dev"}]
    set version [expr {[llength $toks] >= 2 ? [lindex $toks 1] : ""}]
    dservAfter 0 [list extio_ota_pull_run $box $channel $version]
}

proc extio_ota_pull_run {box channel version} {
    if { [catch { extio_ota_push_shelf $box $channel $version } r] } {
        puts "extio ota\[$box\]: shelf pull FAILED: $r"
        # Surface it to the UI on the same state/ota keys the box would use (the box
        # never got cmd/ota/begin, so it won't publish these itself on a resolve error).
        catch { dservSet extio/$box/state/ota/state  fail }
        catch { dservSet extio/$box/state/ota/result "shelf: $r" }
    }
}

proc extio_ota_on_state {dp data} {
    if { ![regexp {^extio/([^/]+)/state/ota/state$} $dp -> box] } return
    set prog ""
    if { [dservExists extio/$box/state/ota/progress] } { set prog " [dservGet extio/$box/state/ota/progress]%" }
    set res ""
    if { [dservExists extio/$box/state/ota/result] }   { set res " ([dservGet extio/$box/state/ota/result])" }
    puts "extio ota\[$box\]: $data$prog$res"
    if { $data eq "armed" } {
        puts "extio ota\[$box\]: verified into the inactive slot -- rebooting into the TBYB trial (back in ~10s)"
    }
    # By ok/armed the box has fully pulled the image into its slot, so free the
    # staged ~150KB (eth pull) and stop any USB ack-driven resender. (armed => ok.)
    if { $data eq "ok" || $data eq "armed" || $data eq "fail" } {
        catch { dservClear extio/$box/ota/image }
        catch { extio_ota_usb_cleanup $box }
    }
}

# ============================================================================
# REQUEST/RESPONSE over datapoints -- the missing layer under the OTA lifecycle
# ============================================================================
#
# Everything that sets a `cmd/...` and waits for a `state/...` has the same four
# problems, and dservWhen solves none of them by itself. It is the right
# primitive -- the only non-blocking observation we have, and blocking would
# deadlock the interp that produces the value -- but it is a transport, not a
# protocol.
#
#   1. NO CORRELATION. `state/ota/arm = armed` does not say WHICH arm it
#      answers. dserv never deletes a datapoint, and dservWhen does a LEVEL
#      CHECK AT REGISTRATION -- it fires immediately if the current value
#      already satisfies. So a reply retained from the previous run satisfies a
#      watch armed for a fresh one, and the caller proceeds against a box that
#      has done nothing. Reading a retained value as live is the single most
#      repeated mistake in this tree (PORTING.md records four, including a
#      boot-loop harness that scored 8/8 while measuring nothing).
#
#   2. NO TIMEOUT. It has to be bolted on with dservAfter and cancelled on
#      every exit path, including the ones you did not think of.
#
#   3. ABSENCE IS NOT OBSERVABLE. A dead box publishes nothing, so "failed" and
#      "still working" are the same signal until a deadline fires. That is not
#      hypothetical: `dserv.live=down` with a frozen watchdog is exactly what a
#      slow transfer looks like from here.
#
#   4. THE SEQUENCE LIVES ONLY IN THE READER'S HEAD -- N callbacks each arming
#      the next, with the order implied rather than stated.
#
# extio_await / extio_request fix 1-3 by construction: the clear cannot be
# forgotten because it is inside the call, and the deadline is a required
# argument rather than an optional habit. ONE request in flight per box, which
# is what makes correlation trivial -- there is only ever one thing it could be
# the answer to.
#
# ORDER IS clear -> watch -> send, and it is not arbitrary. Clearing after
# arming would race the reply; sending before arming could let a fast box reply
# unobserved. A reply that lands between the clear and the watch is still
# caught, because of the level check in (1) -- the same property that makes
# stale values dangerous makes this ordering safe.

set ::extio_req_seq 0

# Wait for a reply that some OTHER action will provoke (or that needs no
# command at all -- e.g. a box re-announcing after a reset).
proc extio_await {box label reply predicate timeout_ms on_ok on_fail {clear {}}} {
    extio_req_clear $box
    foreach k $clear { catch { dservClear extio/$box/state/$k } }

    set id [incr ::extio_req_seq]
    set ::extio_req($box,id)     $id
    set ::extio_req($box,label)  $label
    set ::extio_req($box,on_ok)  $on_ok
    set ::extio_req($box,on_fail) $on_fail
    set ::extio_req($box,timer) \
        [dservAfter $timeout_ms [list extio_req_timeout $box $id $label $timeout_ms]]
    set ::extio_req($box,when) \
        [dservWhen extio/$box/state/$reply $predicate extio_req_reply]
    return $id
}

# ...and the common case: send a command, then wait for its reply. resend_ms > 0
# re-sets the command on that period until the reply lands or the deadline
# fires. For IDEMPOTENT commands whose delivery can race a link coming up --
# and one does, every time (measured 2026-08-10): after the trial reboot the
# box's announce burst fires on the connect-back ACCEPT, BEFORE the box has
# re-sent its %match lines, so a confirm fired off fw_ver is set while the
# box's send-client has no match patterns and silently goes nowhere. There is
# no host-observable "matches registered" signal; retrying the command IS the
# probe. Only pass it for commands that are safe to deliver twice (confirm is:
# boot_write_img_confirmed just re-writes a flag).
proc extio_request {box label cmd value reply predicate timeout_ms on_ok on_fail {clear {}} {resend_ms 0}} {
    set id [extio_await $box $label $reply $predicate $timeout_ms $on_ok $on_fail $clear]
    catch { dservSet extio/$box/cmd/$cmd $value }
    if { $resend_ms > 0 } {
        set ::extio_req($box,resend) \
            [dservAfter $resend_ms [list extio_req_resend $box $id $cmd $value $resend_ms]]
    }
    return $id
}

proc extio_req_resend {box id cmd value ms} {
    # Only while THIS request is still in flight: the id check makes a late
    # timer racing a resolved (or replaced) request a no-op.
    if { ![info exists ::extio_req($box,id)] || $::extio_req($box,id) != $id } return
    catch { dservSet extio/$box/cmd/$cmd $value }
    set ::extio_req($box,resend) \
        [dservAfter $ms [list extio_req_resend $box $id $cmd $value $ms]]
}

# Drop whatever this box has in flight. Both elements are ROUTINELY absent (no
# request yet, already-completed request), and in this file `catch` is not
# silence -- errormon traces every raised error even when caught -- so test
# first rather than catching the expected case.
proc extio_req_clear {box} {
    if { [info exists ::extio_req($box,when)] }   { catch { dservWhenCancel $::extio_req($box,when) } }
    if { [info exists ::extio_req($box,timer)] }  { catch { dservAfterCancel $::extio_req($box,timer) } }
    if { [info exists ::extio_req($box,resend)] } { catch { dservAfterCancel $::extio_req($box,resend) } }
    array unset ::extio_req $box,*
}

proc extio_req_reply {dp value} {
    if { ![regexp {^extio/([^/]+)/state/} $dp -> box] } return
    if { ![info exists ::extio_req($box,on_ok)] } return     ;# already resolved
    set cb $::extio_req($box,on_ok)
    extio_req_clear $box                                     ;# cancel the deadline FIRST
    {*}$cb $box $value
}

proc extio_req_timeout {box id label ms} {
    # Ignore a deadline for a request that already resolved: the timer and the
    # reply can both be in flight, and the id makes that unambiguous.
    if { ![info exists ::extio_req($box,id)] || $::extio_req($box,id) != $id } return
    set cb $::extio_req($box,on_fail)
    extio_req_clear $box
    {*}$cb $box "no reply to '$label' within [expr {$ms/1000}]s -- box stopped responding?"
}

# ============================================================================
# OTA LIFECYCLE -- stage -> verify -> arm -> trial boot -> confirm
# ============================================================================
#
# WHICH DATAPOINTS MATTER. This map is half the point of the section: a web page
# and a human need the same one, and until now it lived only in CHEATSHEET.md
# where code cannot consult it and it cannot go stale loudly.
#
#   state/ota/state        idle|staging|verify|ok|fail  -- transfer outcome
#   state/ota/result       none|too_big|flash|size_mismatch|sha_mismatch|
#                          sha_init|bad_state|bad_geometry
#   state/ota/progress     0..100
#   state/ota/ack          contiguous byte cursor (also paces the resender)
#   state/ota/flash_verify 1 match | 0 mismatch | -1 nothing staged. A READ-BACK
#                          hash: state=ok only says the bytes crossed the link,
#                          this says what is actually IN the slot.
#   state/ota/hdr_ok       1 = MCUboot header readable WHERE MCUBOOT LOOKS
#   state/ota/staged_ver   version in the slot (published by verify)
#   state/ota/arm          armed|failed|refused_in_obs|
#                          refused_no_verified_image|refused_no_image_header
#   state/ota/confirm      confirmed|failed
#   state/ota/trial        1 = running an unconfirmed image
#   state/fw_ver           what MCUboot ACTUALLY booted. NOT state/fw, which is
#                          a build-time constant and can never show an update.
#
# Rolled up into ONE key for the UI:
#   state/ota/lifecycle      idle|staging|verifying|arming|trial|confirming|
#                            done|failed
#   state/ota/lifecycle_msg  the same, as a sentence
#
# Each phase below is now one extio_request/extio_await plus a validator. The
# clear-before-watch and the deadline are inside the layer, so they cannot be
# forgotten per-phase -- which is exactly how the retained-value trap gets
# reintroduced.

# ---- READ-BACK EVIDENCE, AND THE ONE RULE ABOUT IT -------------------------
#
# flash_verify/hdr_ok/staged_ver are the only answer to "what is actually IN the
# inactive slot". dserv RETAINS, and nothing in the transfer path used to touch
# them -- so a verify from the PREVIOUS update survived the next one intact:
# same flash_verify=1, same hdr_ok=1, and a staged_ver still naming the OLD
# version. extio_ota_arm gates on PRESENCE and VALUE, both of which that stale 1
# satisfies, so it would arm a slot nobody had read back -- the exact failure the
# gate was written to prevent. Observed 2026-08-11 on boxa: a completed
# push_shelf fetch (ota/state=ok, progress=100, ack at the new byte count)
# sitting next to three verify keys 36 MINUTES old. The lifecycle was never
# exposed -- extio_ota_update clears these in its transfer await and chains a
# verify -- but every standalone transfer (extio_ota_push_shelf, the
# cmd/ota/pull trigger, the docked path) did neither.
#
# Two defences, because they cover different holes:
#
#   extio_ota_evidence_clear -- every transfer proc clears these BEFORE the
#   first byte moves, so ABSENCE is the post-transfer state. Absence is the one
#   case extio_ota_arm already refuses correctly and specifically, and a clear
#   inside the transfer proc cannot be forgotten by a caller.
#
#   extio_ota_evidence_stale -- a timestamp check for what the clear cannot
#   reach: a client that %sets cmd/ota/begin or cmd/ota/fetch itself, without
#   passing through any proc here. There, absence is not available to us, so ask
#   the other question -- was the read-back done AFTER the bytes it vouches for?
set ::extio_ota_verify_keys {ota/flash_verify ota/hdr_ok ota/staged_ver}

# dservClear on an absent point is a no-op returning TCL_OK, not an error, so
# there is nothing here to catch.
proc extio_ota_evidence_clear {box} {
    foreach k $::extio_ota_verify_keys { dservClear extio/$box/state/$k }
}

# A HEALTHY verify is allowed to read a little older than the transfer keys, and
# not because of clock skew. On any OTA event the firmware re-announces
# state/result/progress/ack once a second for the next 6 s (ota_res_until,
# extio-zephyr/src/main.c) -- and the verify is itself such an event, so the
# verify's own re-announce burst re-stamps ota/state a few seconds AFTER
# flash_verify landed. A strict ordering test would therefore refuse the
# ordinary manual sequence (verify, then arm a second later). This window clears
# that burst with room to spare, and the failure it exists to catch is off by
# minutes, not seconds.
set ::extio_ota_verify_grace_us [expr {10 * 1000000}]

proc extio_ota_age {ts} { return "[format %.0f [expr {([now] - $ts) / 1000000.0}]]s" }

# "" when the evidence is trustworthy (or when there is no transfer on record to
# compare it against); otherwise the sentence explaining why it is not.
proc extio_ota_evidence_stale {box} {
    set v extio/$box/state/ota/flash_verify
    if { ![dservExists $v] } { return "" }        ;# absent is a different answer -- the caller's
    set vts [dservTimestamp $v]

    # Newest transfer footprint wins: ota/state carries the ok|fail that ENDS a
    # transfer, ack and progress move throughout one. Any of them later than the
    # read-back means bytes landed in the slot after we last looked at it.
    set tts 0; set twhat ""
    foreach k {ota/state ota/ack ota/progress} {
        set p extio/$box/state/$k
        if { ![dservExists $p] } continue
        set ts [dservTimestamp $p]
        if { $ts > $tts } { set tts $ts; set twhat $k }
    }
    if { $tts == 0 } { return "" }
    if { $vts >= $tts - $::extio_ota_verify_grace_us } { return "" }

    set sv "?" ; catch { set sv [dservGet extio/$box/state/ota/staged_ver] }
    return "state/ota/flash_verify is [extio_ota_age $vts] old but state/$twhat is only\
            [extio_ota_age $tts] old -- the read-back PREDATES the transfer it would\
            vouch for, and describes whatever was in the slot BEFORE it\
            (staged_ver=$sv)"
}

proc extio_ota_lc_pub {box phase msg} {
    set ::extio_ota_lc($box,phase) $phase
    catch { dservSet extio/$box/state/ota/lifecycle     $phase }
    catch { dservSet extio/$box/state/ota/lifecycle_msg $msg }
    puts "extio ota\[$box\]: $phase -- $msg"
}

proc extio_ota_lc_fail {box why} {
    extio_req_clear $box
    extio_ota_lc_pub $box failed $why
}

# ---- entry point ----------------------------------------------------------
proc extio_ota_update {box {channel dev} {version ""}} {
    # A lifecycle is only genuinely IN PROGRESS if it also has a request in
    # flight. Guarding on the phase alone made this unrecoverable: any run that
    # died between phases -- an error in a callback, a reload of this file,
    # anything that dropped the pending request without publishing a terminal
    # phase -- left `phase` stuck mid-lifecycle, and EVERY later call then
    # errored out before publishing a single thing. From outside that is
    # indistinguishable from nothing happening at all: no state/ota/lifecycle,
    # no transfer, and the reason visible only in the return value of the
    # command. Observed 2026-07-31.
    if { [info exists ::extio_ota_lc($box,phase)]
         && $::extio_ota_lc($box,phase) ni {idle done failed} } {
        if { [info exists ::extio_req($box,label)] } {
            error "extio_ota_update: $box is in '$::extio_ota_lc($box,phase)',\
                   waiting on '$::extio_req($box,label)' -- extio_ota_abort $box to cancel"
        }
        puts "extio ota\[$box\]: previous run left phase\
              '$::extio_ota_lc($box,phase)' with nothing in flight -- restarting"
    }
    extio_ota_lc_pub $box staging "staging $channel image"

    # Watch armed BEFORE the push: push_shelf blocks for seconds on the shelf
    # fetch and the transfer itself can finish quickly on a docked box.
    extio_await $box "transfer" ota/state {in {ok fail}} 180000 \
        extio_ota_lc_staged extio_ota_lc_fail \
        {ota/state ota/result ota/progress ota/ack ota/flash_verify ota/hdr_ok
         ota/staged_ver ota/arm ota/arm_rc ota/confirm ota/confirm_rc}

    if { [catch { extio_ota_push_shelf $box $channel $version } r] } {
        extio_ota_lc_fail $box "shelf: $r"
        return "ota $box: FAILED before transfer -- $r"
    }
    return "ota $box: lifecycle started -- watch extio/$box/state/ota/lifecycle"
}

proc extio_ota_abort {box} {
    extio_req_clear $box
    catch { extio_ota_cancel ::extio_ota_timer $box }
    catch { dservSet extio/$box/cmd/ota/abort 1 }
    extio_ota_lc_pub $box failed "aborted by operator"
}

# ---- phases ---------------------------------------------------------------
proc extio_ota_lc_staged {box value} {
    if { $value ne "ok" } {
        set r "?" ; catch { set r [dservGet extio/$box/state/ota/result] }
        extio_ota_lc_fail $box "transfer failed ($r)"
        return
    }
    extio_ota_lc_pub $box verifying "read-back hashing the slot"
    extio_request $box "verify" ota/verify 1 ota/flash_verify {in {1 0 -1}} 90000 \
        extio_ota_lc_verified extio_ota_lc_fail \
        {ota/flash_verify ota/hdr_ok ota/staged_ver}
}

proc extio_ota_lc_verified {box value} {
    if { $value != 1 } {
        extio_ota_lc_fail $box "read-back hash mismatch (flash_verify=$value) -- the slot does NOT hold the image we sent"
        return
    }
    # hdr_ok and staged_ver are published in the same breath as flash_verify, so
    # they are readable now. Refusing here beats letting the box refuse the arm:
    # same outcome, but the reason is ours and more specific.
    set hdr 0 ; catch { set hdr [dservGet extio/$box/state/ota/hdr_ok] }
    if { $hdr != 1 } {
        extio_ota_lc_fail $box "no MCUboot header where MCUboot looks (hdr_ok=$hdr) -- image staged at the wrong offset"
        return
    }
    set sv "" ; catch { set sv [dservGet extio/$box/state/ota/staged_ver] }
    set ::extio_ota_lc($box,want_ver) $sv

    # fw_ver is cleared HERE, with the arm, not later: arming resets the box, and
    # fw_ver reappearing is how we learn it came back. Clearing it after the reset
    # would race the announce burst and could erase the very fact we are waiting
    # for; a retained value would report the OLD boot as proof of the new one.
    extio_ota_lc_pub $box arming "arming trial boot of $sv"
    extio_request $box "arm" ota/arm 1 ota/arm {ne ""} 30000 \
        extio_ota_lc_armed extio_ota_lc_fail {ota/arm ota/arm_rc fw_ver}
}

proc extio_ota_lc_armed {box value} {
    if { $value ne "armed" } {
        extio_ota_lc_fail $box "arm refused: $value"
        return
    }
    # No command to send -- the box is already resetting into the MCUboot swap.
    # fw_ver was cleared with the arm, so any value now is a fresh announce.
    extio_ota_lc_pub $box trial "swapping + rebooting into the trial image"
    extio_await $box "trial boot" fw_ver {ne ""} 120000 \
        extio_ota_lc_booted extio_ota_lc_fail
}

proc extio_ota_lc_booted {box value} {
    set want ""
    if { [info exists ::extio_ota_lc($box,want_ver)] } { set want $::extio_ota_lc($box,want_ver) }
    # The box is back and has said which image MCUboot chose. If that is not the
    # staged one, the trial did not take -- MCUboot declined it, or it booted and
    # reverted -- and confirming would be meaningless.
    if { $want ne "" && $value ne $want } {
        extio_ota_lc_fail $box "trial did not take: booted $value, staged $want -- check state/ota/rejects and the boot reason"
        return
    }
    extio_ota_lc_pub $box confirming "trial image $value is live -- confirming"
    # The command ARRIVING is itself the liveness proof: a "publishing but deaf"
    # image cannot ack it, and the deadline catches precisely that. Resent every
    # 2 s: fw_ver (which got us here) lands ms BEFORE the box re-registers its
    # %match lines, so the first send reliably loses that race -- see
    # extio_request.
    extio_request $box "confirm" ota/confirm 1 ota/confirm {in {confirmed failed}} 30000 \
        extio_ota_lc_confirmed extio_ota_lc_fail {ota/confirm ota/confirm_rc} 2000
}

proc extio_ota_lc_confirmed {box value} {
    if { $value ne "confirmed" } {
        extio_ota_lc_fail $box "confirm failed -- the image will REVERT on the next reset"
        return
    }
    set v "?" ; catch { set v [dservGet extio/$box/state/fw_ver] }
    extio_ota_lc_pub $box done "running $v, confirmed permanent"
}

# ---- the lifecycle steps, individually ------------------------------------
#
# extio_ota_update drives all of these in order. They are ALSO exposed one at a
# time, for two reasons. The manual path is what you fall back to when the
# lifecycle is wedged -- which is exactly when you least want to be typing
# `dservSet extio/<box>/cmd/ota/arm 1` from memory. And a bare dservSet reports
# NOTHING: the box answers with a reason (refused_no_verified_image,
# refused_in_obs, refused_no_image_header) that nothing surfaced, so a refused
# arm looked identical to a silent one.
#
# Each shares the request layer, so each gets the clear-before-watch and the
# mandatory deadline for free. One request in flight per box means a standalone
# step and a running lifecycle cannot interleave -- intended, not a limitation.

proc extio_ota_step_fail {box why} { puts "extio ota\[$box\]: $why" }

proc extio_ota_verify {box} {
    extio_request $box "verify" ota/verify 1 ota/flash_verify {in {1 0 -1}} 90000 \
        extio_ota_verify_done extio_ota_step_fail \
        {ota/flash_verify ota/hdr_ok ota/staged_ver}
    return "verify sent to $box"
}
proc extio_ota_verify_done {box value} {
    set hdr "?" ; catch { set hdr [dservGet extio/$box/state/ota/hdr_ok] }
    set sv  "?" ; catch { set sv  [dservGet extio/$box/state/ota/staged_ver] }
    if { $value == 1 && $hdr == 1 } {
        puts "extio ota\[$box\]: verify OK -- slot holds $sv, header where MCUboot looks. Ready to arm."
    } elseif { $value == 1 } {
        puts "extio ota\[$box\]: read-back hash matches but hdr_ok=$hdr -- image staged at the wrong offset; arm would be refused"
    } else {
        puts "extio ota\[$box\]: verify FAILED (flash_verify=$value) -- the slot does NOT hold the image that was sent"
    }
}

proc extio_ota_arm {box} {
    # Refuse locally rather than let the box refuse: same outcome, but the
    # reason is specific and arrives without a round trip. flash_verify is a
    # READ-BACK; ota/state=ok only means the bytes crossed the link.
    # ABSENT and ZERO ARE DIFFERENT ANSWERS, and conflating them sends you to fix
    # the wrong thing. `catch {set fv [dservGet ...]}` leaving fv at 0 reports
    # "verify failed" for a datapoint that was never readable here -- which is
    # what it did on 2026-07-31 while the box console plainly said
    # "slot1 sha MATCHES". Say which one it is.
    if { ![dservExists extio/$box/state/ota/flash_verify] } {
        return "extio_ota_arm: $box -- state/ota/flash_verify is NOT PRESENT.\
                Either verify has not run, or its reply is not reaching this\
                interp. Check the box console: if it says `slot1 sha MATCHES`\
                then the box answered and the datapoint is being lost between\
                the box and here."
    }
    # PRESENT IS NOT FRESH. dserv retains, so the verify keys from the previous
    # update outlive the next transfer untouched -- see extio_ota_evidence_stale.
    # Checked before the value: a stale 1 and a stale 0 are equally uninformative,
    # and the fix for both is the same verify.
    set stale [extio_ota_evidence_stale $box]
    if { $stale ne "" } {
        return "extio_ota_arm: $box -- $stale.\
                Run `extio_ota_verify $box` and check state/ota/staged_ver names the\
                version you just pushed."
    }
    set fv [dservGet extio/$box/state/ota/flash_verify]
    if { $fv != 1 } {
        return "extio_ota_arm: $box read-back verify FAILED (flash_verify=$fv)\
                -- the slot does not hold the image that was sent"
    }
    if { [dservExists extio/$box/state/ota/hdr_ok]
         && [dservGet extio/$box/state/ota/hdr_ok] != 1 } {
        return "extio_ota_arm: $box has no MCUboot header where MCUboot looks\
                (hdr_ok=[dservGet extio/$box/state/ota/hdr_ok]) -- arm would be refused"
    }
    extio_request $box "arm" ota/arm 1 ota/arm {ne ""} 30000 \
        extio_ota_arm_done extio_ota_step_fail {ota/arm ota/arm_rc fw_ver}
    return "arm sent to $box"
}
proc extio_ota_arm_done {box value} {
    if { $value eq "armed" } {
        puts "extio ota\[$box\]: ARMED -- box is rebooting into ONE trial boot. Any reset before confirm reverts."
    } else {
        set rc "?" ; catch { set rc [dservGet extio/$box/state/ota/arm_rc] }
        puts "extio ota\[$box\]: arm refused: $value (rc=$rc)"
    }
}

proc extio_ota_confirm {box} {
    extio_request $box "confirm" ota/confirm 1 ota/confirm {in {confirmed failed}} 30000 \
        extio_ota_confirm_done extio_ota_step_fail {ota/confirm ota/confirm_rc} 2000
    return "confirm sent to $box"
}
proc extio_ota_confirm_done {box value} {
    set v "?" ; catch { set v [dservGet extio/$box/state/fw_ver] }
    if { $value eq "confirmed" } {
        puts "extio ota\[$box\]: CONFIRMED -- $v is now permanent"
    } else {
        puts "extio ota\[$box\]: confirm FAILED -- $v will REVERT on the next reset"
    }
}

# Rejecting a trial is deliberately not a command: there is nothing to send.
proc extio_ota_reject {box} {
    return "to reject the trial on $box: do NOT confirm -- power cycle it. The\
            previous image returns by construction and the box reports `revert`\
            in state/ota/*. That is the safety property, not a workaround."
}

# One-line status for a human or a polling UI. AGE on every field, always:
# dserv retains, so a dead box serves its last values forever and every field
# reads healthy. The value alone is not evidence.
proc extio_ota_status {box} {
    set out {}
    foreach k {ota/lifecycle ota/lifecycle_msg ota/state ota/progress ota/ack
               ota/flash_verify ota/hdr_ok ota/staged_ver ota/arm ota/confirm
               ota/trial fw_ver} {
        set d extio/$box/state/$k
        if { ![dservExists $d] } continue
        lappend out "$k=[dservGet $d] ([extio_ota_age [dservTimestamp $d]])"
    }
    return [join $out "\n"]
}

# ============================================================================
# CONFIG WRITES -- the named, whitelisted surface a UI is allowed to touch
# ============================================================================
#
# The web page talks to this interp by evaluating Tcl, so without a narrow API
# the config page IS a Tcl console pointed at the rig. These procs are that
# narrowing: a page can set a box's debounce, and cannot `exec rm`.
#
# WHITELISTED BY SHAPE, not by string equality, because the leaves are indexed
# (pin/7/label, group/2/pins, ain/group/1/channels). The patterns below mirror
# the sscanf formats in dserv_config.h; anything not matching is refused with the
# leaf named, so a typo says so instead of silently setting a datapoint that no
# box will ever read -- dserv accepts any name, which makes a misspelled leaf
# indistinguishable from a working one.
#
# NOTE ON PERSISTENCE, because it is the thing operators lose settings to: these
# apply LIVE and do NOT survive a reboot until `extio_cfg_save`. A couple
# (console) additionally need the reboot to take effect at all. A UI that does
# not show the difference will quietly cost somebody their pin map.

set ::extio_cfg_writable {
    name desc
    net/mode net/ip net/mask net/gateway
    dserv/ip dserv/port
    wifi/ssid wifi/pass wifi/pm
    obs/pin obs/mode sync/pin xport/mode
    ain/enable ain/rate ain/oversample ain/pace ain/clk_ppm
    dbg/level
    console channel
}
set ::extio_cfg_writable_rx {
    {^pin/[0-9]+/(mode|label|active_low|debounce_ms|pulse_us)$}
    {^group/[0-9]+/(pins|label|settle_ms|quiet)$}
    {^ain/group/[0-9]+/[a-z_]+$}
    {^ain/label/[0-9]+$}
}

proc extio_cfg_writable {leaf} {
    if { $leaf in $::extio_cfg_writable } { return 1 }
    foreach rx $::extio_cfg_writable_rx { if { [regexp $rx $leaf] } { return 1 } }
    return 0
}

# Add a leaf to the allowlist at RUNTIME, for the exact case below: firmware has
# gained a config leaf and this dserv has not been restarted yet. Does not
# persist -- put it in ::extio_cfg_writable above so it survives a restart.
proc extio_cfg_allow {leaf} {
    if { [extio_cfg_writable $leaf] } { return "already writable: $leaf" }
    lappend ::extio_cfg_writable $leaf
    set ::extio_cfg_writable [lsort -unique $::extio_cfg_writable]
    puts "extio: allowlisted config leaf '$leaf' (RUNTIME only -- add it to\
          ::extio_cfg_writable in config/extioconf.tcl to keep it)"
    return "allowed: $leaf"
}

proc extio_cfg_set {box leaf value} {
    if { ![extio_cfg_writable $leaf] } {
        # LOUD, AND ACTIONABLE, because this gate is a DUPLICATE of knowledge
        # that really lives in the firmware. dserv_cfg_apply() already refuses
        # a leaf the box does not know; this list exists only so a typo is
        # caught before it reaches the wire, and it therefore has to be updated
        # by hand every time firmware gains a setting. It caught ain/pace,
        # dbg/level and ain/clk_ppm in succession during one evening's work,
        # each time presenting as "the box ignored my write" rather than as a
        # host-side omission -- so the message now says exactly which file to
        # edit and gives the one-liner that unblocks a running rig.
        set hint "extio_cfg_set: '$leaf' is not in dserv's writable-leaf allowlist.
    The BOX may well accept it -- this is a SECOND, host-side gate that has to be
    updated whenever the firmware gains a config leaf.
    Unblock this dserv now:   extio_cfg_allow $leaf
    Keep it:                  add '$leaf' to ::extio_cfg_writable in config/extioconf.tcl"
        puts "extio: REFUSED config write '$leaf' -- not in ::extio_cfg_writable\
              (see extio_cfg_allow to unblock without a restart)"
        error $hint
    }
    if { ![dservExists extio/$box/state/fw] && ![dservExists extio/$box/state/board] } {
        error "extio_cfg_set: no box '$box' has announced itself"
    }
    dservSet extio/$box/config/$leaf $value
    lappend ::extio_cfg_dirty($box) $leaf
    set ::extio_cfg_dirty($box) [lsort -unique $::extio_cfg_dirty($box)]
    return "set $leaf = $value on $box (LIVE, not saved)"
}

# Leaves changed since the last save. The UI shows this; the box cannot, because
# it has no idea which of its settings came from flash and which from a datapoint
# five seconds ago.
proc extio_cfg_dirty {box} {
    if { [info exists ::extio_cfg_dirty($box)] } { return $::extio_cfg_dirty($box) }
    return {}
}

# Ask the box to re-publish its FULL manifest now.
#
# The announce burst otherwise fires only when a host opens the pipe, and only
# SOME config edits re-publish the manifest as a side effect (pin mode,
# obs/sync, debounce, active_low, group, label, desc). Set ain/rate or dserv/ip
# and NOTHING is published, so a UI keeps showing the old value -- which is
# indistinguishable from the write having failed, and is exactly the kind of
# silent staleness this tree keeps paying for.
proc extio_cfg_announce {box} {
    dservSet extio/$box/cmd/announce 1
    return "announce requested from $box"
}

# THE LEDGER IS CLEARED BY THE BOX'S RECEIPT, NEVER BY THE SEND.
#
# This used to be `dservSet cmd/save 1` followed unconditionally by unsetting
# the dirty list: fire and forget, with the UI reporting a success it had not
# observed. A cmd/save that never arrived -- which is exactly what happens in
# the ~30 s window after an adoption that lost its cmd/* match, and equally
# after a rename the box has not taken -- looked identical to one that worked,
# and the config quietly reverted on the next reboot. The rename comment below
# has warned about this since +59; the honest fix is to wait for the box.
#
# state/cfg/saved is the receipt (firmware >= v0.4.0+89): >0 bytes written,
# 0 the flash write failed, <0 nothing written but nothing wrong (deferred
# behind an obs, or no persistence on this board). A deferred save publishes
# twice -- SAVED_DEFERRED now, the real outcome when the obs ends -- so the
# deadline here is generous enough to cover an ordinary trial.
#
# Async by construction: dserv's Tcl has no event loop to block on, so this
# returns immediately and the ledger clears from the callback. The page polls
# extio_cfg_dirty, so the banner and the Save button follow the box rather
# than the click.
proc extio_cfg_save {box} {
    extio_request $box "save" save 1 cfg/saved {ne ""} 20000 \
        extio_cfg_save_done extio_cfg_save_failed {cfg/saved}
    return "save sent to $box -- waiting for the box to confirm"
}

proc extio_cfg_save_done {box value} {
    if { ![string is integer -strict $value] } {
        puts "extio: $box save returned an unreadable receipt '$value' -- ledger kept"
        return
    }
    if { $value > 0 } {
        unset -nocomplain ::extio_cfg_dirty($box)
        puts "extio: $box saved ($value bytes)"
    } elseif { $value == 0 } {
        puts "extio: $box SAVE FAILED (flash write error -- see the box console\
              for the errno). Unsaved changes kept in the ledger."
    } elseif { $value == -1 } {
        # Deferred behind an obs. The box publishes again when it lands, but
        # THIS request is already resolved -- re-arm on the same receipt so the
        # ledger still clears when the write actually happens.
        puts "extio: $box save deferred to the end of the current obs"
        extio_await $box "deferred save" cfg/saved {ne ""} 300000 \
            extio_cfg_save_done extio_cfg_save_failed {cfg/saved}
    } else {
        puts "extio: $box has no persistent store -- nothing was saved"
    }
}

proc extio_cfg_save_failed {box why} {
    puts "extio: $box save NOT confirmed -- $why. Unsaved changes kept in the\
          ledger; check `%getmatch <boxip> 5010` for extio/$box/cmd/*."
}

proc extio_cfg_reboot {box} {
    dservSet extio/$box/cmd/reboot 1
    return "reboot sent to $box -- unsaved changes are lost by definition"
}

# ============================================================================
# RENAME -- one committed operation, not a config edit
# ============================================================================
# `name` stays writable via extio_cfg_set for bench/CLI use, but a bare rename
# is the worst possible "applies live" edit: the instant the box takes it, its
# datapoint prefix changes -- the page's selection, the dirty ledger, and
# every cmd/* forward are keyed to a name that no longer answers. Worse, a
# Save clicked under the old name is silently dropped by the box's dispatcher
# while extio_cfg_save clears the ledger anyway: the UI reports saved, and the
# next reboot reverts the name.
#
# extio_rename makes it one operation with an observable outcome:
#   1. refuse while the old name has unsaved edits (the ledger cannot
#      straddle identities) or a non-terminal OTA lifecycle;
#   2. clear + watch extio/<new>/state/build -- the announce burst under the
#      NEW name. Firmware >= +59 does a full re-registration on rename, and
#      the announce hold guarantees that burst follows the new %match lines,
#      so build appearing MEANS the box is already commandable under <new>;
#   3. set config/name;
#   4. on the burst: cmd/save under <new> (an unsaved rename reverts on
#      reboot -- exactly the confusion this proc exists to kill), then
#      extio_clear <old> purges the ghost identity.
# On pre-+59 firmware nothing re-registers and no burst fires: the await
# times out and the failure text says what state the box is really in.
proc extio_rename {old new} {
    if { $new eq $old } { return "extio_rename: already named '$old'" }
    # Stricter than the firmware's dserv_name_valid (any printable but '/'):
    # names ride whitespace-split %match lines and web UIs, so keep them
    # word-shaped. 15 chars = BOX_NAME_MAX 16 minus the terminator.
    if { ![regexp {^[A-Za-z0-9][A-Za-z0-9._-]*$} $new] || [string length $new] > 15 } {
        error "extio_rename: bad name '$new' -- letters/digits/._- (leading\
               alphanumeric), at most 15 chars"
    }
    if { ![dservExists extio/$old/state/build] } {
        error "extio_rename: no box '$old' has announced itself"
    }
    if { [info exists ::extio_known($new)] } {
        error "extio_rename: a live box is already named '$new'"
    }
    if { [llength [extio_cfg_dirty $old]] } {
        error "extio_rename: $old has unsaved changes ([extio_cfg_dirty $old])\
               -- Save or reboot first, so the ledger doesn't straddle identities"
    }
    if { [info exists ::extio_ota_lc($old,phase)]
         && $::extio_ota_lc($old,phase) ni {idle done failed} } {
        error "extio_rename: $old has an OTA in '$::extio_ota_lc($old,phase)' -- not while that runs"
    }
    # Arm the watch under the NEW name BEFORE firing (the burst can beat a
    # late registration), and clear build first: a retained value from some
    # earlier box that once held this name would satisfy the level check
    # instantly and fake success.
    extio_await $new "rename from $old" build {ne ""} 20000 \
        [list extio_rename_up $old] extio_rename_fail {build}
    dservSet extio/$old/config/name $new
    return "rename $old -> $new: box re-registering; watch extio/$new/state/build"
}

proc extio_rename_up {old box value} {
    dservSet extio/$box/cmd/save 1   ;# persist NOW: an unsaved rename reverts on reboot
    catch { extio_clear $old }       ;# forwards + retained datapoints + tracking
    unset -nocomplain ::extio_cfg_dirty($old)
    # Tombstone for BYSTANDER pages. extio_clear removed the old identity from
    # the table, but dserv never pushes deletions -- so every open page EXCEPT
    # the one that clicked Rename keeps a ghost card (and a frozen panel)
    # under the old name until reload (observed 2026-08-10). One retained key
    # that all pages mirror: drop <old>, follow to <new> if it was selected.
    # Set AFTER the clear, so it cannot be swept with the old subtree.
    dservSet extio/renamed "$old $box"
    puts "extio rename: $old -> $box live + saved; old identity cleared"
}

proc extio_rename_fail {box why} {
    puts "extio rename\[$box\]: no announce under the new name -- $why. On pre-+59\
          firmware the box HAS renamed live but cannot say so: wait ~30 s (match\
          refresh) then extio_cfg_save $box -- or reboot it to revert the rename."
}

proc extio_wire_common {} {                 ;# device-independent: sync + obs_pin
    dservAddMatch ess/in_obs
    dpointSetScript ess/in_obs usbio_forward
    dservAddMatch extio/*/state/group/*     ;# chord-group events + manifest -> decode
    dpointSetScript extio/*/state/group/* extio_group_decode
    dservAddMatch extio/*/state/label/*     ;# relabels invalidate cached maps
    dpointSetScript extio/*/state/label/* extio_label_invalidate
    # analog manifest: react to a group or a channel name changing rather than
    # waiting up to one 2 s tick, so a file opened right after an edit records
    # what the page just showed. Publish-on-change makes this idempotent with
    # the tick.
    dservAddMatch   extio/*/state/ain/group/*/chans
    dpointSetScript extio/*/state/ain/group/*/chans extio_ain_publish_manifest
    dservAddMatch   extio/*/state/ain/label/*
    dpointSetScript extio/*/state/ain/label/* extio_ain_publish_manifest
    dservAddMatch extio/*/state/ota/state   ;# OTA progress log + free the staged image on finish
    dpointSetScript extio/*/state/ota/state extio_ota_on_state
    dservAddMatch extio/*/cmd/ota/pull      ;# network-triggered shelf OTA (extio-setup dserv mode)
    dpointSetScript extio/*/cmd/ota/pull extio_ota_pull_trigger
    dservAddMatch extio/*/state/build       ;# one set per announce burst = one per uplink connect
    dpointSetScript extio/*/state/build extio_on_connect
    dservAddMatch extio/*/state/ota/trial   ;# trial->0 = OTA settled: unstick a stale lifecycle
    dpointSetScript extio/*/state/ota/trial extio_ota_on_trial
    dservAddMatch extio/*/state/obs_leader  ;# leader announce -> rig-level auto-bind (opt-in)
    dpointSetScript extio/*/state/obs_leader extio_on_obs_leader
}

# ---- connect counter (fleet page): a box bursts its announce at every uplink
# (re)connect (BOX_NET_RESET in extio-zephyr main.c; publish_ident on the
# RP2350s), and state/build is published exactly once per burst on both
# families -- so counting its sets counts connects. The count baselines at
# THIS dserv's start (a restart resets it), which is the question the counter
# answers: "is the link churning while the host sits stable?" A steady box
# reads 1; every increment after that is a reconnect (box reboot, cable/PHY
# event, peer-not-draining cycle) -- or an operator's cmd/announce, which
# also bursts; the timestamp says when to check the console. ----
proc extio_on_connect {dp data} {
    if { ![regexp {^extio/([^/]+)/state/build$} $dp -> box] } return
    set n 0
    catch { set n [dservGet extio/$box/host/connects] }
    if { ![string is integer -strict $n] } { set n 0 }
    dservSet extio/$box/host/connects [expr {$n + 1}]
    dservSet extio/$box/host/connect_last \
        [clock format [clock seconds] -format %H:%M:%S]
    # Self-heal a STALE OTA lifecycle. A box that (re)registers while the
    # lifecycle sits in a non-terminal transfer phase is orphaned: the confirm
    # request timed out (human delay past the 30 s deadline), the confirm was
    # driven from the console, or the trial image reverted. The web UI then
    # sticks at "confirming" with no shelf-update option (box02, 2026-08-05).
    # trial==0 means the box has SETTLED on a permanent image, so the transfer
    # is over however it ended -- resolve the UI. A genuinely in-flight OTA
    # has trial==1 across its one reboot, so this never pre-empts a live one.
    catch {
        set phase [dservGet extio/$box/state/ota/lifecycle]
        if { $phase in {staging verifying arming trial confirming} } {
            set trial 1
            catch { set trial [dservGet extio/$box/state/ota/trial] }
            if { $trial == 0 } {
                set v "?"; catch { set v [dservGet extio/$box/state/fw_ver] }
                extio_ota_lc_pub $box done "running $v (settled after reconnect)"
            }
        }
    }
}

# The confirm that resolves the lifecycle can arrive WITHOUT the web request
# catching it: from the console, or after the 30 s request deadline lapsed
# while a human read the log. The box publishes state/ota/trial -> 0 on
# confirm (main.c) regardless of HOW confirm happened, so watch that as the
# authoritative "done" signal -- the request callback is now just the fast
# path, not the only one. (box02 console-confirm left the UI stuck, 2026-08-05.)
proc extio_ota_on_trial {dp data} {
    if { ![regexp {^extio/([^/]+)/state/ota/trial$} $dp -> box] } return
    if { $data != 0 } return
    set phase ""
    catch { set phase [dservGet extio/$box/state/ota/lifecycle] }
    if { $phase in {confirming trial arming} } {
        set v "?"; catch { set v [dservGet extio/$box/state/fw_ver] }
        extio_ota_lc_pub $box done "running $v, confirmed permanent"
    }
}

# ---- obs-leader auto-bind (opt-in per rig) ---------------------------------
# Announcing `leader` makes a box the obs-onset AUTHORITY -- but the pin only
# fires once the HOST binds (::ess::obs_schedule_bind), and that bind is
# session state: every dserv restart silently reverted a leader rig to "dark
# obs pin, loud per-obs fallback" until someone re-bound by hand (officepi,
# 2026-08-04). Consent lives on the HOST. Its DECLARED home is
# local/extio.tcl (see the .EXAMPLE) -- that file sources last, so its value
# re-asserts at every boot; settings.db (subsystem obs_autobind) is the
# runtime persistence, and the fallback for rigs with no file line.
# Values: "" = off (default), "auto" = bind whatever single
# leader announces, "<boxname>" = only that box. The TRIGGER is the announce
# (state/obs_leader -> 1), never boot -- announces always come (registration
# burst, box reboot, cmd/announce), so the bind self-heals across every
# disruption class without racing startup. Re-announces are idempotent: an
# already-bound host is left alone. Never grabs authority mid-obs (defers).
#
#   send extio {extio_obs_autobind auto}     ;# opt this rig in (persisted)
#   send extio {extio_obs_autobind off}      ;# back to manual binding

# Boolean-intent spellings are consent, not box names: `extio_obs_autobind 1`
# persisted "1", every announce was declined against it as a name filter, and
# the rig sat unbound with zero breadcrumbs (rpi500, 2026-08-06). Normalize on
# read too, so a value persisted before this guard self-heals.
proc extio_obs_autobind_norm {v} {
    switch -nocase -- $v {
        1 - on - true - yes { return auto }
        0 - off - false - no - none { return "" }
    }
    return $v
}

proc extio_obs_autobind_get {} {
    set v ""
    catch { set v [::settingsdb::load obs_autobind] }
    return [extio_obs_autobind_norm $v]
}

# What does local/extio.tcl DECLARE for autobind, if anything?
# Returns {1 <normalized>} when an active (uncommented) line exists, {0 {}}
# otherwise. Last line wins, matching what sourcing the file would do.
proc extio_local_declared_autobind {} {
    set f [file join $::dspath local extio.tcl]
    if { ![file exists $f] || [catch { open $f } fd] } { return {0 {}} }
    set txt [read $fd]; close $fd
    set found 0; set val ""
    foreach line [split $txt \n] {
        set line [string trim $line]
        if { [string index $line 0] eq "#" } continue
        if { [regexp {^extio_obs_autobind\s+(\S+)} $line -> v] } {
            set found 1
            set val [extio_obs_autobind_norm [string trim $v "\"{}"]]
        }
    }
    return [list $found $val]
}

proc extio_obs_autobind {value} {
    set value [extio_obs_autobind_norm $value]
    ::settingsdb::save obs_autobind $value
    dservSet extio/obs_autobind $value
    # apply immediately if a matching leader is already announced
    if { $value ne "" } {
        set boxes {}
        catch { set boxes [dservGet extio/boxes] }
        foreach b $boxes {
            set lead 0
            catch { set lead [dservGet extio/$b/state/obs_leader] }
            if { $lead == 1 } { extio_obs_autobind_try $b }
        }
    }
    set out [expr {$value eq "" ? "off" : $value}]
    # A runtime change is outlived by the file's declaration (re-asserted at
    # every boot) -- say so now, instead of silently reverting at the next
    # restart (the officepi lesson, generalized).
    if { ![info exists ::extio_local_sourcing] || !$::extio_local_sourcing } {
        lassign [extio_local_declared_autobind] dfound dval
        if { $dfound && $dval ne $value } {
            append out " -- NOTE: local/extio.tcl declares\
 '[expr {$dval eq "" ? "off" : $dval}]', which re-asserts at the next dserv\
 restart; edit the file to make this change permanent"
        }
    }
    return $out
}

proc extio_obs_autobind_try {box} {
    set flag [extio_obs_autobind_get]
    if { $flag eq "" } { return }
    if { $flag ne "auto" && $flag ne $box } {
        # a name-filtered rig is quiet only while its chosen box is bound;
        # an unbound rig declining announces must say so (the rpi500 lesson:
        # the silent version of this branch cost a debugging session)
        set bound ""
        catch { set bound [send ess {set ::ess::obs_sched_box}] }
        if { $bound eq "" } {
            dservSet extio/obs_autobind_last "declined $box (flag $flag)"
        }
        return
    }
    set bound ""
    catch { set bound [send ess {set ::ess::obs_sched_box}] }
    if { $bound ne "" } { return }
    set inobs 0
    catch { set inobs [dservGet ess/in_obs] }
    if { $inobs == 1 } {
        dservAfter 3000 [list extio_obs_autobind_try $box]
        return
    }
    # send reports interp errors as "!TCL_ERROR ..." STRINGS, not Tcl errors
    # (the wav_send lesson) -- a naive catch logged a failed resolve as
    # "bound" on officepi 2026-08-04
    set r ""
    catch { set r [send ess {::ess::obs_schedule_bind auto auto 80}] } r
    if { [string first "!TCL_ERROR " $r] == 0 } {
        puts "extio: obs auto-bind to $box failed: $r"
        dservSet extio/obs_autobind_last "failed: $r"
    } else {
        puts "extio: obs scheduler auto-bound to $box"
        dservSet extio/obs_autobind_last \
            "bound $box [clock format [clock seconds] -format %H:%M:%S]"
    }
}

proc extio_on_obs_leader {dp data} {
    if { ![regexp {^extio/([^/]+)/state/obs_leader$} $dp -> box] } return
    if { $data != 1 } return
    # the announce burst delivers obs_leader alongside obs_pin and the rest of
    # the manifest; a short defer lets the whole burst land before the
    # resolver reads it (it requires a watchdog-fresh announce anyway)
    dservAfter 1000 [list extio_obs_autobind_try $box]
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

# surface the persisted auto-bind flag (UIs + humans); "" reads as off
dservSet extio/obs_autobind [extio_obs_autobind_get]

# rig-local DECLARED config + overrides (autobind consent, port pinning,
# extra forwards) -- see local/extio.tcl.EXAMPLE. Sourced LAST deliberately:
# file declarations re-assert over db-persisted values at every boot. The
# flag lets procs called from inside the file (e.g. extio_obs_autobind)
# know the file itself is speaking, so they skip the ephemerality note.
set ::extio_local_sourcing 0
if { [file exists $dspath/local/extio.tcl] } {
    set ::extio_local_sourcing 1
    source $dspath/local/extio.tcl
    set ::extio_local_sourcing 0
}

puts "extio initialized"

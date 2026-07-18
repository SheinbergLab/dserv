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
        # fallback: the old heuristic (pre-identity firmware, or ioreg hiccup).
        # Beware: identity-blind -- a handheld/other CDC device can win the sort.
        set ports [lsort -dictionary [glob -nocomplain /dev/cu.usbmodem*]]
        if {[llength $ports]} { return [lindex $ports end] }
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
proc usbio_forward {dp data} { catch { usbioSendFrame $dp [dservTimestamp $dp] $data } }
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

    # A USB box has no socket to pull over -> PUSH the image as 'D' frames instead
    # of staging it for a pull. Pure-usb and dual-in-USB-mode both report "usb".
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
    catch { after cancel $::extio_ota_timer($box) }

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

proc extio_ota_usb_on_ack {dp data} {                  ;# box published a new contiguous cursor
    if { ![regexp {^extio/([^/]+)/state/ota/ack$} $dp -> box] } return
    if { ![info exists ::extio_ota_size($box)] } return
    if { $data >= $::extio_ota_size($box) } { extio_ota_usb_cleanup $box; return }   ;# all delivered
    extio_ota_usb_deadline $box                                                      ;# progress -> re-arm
    catch { after cancel $::extio_ota_timer($box) }                                  ;# debounce: resend only
    set ::extio_ota_timer($box) [after 400 [list extio_ota_usb_blast $box $data]]    ;# when stuck ~400ms
}

# No-ack deadline: catches the SILENT death shape -- writes "succeed" into a
# doomed kernel buffer (device detached with room left), so the blast finishes
# but no ack ever comes and the ack-driven resender never fires. 10 s with no
# cursor progress (acks normally arrive every ~0.4 s) -> host-side abort. The
# box's own stall timeout (OTA_USB_TIMEOUT_US, 10 s) is the mirror-image guard.
# Progress-checked at fire time: a resend blast can outlive the timer, so an
# expired deadline racing queued ack events must re-arm, not kill the push.
proc extio_ota_usb_deadline {box} {
    catch { after cancel $::extio_ota_dead($box) }
    set at -1
    catch { set at [dservGet extio/$box/state/ota/ack] }
    set ::extio_ota_dead($box) [after 10000 [list extio_ota_usb_deadcheck $box $at]]
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

proc extio_ota_usb_cleanup {box} {                     ;# stop resending (delivered, or state -> armed/fail)
    catch { after cancel $::extio_ota_timer($box) }
    catch { after cancel $::extio_ota_dead($box) }
    catch { unset ::extio_ota_img($box) }
    catch { unset ::extio_ota_size($box) }
    catch { unset ::extio_ota_timer($box) }
    catch { unset ::extio_ota_dead($box) }
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

    # 2. channel manifest (JSON is text -- https_get string return is fine here).
    set url "$base/api/firmware/extio/$channel"
    if { [catch { https_get $url -timeout 15000 } json] } {
        error "extio_ota_push_shelf: shelf fetch failed ($url): $json"
    }
    set d [::yajl::json2dict $json]
    if { $version eq "" } {
        if { [dict exists $d latest] } { set version [dict get $d latest] }
    }
    if { $version eq "" } { error "extio_ota_push_shelf: channel '$channel' has no version to pull" }

    # 3. find the OTA image for this box's build in the latest version.
    set img ""
    foreach v [dict get $d versions] {
        if { ![dict exists $v version] || [dict get $v version] ne $version } continue
        foreach im [dict get $v images] {
            if { ![dict exists $im build] || [dict get $im build] ne $bbuild } continue
            if { ![dict exists $im bin] || [dict get $im bin] eq "" } continue
            set img $im; break
        }
    }
    if { $img eq "" } {
        error "extio_ota_push_shelf: no OTA (.bin) image for build '$bbuild' in $channel/$version"
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

# Datapoint trigger for a network client (extio-setup's dserv driver, which can
# only %set extio/<box>/cmd/... -- no Tcl eval) to kick off a shelf OTA. Value =
# "<channel> ?<version>?" (empty -> dev/latest). We DEFER the actual pull with
# `after 0` so the triggering %set returns its rc immediately: extio_ota_push_shelf
# blocks a few seconds on the .bin fetch, and the client's command socket would
# otherwise time out waiting for the reply.
proc extio_ota_pull_trigger {dp data} {
    if { ![regexp {^extio/([^/]+)/cmd/ota/pull$} $dp -> box] } return
    set toks [split [string trim $data]]
    set channel [expr {[llength $toks] >= 1 && [lindex $toks 0] ne "" ? [lindex $toks 0] : "dev"}]
    set version [expr {[llength $toks] >= 2 ? [lindex $toks 1] : ""}]
    after 0 [list extio_ota_pull_run $box $channel $version]
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

proc extio_wire_common {} {                 ;# device-independent: sync + obs_pin
    dservAddMatch ess/in_obs
    dpointSetScript ess/in_obs usbio_forward
    dservAddMatch extio/*/state/group/*     ;# chord-group events + manifest -> decode
    dpointSetScript extio/*/state/group/* extio_group_decode
    dservAddMatch extio/*/state/label/*     ;# relabels invalidate cached maps
    dpointSetScript extio/*/state/label/* extio_label_invalidate
    dservAddMatch extio/*/state/ota/state   ;# OTA progress log + free the staged image on finish
    dpointSetScript extio/*/state/ota/state extio_ota_on_state
    dservAddMatch extio/*/cmd/ota/pull      ;# network-triggered shelf OTA (extio-setup dserv mode)
    dpointSetScript extio/*/cmd/ota/pull extio_ota_pull_trigger
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

# extio OTA lifecycle test -- runs on plain tclsh, no dserv, no hardware.
#
#   tclsh config/test/extio_ota_lifecycle_test.tcl
#
# Mocks the dserv primitives the lifecycle uses, INCLUDING dservWhen's level
# check at registration -- which is the property that makes retained values
# dangerous, so a mock without it would pass while the real thing broke.
#
# It extracts the request layer + lifecycle straight out of config/extioconf.tcl
# rather than duplicating them, so the test cannot drift from the code.
#
# The cases are the failures actually hit on hardware, not invented ones:
# stale-reply short-circuit, transfer failure, arm refused, deadline expiry, a
# trial that booted the OLD version, a run that died mid-lifecycle wedging every
# later call, and a read-back that describes the PREVIOUS update.
# Mock dserv: in-memory datapoints + dservWhen semantics (predicate as a mathop
# fragment, one-shot, LEVEL CHECK AT REGISTRATION) + manual dservAfter timers.
array set ::DP {}; array set ::TS {}; array set ::WHEN {}; array set ::AFTER {}
set ::seq 0; set ::clock 1000000

proc now {} { return [incr ::clock 1000] }
proc dservExists {k} { info exists ::DP($k) }
proc dservGet {k} { if {![info exists ::DP($k)]} { error "no such dp $k" }; return $::DP($k) }
proc dservTimestamp {k} { return $::TS($k) }
proc dservClear {k} { unset -nocomplain ::DP($k) ::TS($k) }
proc dservSet {k v} {
    set ::DP($k) $v; set ::TS($k) [now]
    foreach id [array names ::WHEN] {
        lassign $::WHEN($id) key pred cb
        if {$key ne $k} continue
        if {[when_true $pred $v]} { unset -nocomplain ::WHEN($id); {*}$cb $k $v; break }
    }
}
proc when_true {pred v} {
    if {$pred eq ""} { return 1 }
    set op [lindex $pred 0]; set rest [lrange $pred 1 end]
    return [expr {[::tcl::mathop::$op $v {*}$rest] ? 1 : 0}]
}
proc dservWhen {key pred cb} {
    set id [incr ::seq]; set ::WHEN($id) [list $key $pred $cb]
    if {[info exists ::DP($key)] && [when_true $pred $::DP($key)]} {   ;# level check
        unset -nocomplain ::WHEN($id); {*}$cb $key $::DP($key)
    }
    return $id
}
proc dservWhenCancel {id} { unset -nocomplain ::WHEN($id) }
proc dservAfter {ms script} { set id [incr ::seq]; set ::AFTER($id) $script; return $id }
proc dservAfterCancel {id} { unset -nocomplain ::AFTER($id) }
proc fire_timers {} {   ;# manual: fire every pending timer
    foreach id [array names ::AFTER] { set s $::AFTER($id); unset ::AFTER($id); {*}$s }
}
proc puts_capture {args} {}

# --- the code under test ---------------------------------------------------
set here [file dirname [file normalize [info script]]]
set src [file join $here .. extioconf.tcl]
set fh [open $src]; set all [read $fh]; close $fh
set a [string first "# REQUEST/RESPONSE" $all]
set b [string first "\nproc extio_wire_common" $all]
if {$a < 0 || $b < 0} { puts "cannot locate the lifecycle block in $src"; exit 1 }
eval [string range $all $a $b]
proc extio_ota_push_shelf {box channel {version ""}} { return "mock push" }
proc extio_ota_cancel {a b} {}

# --- a mock box that answers commands --------------------------------------
proc box_reply {box} {
    set B extio/$box
    if {[dservExists $B/cmd/ota/verify]} {
        dservClear $B/cmd/ota/verify
        dservSet $B/state/ota/hdr_ok 1; dservSet $B/state/ota/staged_ver "0.4.0+3"
        dservSet $B/state/ota/flash_verify 1
    } elseif {[dservExists $B/cmd/ota/arm]} {
        dservClear $B/cmd/ota/arm
        dservSet $B/state/ota/arm armed
    } elseif {[dservExists $B/cmd/ota/confirm]} {
        dservClear $B/cmd/ota/confirm
        dservSet $B/state/ota/confirm confirmed
    }
}
proc phase {box} { return $::extio_ota_lc($box,phase) }
proc check {label got want} {
    if {$got eq $want} { puts "  ok   $label = $got" } else { puts "  FAIL $label = $got (want $want)" ; incr ::fails }
}
proc says {label text pattern} { check $label [string match $pattern $text] 1 }
set ::fails 0

puts "== happy path =="
extio_ota_update box01 dev
check "after start" [phase box01] staging
dservSet extio/box01/state/ota/state ok      ;# transfer completes
check "-> verifying" [phase box01] verifying
box_reply box01                               ;# verify answers
check "-> arming" [phase box01] arming
box_reply box01                               ;# arm answers
check "-> trial" [phase box01] trial
dservSet extio/box01/state/fw_ver "0.4.0+3"  ;# box reboots + announces
check "-> confirming" [phase box01] confirming
box_reply box01                               ;# confirm answers
check "-> done" [phase box01] done

puts "== stale reply must NOT short-circuit (the retained-value trap) =="
# state/ota/arm is still "armed" and fw_ver still set from the run above.
extio_ota_update box01 dev
check "restart is staging, not arming" [phase box01] staging
check "stale arm cleared" [dservExists extio/box01/state/ota/arm] 0

puts "== transfer failure =="
dservSet extio/box01/state/ota/result sha_mismatch
dservSet extio/box01/state/ota/state fail
check "-> failed" [phase box01] failed

puts "== arm refused =="
extio_ota_update box02 dev
dservSet extio/box02/state/ota/state ok
dservSet extio/box02/state/ota/hdr_ok 1; dservSet extio/box02/state/ota/staged_ver "0.4.0+3"
dservSet extio/box02/state/ota/flash_verify 1
check "-> arming" [phase box02] arming
dservSet extio/box02/state/ota/arm refused_no_verified_image
check "-> failed" [phase box02] failed

puts "== timeout path =="
extio_ota_update box02 dev
dservSet extio/box02/state/ota/state ok
check "-> verifying" [phase box02] verifying
fire_timers                                   ;# box never answers verify
check "-> failed on deadline" [phase box02] failed

puts "== trial booted WRONG version (revert) =="
extio_ota_update box02 dev
dservSet extio/box02/state/ota/state ok
dservSet extio/box02/state/ota/hdr_ok 1; dservSet extio/box02/state/ota/staged_ver "0.4.0+3"
dservSet extio/box02/state/ota/flash_verify 1
dservSet extio/box02/state/ota/arm armed
dservSet extio/box02/state/fw_ver "0.4.0+2"  ;# old image came back
check "-> failed (revert detected)" [phase box02] failed

puts "== a run that died mid-lifecycle must not wedge every later call =="
# Exactly the observed failure: phase stuck mid-lifecycle with NOTHING in
# flight, so the old guard errored before publishing anything -- no
# state/ota/lifecycle, no transfer, reason visible only in the return value.
set ::extio_ota_lc(box03,phase) arming
array unset ::extio_req box03,*
if {[catch {extio_ota_update box03 dev} r]} { puts "  FAIL wedged: $r"; incr ::fails } \
else { check "restarts after a dead run" [phase box03] staging }
# ...but a genuinely live request must still be refused.
set ::extio_ota_lc(box04,phase) arming
set ::extio_req(box04,label) arm
if {[catch {extio_ota_update box04 dev} r]} { puts "  ok   refuses while a request is in flight" } \
else { puts "  FAIL allowed restart over a live request"; incr ::fails }

puts "== arm refuses a read-back that PREDATES the transfer (observed 2026-08-11) =="
# boxa: extio_ota_push_shelf completed a fetch, and flash_verify/hdr_ok/staged_ver
# were still 36 minutes old from the PRIOR update -- staged_ver still naming the
# old version. PRESENT and ==1 both held, so the gate passed a slot nobody had
# read back.
set B extio/box05
dservSet $B/state/ota/hdr_ok 1
dservSet $B/state/ota/staged_ver "0.4.0+70"           ;# the PREVIOUS version
dservSet $B/state/ota/flash_verify 1
incr ::clock [expr {36 * 60 * 1000000}]               ;# 36 minutes pass...
dservSet $B/state/ota/ack 149504                      ;# ...a fresh fetch completes
dservSet $B/state/ota/progress 100
dservSet $B/state/ota/state ok
set r [extio_ota_arm box05]
says  "refused, naming the ordering" $r "*PREDATES the transfer*"
says  "names the stale staged_ver"   $r "*staged_ver=0.4.0+70*"
says  "names both ages"              $r "*flash_verify is 2160s old*"
check "no arm was sent" [dservExists $B/cmd/ota/arm] 0

puts "== ...but the ordinary verify-then-arm sequence still arms =="
# The grace window earns its keep here: on any OTA event the firmware
# re-announces state/result/progress/ack for 6 s (ota_res_until), and the verify
# IS such an event -- so ota/state gets re-stamped AFTER flash_verify landed. A
# strict ordering test would refuse this, the normal manual flow.
set B extio/box06
dservSet $B/state/ota/state ok                        ;# transfer completed
dservSet $B/state/ota/ack 149504
incr ::clock 3000000                                  ;# operator verifies 3 s later
dservSet $B/state/ota/hdr_ok 1; dservSet $B/state/ota/staged_ver "0.4.0+74"
dservSet $B/state/ota/flash_verify 1
incr ::clock 1000000                                  ;# the box's re-announce burst...
dservSet $B/state/ota/state ok                        ;# ...re-stamps these AFTER the verify
dservSet $B/state/ota/ack 149504
set r [extio_ota_arm box06]
says  "not refused" $r "arm sent*"
check "arm was sent" [dservExists $B/cmd/ota/arm] 1

puts "== a transfer clears the previous cycle's evidence, so absence forces a verify =="
set B extio/box07
dservSet $B/state/ota/flash_verify 1; dservSet $B/state/ota/hdr_ok 1
dservSet $B/state/ota/staged_ver "0.4.0+70"
extio_ota_evidence_clear box07                        ;# what every transfer proc now does first
check "flash_verify gone" [dservExists $B/state/ota/flash_verify] 0
check "hdr_ok gone"       [dservExists $B/state/ota/hdr_ok] 0
check "staged_ver gone"   [dservExists $B/state/ota/staged_ver] 0
dservSet $B/state/ota/state ok                        ;# ...then the transfer completes
set r [extio_ota_arm box07]
says  "arm refuses on absence" $r "*NOT PRESENT*"
check "no arm was sent" [dservExists $B/cmd/ota/arm] 0

puts "== a bare verify with no transfer on record is not 'stale' =="
# Nothing to compare against -- refusing here would break arming a box whose
# transfer happened before this interp was reloaded.
set B extio/box08
dservSet $B/state/ota/hdr_ok 1; dservSet $B/state/ota/staged_ver "0.4.0+74"
dservSet $B/state/ota/flash_verify 1
says "not refused" [extio_ota_arm box08] "arm sent*"

puts ""
puts [expr {$::fails == 0 ? "ALL CHECKS PASSED" : "$::fails CHECK(S) FAILED"}]

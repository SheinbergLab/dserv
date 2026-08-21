#
# test_ess_joystick_labels.tcl
#
#  joystick_dir_canon must TOKEN-MATCH the way buttons already do, and
#  joystick_map_for must derive its map from labels rather than falling back
#  to pin order.
#
#  WHY THIS TEST EXISTS. Buttons have always token-matched (button_group_bit
#  splits a label on _ and -, so `btn_left` binds as `left`). The joystick was
#  exact-only, so `joy_up` named no direction -- and labelling a joystick
#  consistently with the buttons beside it is the obvious thing for a person
#  to do. With no canonical label, joystick_map_for falls back to POSITIONAL
#  bits 0-3 in ascending pin order, which on a real box (joy_down/joy_up/
#  joy_right/joy_left on pins 2,3,4,6) reads as a clean up<->down AND
#  left<->right flip. The subject's every response is mirrored and the decode
#  looks blameless. That cost one rig session and nearly cost a second.
#
#  The flip is what makes it worth pinning: it is symmetric, so it does not
#  look like a wiring fault, and the fallback is silent apart from one
#  ess_warning nobody reads at the time.
#
#  A UNIT test: dserv is STUBBED, so this runs under plain tclsh with no
#  server, rig or hardware.
#
#  Run as: tclsh tests/test_ess_joystick_labels.tcl   (or: ctest -R ess_joystick_labels)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

# --- stubs -------------------------------------------------------------
rename package __real_package
proc package { sub args } {
    switch -- $sub { require { return 1.0 } provide { return } \
                     default { return [__real_package $sub {*}$args] } }
}
namespace eval ::settings { proc declare { args } {} ; proc get { args } { error "no settings" } }
proc now {} { return 1000 }
proc dservSet { k v } { set ::DP($k) $v }
proc dservGet { k } { return $::DP($k) }
proc dservExists { k } { return [info exists ::DP($k)] }
foreach p { dservAddExactMatch dservAddMatch dpointAddScript dpointRemoveScript
            dpointSetScript dservRemoveMatch dservKeys } { proc $p { args } {} }
proc do_update {} {}
set ::WARN {}
namespace eval ess {
    proc ess_warning { msg args } { lappend ::WARN $msg }
    proc respwin_active {} { return 1 }
}
source [file join $::REPO lib ess_transports-1.0.tm]

set nfail 0
proc ok { name got want } {
    global nfail
    if { $got eq $want } { puts "ok    $name -> $got" } \
    else { puts "FAIL  $name -> got '$got', want '$want'"; incr nfail }
}

# --- what already worked must keep working -----------------------------
foreach { label want } {
    up up   down down   left left   right right
    UP up   Down down   LEFT left
    u  up   d  down     l  left     r  right
    U  up   D  down
} { ok "exact: '$label'" [::ess::joystick_dir_canon $label] $want }

# --- the fix: tokens, like buttons -------------------------------------
foreach { label want } {
    joy_up up      joy_down down   joy_left left   joy_right right
    btn_up up      resp_left left  hat_right right
    joy-up up      joy-down down
    stick_up_sw up UP_BTN up       Joy_Left left
} { ok "token: '$label'" [::ess::joystick_dir_canon $label] $want }

# --- a lone letter as ONE TOKEN is not a direction ----------------------
# `d_pad` means d-pad, not down. Single letters count only as a whole label,
# which is how they are documented.
foreach label { d_pad d-pad l_stick r_trig u_sw } {
    ok "not a direction: '$label'" [::ess::joystick_dir_canon $label] {}
}
ok "whole-label 'd' still works" [::ess::joystick_dir_canon d] down

# --- genuine non-directions --------------------------------------------
# `downstream` and `update` contain the letters but never become the TOKEN
# `down` or `up`, because the split is on _ and - only.
foreach label { "" select start fire joystick response downstream update } {
    ok "not a direction: '$label'" [::ess::joystick_dir_canon $label] {}
}

# --- the map, on the box that nearly flipped ----------------------------
proc mapfor { pins labels } {
    array unset ::DP ; array set ::DP {}
    set ::DP(extio/box/state/group/joystick/pins) $pins
    foreach p [split $pins ,] l $labels { set ::DP(extio/box/state/label/$p) $l }
    array unset ::ess::joystick_maps ; array set ::ess::joystick_maps {}
    set ::WARN {}
    return [::ess::joystick_map_for extio/box/state/group/joystick]
}
set FALLBACK {up 0 down 1 left 2 right 3}

ok "joy_* labels derive from LABELS" \
    [mapfor 2,3,4,6 {joy_down joy_up joy_right joy_left}] \
    {down 0 up 1 right 2 left 3}
ok "  and are NOT the positional fallback" \
    [expr {[mapfor 2,3,4,6 {joy_down joy_up joy_right joy_left}] eq $FALLBACK}] 0
ok "  silently"  [llength $::WARN] 0
ok "bare labels unchanged" \
    [mapfor 2,3,4,6 {down up right left}] {down 0 up 1 right 2 left 3}

# --- the fallback still exists for genuinely unlabelled groups ----------
ok "no direction labels -> fallback" [mapfor 2,3,4,6 {select start fire mode}] $FALLBACK
ok "  and it warns"                  [expr {[llength $::WARN] > 0}] 1

# --- two members naming one direction: say so, do not pick silently -----
set m [mapfor 2,3,4,6 {joy_up up_sw left right}]
ok "duplicate direction keeps the FIRST" [dict get $m up] 0
ok "  and warns"                         [expr {[llength $::WARN] > 0}] 1

# --- partial pads still work -------------------------------------------
ok "2-way pad" [mapfor 4,6 {joy_left joy_right}]        {left 0 right 1}
ok "3-way pad" [mapfor 2,3,4 {joy_up joy_down joy_left}] {up 0 down 1 left 2}


# --- a relabel must INVALIDATE the derived maps -------------------------
#
# button_group_map and joystick_map_for both cache label->bit maps. Nothing
# cleared them on a label change, so after relabelling a box ess/inputs/*
# kept answering from the OLD labels -- and answered `unresolved` for a rig
# that was fine, while joystick_map_for (cleared by joystick_init) already
# had the right map. Two caches disagreeing about one box.
proc setlabels { pins labels } {
    set ::DP(extio/box/state/group/joystick/pins) $pins
    foreach p [split $pins ,] l $labels { set ::DP(extio/box/state/label/$p) $l }
}
array unset ::DP ; array set ::DP {}
array unset ::ess::joystick_maps       ; array set ::ess::joystick_maps {}
array unset ::ess::button_group_maps   ; array set ::ess::button_group_maps {}
array unset ::ess::button_group_warned ; array set ::ess::button_group_warned {}

# Label it one way and read, which populates both caches. The two labellings
# below must produce DIFFERENT maps or this test proves nothing -- so they are
# deliberately inverted (a relabel from joy_down->joy_up etc. would now
# canonicalize identically and the staleness would be invisible).
setlabels 2,3,4,6 {joy_down joy_up joy_right joy_left}
::ess::joystick_map_for extio/box/state/group/joystick
::ess::button_group_map extio/box/state/group/joystick
ok "joystick map cached"     [expr {[array size ::ess::joystick_maps] > 0}] 1
ok "button group map cached" [expr {[array size ::ess::button_group_maps] > 0}] 1

# Relabel underneath the caches, INVERTING the pad. A correct read would now
# give {up 0 down 1 left 2 right 3}; the stale cache keeps the old answer, and
# that stale answer is the bug.
setlabels 2,3,4,6 {joy_up joy_down joy_left joy_right}
ok "cache is STALE after a relabel" \
    [::ess::joystick_map_for extio/box/state/group/joystick] {down 0 up 1 right 2 left 3}

# the invalidator clears BOTH, plus the warned set
set ::ess::button_group_warned(x,y) 1
::ess::input_maps_invalidate extio/box/state/label/2 down
ok "invalidate clears joystick maps"  [array size ::ess::joystick_maps] 0
ok "invalidate clears group maps"     [array size ::ess::button_group_maps] 0
ok "invalidate clears warned set"     [array size ::ess::button_group_warned] 0

# and the next lookup rebuilds from the NEW labels -- the INVERTED map
ok "rebuilds from the new labels" \
    [::ess::joystick_map_for extio/box/state/group/joystick] {up 0 down 1 left 2 right 3}

# a label change on a DIFFERENT device still purges (deliberately global)
::ess::button_group_map extio/box/state/group/joystick
::ess::input_maps_invalidate extio/otherbox/state/label/9 left
ok "purge is global, not per-device" [array size ::ess::button_group_maps] 0

ok "the watcher proc exists" [llength [info procs ::ess::input_watch_labels]] 1
ok "publish-all proc exists" [llength [info procs ::ess::button_publish_all]] 1

puts ""
if { $nfail } { puts "$nfail FAILURE(S)"; exit 1 }
puts "all checks passed"

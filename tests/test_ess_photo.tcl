# test_ess_photo.tcl -- the gear's `photo` section reaches the processor.
#
# config/essconf.tcl declares the section; lib/ess-2.0.tm (BEGIN photo)
# pushes each key to dserv's `photo` processor (processors/photo.c) and
# logs proc/photo/edge in each data file while the rig has it on. The
# processor keeps nothing across a restart, so these pushes are the only
# way a rig's settings reach it.
#
#   tclsh tests/test_ess_photo.tcl
#
# The block and the declarations are read out of the two files (the whole
# module needs a dserv); processSetParam, the logger and the settings
# store are stand-ins.

set here [file dirname [file normalize [info script]]]
set root [file dirname $here]

set fails 0
proc check { got want what } {
    if { $got eq $want } { puts "  ok: $what" } else {
        puts "  FAIL: $what: got {$got} want {$want}"
        incr ::fails
    }
}
proc slurp { f } { set fd [open $f] ; set s [read $fd] ; close $fd ; return $s }

# the processor: records each param, answers like dserv (0, or -1 when
# there is no such processor)
set ::sent {}
set ::psp 0
proc processSetParam { name p v } {
    lappend ::sent [list $name $p $v]
    return $::psp
}
set ::matches {}
proc dservLoggerAddMatch { args } { lappend ::matches $args }
namespace eval ::ess {
    variable warned {}
    proc ess_warning { msg cat } { variable warned ; lappend warned $msg }
}

# ---- the ess block ----
set src [slurp [file join $root lib ess-2.0.tm]]
set b [string first "    # BEGIN photo\n" $src]
set e [string first "    # END photo\n" $src]
if { $b < 0 || $e < 0 } { error "no photo block in ess-2.0.tm" }
namespace eval ::ess [string range $src $b $e]

puts "pushes:"
set ::sent {}
::ess::set_photo_group " box01/photodiode "
::ess::set_photo_strobe yes
::ess::set_photo_period_ms 8.3333
::ess::set_photo_smooth_ms 2
::ess::set_photo_min_step 150
check $::sent {{photo group box01/photodiode} {photo strobe 1} {photo period 0.0083333} {photo smooth 0.002} {photo min_step 150}} \
    "each key, ms as seconds, group trimmed"

puts "validation:"
check [catch { ::ess::photo_group_norm "a b" }] 1 "a group with a space is refused"
check [catch { ::ess::photo_group_norm "a/b/c" }] 1 "two slashes refused"
check [catch { ::ess::photo_group_norm "extio/*" }] 1 "a pattern refused"
check [::ess::photo_group_norm ""] "" "empty allowed (every block)"
check [catch { ::ess::photo_nonneg_norm -1 }] 1 "a negative period refused"
check [catch { ::ess::photo_nonneg_norm abc }] 1 "a word refused"

puts "enabled:"
set ::sent {}
::ess::set_photo_enabled 1
check $::sent {{photo active 1}} "active 1"
check $::ess::warned {} "no warning where the processor exists"
set ::psp -1
::ess::set_photo_enabled 1
::ess::set_photo_enabled 1
check [llength $::ess::warned] 1 "no processor: warned once"
::ess::set_photo_enabled 0
check [llength $::ess::warned] 1 "turning it off does not warn"
set ::psp 0

puts "data file:"
set ::matches {}
::ess::set_photo_enabled 0
check [::ess::photo_log_match f.ess] 0 "off: not logged"
::ess::set_photo_enabled 1
check [::ess::photo_log_match f.ess] 1 "on: logged"
check $::matches {{f.ess proc/photo/edge 0 0 1}} "every edge, not obs-limited, unbuffered"

puts "stim2 patch:"
# rmtSend: the script is run in a stand-in stim2, with or without the module
set ::rmt {}
set ::stim [interp create]
proc rmtSend { script } { lappend ::rmt $script ; $::stim eval $script }
$::stim eval {
    set calls {}
    proc photopatch { args } { lappend ::calls $args ; expr {[lindex $args 0] eq "on" ? 1 : {}} }
}
set ::ess::warned {}
::ess::set_photo_enabled 0
::ess::set_photo_patch 1
check [::ess::stim_photopatch_enable] 0 "photo off: the patch is not drawn"
check $::rmt {} "  nothing sent"
::ess::set_photo_enabled 1
::ess::set_photo_patch 0
check [::ess::stim_photopatch_enable] 0 "patch off: not drawn"
::ess::set_photo_patch 1
::ess::set_photo_patch_mode toggle
::ess::set_photo_patch_corner bl
::ess::set_photo_patch_size " 40 "
::ess::set_photo_patch_inset 5
::ess::set_photo_patch_dark {0.1 0.1 0.2}
::ess::set_photo_patch_match {{SAMPLE *} PATTERN*}
check [::ess::stim_photopatch_enable] 1 "both on: drawn"
check [$::stim eval { set calls }] \
    {{set -mode toggle -corner bl -size 40 -inset 5 -dark {0.1 0.1 0.2} -match {{SAMPLE *} PATTERN*}} on} \
    "  its settings, then on"
check [llength $::rmt] 1 "  one rmtSend, so the data file records it"
check $::ess::warned {} "  no warning"
$::stim eval { rename photopatch {} }
check [::ess::stim_photopatch_enable] -1 "a stim2 without the module: -1"
check [llength $::ess::warned] 1 "  and a warning"
interp delete $::stim
check [catch { ::ess::photo_patch_size_norm 0 }] 1 "size 0 refused"
check [catch { ::ess::photo_patch_inset_norm -1 }] 1 "inset -1 refused"
check [catch { ::ess::photo_patch_dark_norm "0 1" }] 1 "two values for dark refused"
check [catch { ::ess::photo_patch_dark_norm 1.5 }] 1 "dark past 1 refused"
check [::ess::photo_patch_dark_norm " 0.1 "] 0.1 "one grey"
check [catch { ::ess::photo_patch_match_norm "" }] 1 "no patterns refused"
set ::ess::photo_enabled 0
set ::ess::photo_patch 0

# ---- the declarations ----
puts "gear section (essconf.tcl):"
set conf [slurp [file join $root config essconf.tcl]]
set b [string first "settings::declare photo enabled" $conf]
set e [string first "::ess::set_photo_enabled    \[settings::get photo enabled\]" $conf]
if { $b < 0 || $e < 0 } { error "no photo section in essconf.tcl" }
set e [string first "\n" $conf $e]

namespace eval ::settings {
    variable decl [dict create]
    proc declare { sub key args } {
        variable decl
        dict set decl $sub/$key $args
    }
    proc get { sub key } {
        variable decl
        dict get [dict get $decl $sub/$key] -default
    }
}
set ::sent {}
eval [string range $conf $b $e]

set keys {}
dict for { k opts } $::settings::decl { lappend keys $k }
check $keys [concat photo/enabled photo/group photo/strobe photo/period_ms photo/min_step photo/smooth_ms \
    photo/patch photo/patch_mode photo/patch_corner photo/patch_size photo/patch_inset \
    photo/patch_dark photo/patch_match] "thirteen keys"
check [list $::ess::photo_patch $::ess::photo_patch_opts] \
    {0 {-mode tags -corner tr -size 60 -inset 0 -dark 0 -match *}} \
    "boot: the patch off, its defaults as stim2's"
dict for { k opts } $::settings::decl {
    set ap [dict get $opts -apply]
    check [llength [info commands $ap]] 1 "$k applies through $ap"
    if { [dict exists $opts -validate] } {
        set v [dict get $opts -validate]
        check [catch { $v [dict get $opts -default] }] 0 "$k: its default passes $v"
    }
    check [dict exists $opts -doc] 1 "$k is documented"
}
check [lindex $::sent end] {photo active 0} "boot: enabled pushed last, off by default"
check [lsearch -inline -index 1 $::sent group] {photo group photodiode} \
    "boot: group photodiode"

if { $fails } {
    puts "$fails FAILURE(S)"
    exit 1
}
puts "all checks passed"

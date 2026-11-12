#!/usr/bin/env dlsh
#
# NAME
#   dfconvert.tcl
#
# DESCRIPTION
#   Convert .ess datafiles by hand, as dserv's df subprocess does when a
#   file closes (config/dfconf.tcl): FILE.ess -> FILE.obs.dgz -> FILE.trials.dgz,
#   through the same df::make_obs and df::make_trials (lib/df-2.0.tm).  For
#   files df converted badly (with wide_datafiles off, so stim/frame and the
#   other 64-bit datapoints lost their values), after an extractor changed,
#   or on a machine with no dserv.  See docs/datafiles.md.
#
# USAGE
#   dlsh tools/dfconvert/dfconvert.tcl ?options? FILE.ess ...
#
#     -obs DIR        where the obs DGs go (required unless -trials-only)
#     -trials DIR     also extract trials DGs, into DIR
#     -trials-only    extract trials from the obs DGs already in -obs DIR
#                     (with -trials; the .ess is still read by an extractor
#                     without extract_trials_from_obs)
#     -ess-root DIR   the ESS systems for the extractors: <system_path>/<project>
#                     (default ~/systems/ess, as on a dev machine)
#     -narrow         convert with dslog::wideTypes 0, as df does with its
#                     wide_datafiles setting off (only to reproduce that)
#     -lib DIR        the dserv lib with df-2.0.tm (default: this checkout's
#                     lib/, else /usr/local/dserv/lib)
#
#   Existing files are replaced, each only once its new one is written.
#   Each line says what was made, with the obs DG's stim/frame record count
#   (0 from a narrow conversion) and the trials count.  Exit status 1 when
#   any file failed.
#
# EXAMPLE
#   dlsh tools/dfconvert/dfconvert.tcl -obs ~/data/obs -trials ~/data/trials \
#       data/essdat/human_planko_dropHazard_*_FIX_*.ess
#

proc usage {{msg ""}} {
    if {$msg ne ""} { puts stderr "dfconvert: $msg" }
    puts stderr "usage: dlsh dfconvert.tcl ?-obs DIR? ?-trials DIR? ?-trials-only? ?-ess-root DIR? ?-narrow? ?-lib DIR? FILE.ess ..."
    exit 2
}

set here [file dirname [file normalize [info script]]]
set home [expr {[info exists ::env(HOME)] ? $::env(HOME) : ""}]
array set opt {obs "" trials "" trials_only 0 ess_root "" narrow 0 lib ""}
set files {}
for {set i 0} {$i < [llength $argv]} {incr i} {
    set a [lindex $argv $i]
    switch -- $a {
        -obs - -trials - -ess-root - -lib {
            if {$i + 1 >= [llength $argv]} { usage "$a needs a value" }
            set opt([string map {- _} [string range $a 1 end]]) [file normalize [lindex $argv [incr i]]]
        }
        -trials-only { set opt(trials_only) 1 }
        -narrow      { set opt(narrow) 1 }
        -h - -help - --help { usage }
        default {
            if {[string index $a 0] eq "-"} { usage "unknown option $a" }
            lappend files [file normalize $a]
        }
    }
}
if {![llength $files]} { usage "no .ess files" }
if {$opt(obs) eq ""} { usage "-obs DIR is required (the trials stage reads the obs DG too)" }
if {$opt(trials_only) && $opt(trials) eq ""} { usage "-trials-only wants -trials DIR" }

# df-2.0.tm: this checkout's, else the installed one
if {$opt(lib) eq ""} {
    set opt(lib) [file normalize [file join $here .. .. lib]]
    if {![file exists [file join $opt(lib) df-2.0.tm]]} { set opt(lib) /usr/local/dserv/lib }
}
if {![file exists [file join $opt(lib) df-2.0.tm]]} { usage "no df-2.0.tm in $opt(lib) (-lib DIR)" }
::tcl::tm::path add $opt(lib)
package require dslog
package require em
package require df 2.0
if {[info commands ::dslog::wideTypes] eq ""} {
    if {!$opt(narrow)} { puts stderr "dfconvert: this dlsh has no dslog::wideTypes (dlsh 0.17 or later): it can only convert narrow"; exit 1 }
} else {
    dslog::wideTypes [expr {!$opt(narrow)}]
}
if {$opt(trials) ne ""} {
    if {$opt(ess_root) eq "" && $home ne "" && [file isdirectory $home/systems/ess]} { set opt(ess_root) $home/systems/ess }
    if {$opt(ess_root) eq ""} { usage "-ess-root DIR: the ESS systems for the extractors" }
    df::set_ess_root $opt(ess_root)
    puts "extractors from $opt(ess_root)"
}

# stim/frame records in an obs DG: none from a narrow conversion
proc frame_count {obs} {
    set g [dg_read $obs]
    set n 0
    if {[dl_exists $g:<dsn>stim/frame]} { set n [dl_length [dl_unpack $g:<dsn>stim/frame]] }
    if {[dl_exists $g:<ds>stim/frame] && [dl_length [dl_unpack $g:<ds>stim/frame]] == 0} { set n 0 }
    dg_delete $g
    return $n
}

set failed 0
foreach ess $files {
    set base [regsub {\.ess$} [file tail $ess] {}]
    set obs [file join $opt(obs) $base.obs.dgz]
    set line [format "%-50s" $base]
    set t0 [clock milliseconds]
    if {[catch {
        if {!$opt(trials_only)} {
            if {![file isfile $ess]} { error "no such file $ess" }
            set n_obs [df::make_obs $ess $obs]
            append line [format " obs %4d periods, %6d frames" $n_obs [frame_count $obs]]
        } elseif {![file isfile $obs]} {
            error "no $obs to extract from"
        }
        if {$opt(trials) ne ""} {
            set r [df::make_trials $obs [file join $opt(trials) $base.trials.dgz] -ess $ess]
            append line [format " trials %4d (from %s)" [dict get $r n_trials] [dict get $r from]]
        }
        append line [format "  %d ms" [expr {[clock milliseconds] - $t0}]]
    } err]} {
        puts "FAIL $line $err"
        incr failed
    } else {
        puts "ok   $line"
    }
    flush stdout
}
puts "[expr {[llength $files] - $failed}] of [llength $files] converted[expr {$opt(narrow) ? " (narrow)" : ""}]"
exit [expr {$failed ? 1 : 0}]

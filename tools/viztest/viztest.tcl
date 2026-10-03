#
# NAME
#   viztest.tcl
#
# DESCRIPTION
#   Run an ESS protocol's viz config (set_viz_config) OFFLINE, in a plain
#   tclsh, against a stimdg, and see what it draws -- without a rig, a
#   GUI, or touching the running dserv.
#
#   The config is evaluated the way config/vizconf.tcl evaluates it
#   (namespace eval ::viz::<system>), with the REAL ::viz::setup_window /
#   text_size / set_base_font lifted from vizconf.tcl, and the REAL event
#   id tables built from lib/ess-2.0.tm's evt_info. What is stubbed is
#   only the plumbing: evtSetScript / vizSubscribe record handlers so this
#   script can fire them, dservGet reads a local table, and flushwin
#   captures `dumpwin json` -- the exact payload GraphicsRenderer draws.
#
# USAGE
#   tclsh9.0 viztest.tcl ?options? <protocol.tcl | live>
#
#     <protocol.tcl>      a protocol file containing `$s set_viz_config {...}`;
#                         the system name is taken from its path
#                         (<project>/<system>/<protocol>/<protocol>.tcl)
#     live                the config the running ess published
#                         (ess/viz_config), for the loaded system
#
#     -stimdg live        fetch the running ess's stimdg (read-only)
#     -stimdg <file>      dg_read a saved stimdg (e.g. from dg_write)
#     -variant <name>     build the stimdg with the protocol's REAL loader
#                         for that variant, offline, via dlsh's ess_test
#                         (ess_test::run_variant)
#                         default: live when the target is `live`, else none
#                         -- a scenario then has to build one
#     -live               also take event tables, ess/params and screen
#                         extents from the running ess (implied by `live`)
#     -host <addr>        essctrl server for -live/live (default localhost)
#     -system <name>      override the system namespace name
#     -screen "hx hy"     screen half-extents in deg when not -live
#                         (default "24.8 14.2", a 16:9 rig display)
#     -scenario <file>    a Tcl script that drives the viz (see HELPERS);
#                         without one, a smoke test runs every handler once
#     -html <out.html>    write a page that renders every snapshot with
#                         www/js/GraphicsRenderer.js (the panel's own
#                         renderer); open it in any browser
#     -json <out.json>    write the last frame
#     -v                  print a summary line per snapshot
#
#   Exit status is non-zero if setup or a scenario step threw, a `check`
#   failed, or the config logged an error (e.g. an unknown event name).
#   In the smoke test a handler error is only a WARN: every event gets
#   data 0, which a handler expecting a real payload will reject.
#
# HELPERS (available to a -scenario script)
#   fire TYPE SUBTYPE ?data?   dispatch an event to the config's handlers,
#                              by name (TARGET ON) or id (33 1); subtype
#                              handlers registered as * / -1 match too
#   dpoint NAME DATA           deliver a datapoint update to vizSubscribe'd
#                              handlers (e.g. dpoint ess/dial/pointer 1,2,1,0)
#   setdp NAME VALUE           set what the config's dservGet returns
#   zoom Z                     set ess/viz/zoom (the operator's control --
#                              a MULTIPLIER on the config's own zoom, so
#                              1.0 is no change) and call the config's
#                              `redraw`, as ::viz::on_display_control does
#   snap LABEL                 record the current frame as a snapshot
#   frame                      the last frame (JSON string)
#   summary ?frame?            compact "cmd args; ..." list of the shapes
#                              and text in a frame, pixel coordinates
#   check EXPR MSG             assertion; counted in the exit status
#   handlers                   the registered event handlers
#   vns                        the config's namespace (::viz::<system>),
#                              e.g. `set [vns]::outcome` to inspect state
#
#   Every fire/dpoint/zoom also snapshots automatically if it flushed.
#
# EXAMPLES
#   # smoke test a protocol against the stimdg ess has loaded right now
#   tclsh9.0 viztest.tcl -stimdg live ~/systems/ess/joystick/motiondir/motiondir.tcl
#
#   # ... or against the stimdg a variant's real loader builds, no rig at all
#   tclsh9.0 viztest.tcl -variant left_right ~/systems/ess/joystick/motiondir/motiondir.tcl
#
#   # the config + stimdg actually running, rendered to a page
#   tclsh9.0 viztest.tcl -html /tmp/viz.html live
#
#   # a scripted trial (see examples/)
#   tclsh9.0 viztest.tcl -scenario examples/motiondir_trial.tcl \
#       -html /tmp/motiondir.html ~/systems/ess/joystick/motiondir/motiondir.tcl
#

namespace eval ::viztest {
    variable here [file dirname [file normalize [info script]]]
    variable opts [dict create stimdg "" variant "" live 0 host localhost system "" \
                       screen {24.8 14.2} scenario "" html "" json "" verbose 0]
    variable target ""
    variable handlers {}       ;# list of {type subtype script}
    variable subs [dict create];# dpoint -> list of scripts
    variable dps  [dict create];# what dservGet returns
    variable frames 0
    variable last ""
    variable snaps {}          ;# list of {label json}
    variable last_snapped -1
    variable errors 0
    variable warnings 0
    variable smoking 0         ;# smoke test: handlers get a fake payload
    variable checks 0
    variable failed 0
    variable evt_type_ids [dict create]
    variable evt_subtype_ids [dict create]
    variable vns ""
}

proc ::viztest::usage { {msg ""} } {
    if { $msg ne "" } { puts stderr "viztest: $msg" }
    puts stderr "usage: tclsh9.0 viztest.tcl ?-stimdg live|file? ?-variant v? ?-live? ?-host h?\
                 ?-system s? ?-screen \"hx hy\"? ?-scenario f? ?-html f? ?-json f?\
                 ?-v? <protocol.tcl | live>"
    exit 2
}

proc ::viztest::parse_args { argv } {
    variable opts; variable target
    while { [llength $argv] } {
        set argv [lassign $argv a]
        switch -- $a {
            -stimdg   { set argv [lassign $argv v]; dict set opts stimdg $v }
            -variant  { set argv [lassign $argv v]; dict set opts variant $v }
            -live     { dict set opts live 1 }
            -host     { set argv [lassign $argv v]; dict set opts host $v }
            -system   { set argv [lassign $argv v]; dict set opts system $v }
            -screen   { set argv [lassign $argv v]; dict set opts screen $v }
            -scenario { set argv [lassign $argv v]; dict set opts scenario $v }
            -html     { set argv [lassign $argv v]; dict set opts html $v }
            -json     { set argv [lassign $argv v]; dict set opts json $v }
            -v        { dict set opts verbose 1 }
            -h - -help - --help { usage }
            default {
                if { [string index $a 0] eq "-" } { usage "unknown option $a" }
                if { $target ne "" } { usage "more than one target" }
                set target $a
            }
        }
    }
    if { $target eq "" } { usage }
    if { $target eq "live" } {
        dict set opts live 1
        if { [dict get $opts stimdg] eq "" } { dict set opts stimdg live }
    }
}

######################################################################
#                       Locating dserv / dlsh                        #
######################################################################

# Prefer the checkout this script lives in, then the installed tree.
proc ::viztest::dserv_file { rel } {
    variable here
    foreach base [list [file normalize [file join $here .. ..]] /usr/local/dserv] {
        set f [file join $base $rel]
        if { [file exists $f] } { return $f }
    }
    error "viztest: cannot find $rel in the dserv checkout or /usr/local/dserv"
}

# dlsh ships as a zip next to the dserv install (see config/dsconf.tcl).
proc ::viztest::load_dlsh {} {
    if { ![catch { package require dlsh }] } return
    set zips {}
    if { [info exists ::env(DLSH_ZIP)] } { lappend zips $::env(DLSH_ZIP) }
    lappend zips /usr/local/dlsh/dlsh.zip
    foreach z $zips {
        if { ![file exists $z] } continue
        set base [file join [zipfs root] dlsh]
        if { [catch { zipfs mount $z $base }] } continue
        set ::auto_path [linsert $::auto_path 0 $base/lib]
        package require dlsh
        return
    }
    error "viztest: no dlsh -- set DLSH_ZIP to dlsh.zip"
}

######################################################################
#                          Talking to ess                            #
######################################################################

proc ::viztest::ess { script } {
    variable opts
    return [exec essctrl [dict get $opts host] -s ess -c $script]
}

# A dyngroup datapoint is binary; base64 it on the far side so it
# survives the text transport, rebuild it here, and make sure it is
# called stimdg (the name every viz config reads).
proc ::viztest::stimdg_from_live {} {
    set b [ess {binary encode base64 [dservGet stimdg]}]
    if { [dg_exists stimdg] } { dg_delete stimdg }
    set g [dg_fromString [binary decode base64 [string trim $b]]]
    if { $g ne "stimdg" } { dg_rename $g stimdg }
}

# The protocol's own loader, run headless by ess_test exactly as ESS
# would for the variant's defaults. Done BEFORE install_stubs: ess_test
# installs its own dserv stubs, which ours then replace.
proc ::viztest::stimdg_from_variant { project system protocol variant } {
    package require ess_test
    ess_test::config -systems_root $project
    ess_test::load_loaders $system $protocol
    set g [ess_test::run_variant $system $protocol $variant]
    if { $g ne "stimdg" } {
        if { [dg_exists stimdg] } { dg_delete stimdg }
        dg_rename $g stimdg
    }
}

proc ::viztest::stimdg_from_file { f } {
    if { [dg_exists stimdg] } { dg_delete stimdg }
    set g [dg_read $f]
    if { $g ne "stimdg" } { dg_rename $g stimdg }
}

######################################################################
#                Real pieces of vizconf.tcl and ess                  #
######################################################################

# The top-level commands of the first `namespace eval <ns> {` block in
# a file, one complete command per element.
proc ::viztest::block_commands { file opener } {
    set f [open $file]; set src [read $f]; close $f
    set i [string first $opener $src]
    if { $i < 0 } { error "viztest: '$opener' not found in $file" }
    set i [expr {$i + [string length $opener]}]
    set cmds {}
    set cur ""
    foreach line [split [string range $src $i end] \n] {
        if { $cur eq "" && [string trim $line] eq "\}" } break
        append cur $line \n
        if { [info complete $cur] } {
            if { [string trim $cur] ne "" } { lappend cmds $cur }
            set cur ""
        }
    }
    return $cmds
}

# setup_window, text_size, set_base_font and the font variables they
# use, verbatim from vizconf.tcl, so framing/zoom/font behave exactly
# as on the rig. Everything else in ::viz is stubbed below.
proc ::viztest::load_viz_framework {} {
    set keep {setup_window text_size set_base_font}
    set vars {default_fontsize base_fontsize base_fontfamily text_steps}
    foreach c [block_commands [dserv_file config/vizconf.tcl] "namespace eval viz \{"] {
        set c [string trim $c]
        if { [string match "#*" $c] } continue
        set w1 ""; set w2 ""; set w3 ""
        regexp {^(\S+)\s+(\S+)(?:\s+(\S+))?} $c -> w1 w2 w3
        if { ($w1 eq "proc" && $w2 in $keep) ||
             ($w1 eq "variable" && $w2 in $vars) ||
             ($w1 eq "array" && $w2 eq "set" && $w3 in $vars) } {
            namespace eval ::viz $c
        }
    }
    foreach p $keep {
        if { ![llength [info procs ::viz::$p]] } {
            error "viztest: ::viz::$p not found in vizconf.tcl"
        }
    }
}

# Event tables from ess-2.0.tm's evt_info block -- the same definitions
# ess publishes as ess/evt_type_ids / ess/evt_subtype_ids.
proc ::viztest::load_evt_tables_from_source {} {
    variable evt_type_ids; variable evt_subtype_ids
    set f [open [dserv_file lib/ess-2.0.tm]]; set src [read $f]; close $f
    set a [string first "dict set evt_info MAGIC" $src]
    set b [string first "# initialize evt_type_names" $src $a]
    if { $a < 0 || $b < 0 } { error "viztest: evt_info block not found in ess-2.0.tm" }
    namespace eval ::viztest::evt [list set evt_info [dict create]]
    namespace eval ::viztest::evt [string range $src $a [expr {$b-1}]]
    dict for { k v } [set ::viztest::evt::evt_info] {
        dict set evt_type_ids $k [lindex $v 0]
        set st [lindex $v 3]
        if { $st ne "" } {
            dict for { sk sv } $st { dict set evt_subtype_ids $k $sk $sv }
        }
    }
    namespace delete ::viztest::evt
}

######################################################################
#                            Stubs                                   #
######################################################################

proc ::viztest::install_stubs {} {
    namespace eval ::viz {
        proc log { level message { category visualization } } {
            if { $level eq "error" } { incr ::viztest::errors }
            puts stderr "\[viz $level\] $message"
        }
        proc update_display {} { ::viztest::flush }
        proc clear_display {} { clearwin; update_display }
        proc evtSetScriptByName { type_name subtype_name script } {
            set ts [::viztest::resolve $type_name $subtype_name]
            if { $ts eq "" } {
                log error "evtSetScriptByName: unknown event '$type_name $subtype_name'"
                return
            }
            evtSetScript {*}$ts $script
        }
        proc subscribe { dpoint script } {
            dict lappend ::viztest::subs $dpoint $script
        }
    }
    proc ::flushwin {} { ::viz::update_display }
    proc ::evtSetScriptByName { t s script } { ::viz::evtSetScriptByName $t $s $script }
    proc ::vizSubscribe { dpoint script } { ::viz::subscribe $dpoint $script }
    proc ::evtSetScript { type subtype script } {
        lappend ::viztest::handlers [list $type $subtype $script]
    }
    proc ::evtRemoveAllScripts {} { set ::viztest::handlers {} }
    proc ::dservGet { name } {
        if { [dict exists $::viztest::dps $name] } { return [dict get $::viztest::dps $name] }
        error "dservGet: no datapoint $name"
    }
    proc ::dservExists { name } { dict exists $::viztest::dps $name }
    proc ::dservSet { name value } { dict set ::viztest::dps $name $value }
    proc ::dservTouch { name } {}
    proc ::dservAddExactMatch { args } {}
    proc ::dpointSetScript { dpoint script } { dict lappend ::viztest::subs $dpoint $script }
}

# name or id -> {type_id subtype_id}; "" if unknown
proc ::viztest::resolve { type subtype } {
    variable evt_type_ids; variable evt_subtype_ids
    if { [string is integer -strict $type] } {
        set tid $type
        set tname ""
        dict for { k v } $evt_type_ids { if { $v == $tid } { set tname $k; break } }
    } else {
        if { ![dict exists $evt_type_ids $type] } { return "" }
        set tid [dict get $evt_type_ids $type]
        set tname $type
    }
    if { $subtype eq "*" || $subtype eq "-1" } {
        set sid -1
    } elseif { [string is integer -strict $subtype] } {
        set sid $subtype
    } elseif { $tname ne "" && [dict exists $evt_subtype_ids $tname $subtype] } {
        set sid [dict get $evt_subtype_ids $tname $subtype]
    } else {
        return ""
    }
    return [list $tid $sid]
}

# ids -> "TARGET ON", for snapshot labels
proc ::viztest::evt_label { tid sid } {
    variable evt_type_ids; variable evt_subtype_ids
    set tname $tid
    dict for { k v } $evt_type_ids { if { $v == $tid } { set tname $k; break } }
    set sname [expr {$sid == -1 ? "*" : $sid}]
    if { [dict exists $evt_subtype_ids $tname] } {
        dict for { k v } [dict get $evt_subtype_ids $tname] {
            if { $v == $sid } { set sname $k; break }
        }
    }
    return "$tname $sname"
}

proc ::viztest::flush {} {
    variable frames; variable last
    incr frames
    set last [dumpwin json]
}

######################################################################
#                       Scenario helpers                             #
######################################################################

proc ::viztest::run_script { label script } {
    variable errors; variable warnings; variable smoking; variable frames
    set before $frames
    if { [catch { uplevel #0 $script } err] } {
        # The smoke test hands every event data 0, which a handler that
        # unpacks a real payload ({resp correct}, "x y r") rejects -- a
        # finding to look at, not a failure. setup and redraw get no
        # payload, so their errors always count.
        if { $smoking && ![string match {setup*} $label] &&
             ![string match {zoom*} $label] } {
            incr warnings
            puts stderr "WARN  in $label: $err  (smoke payload is 0 --\
                         check with a scenario sending the real data)"
        } else {
            incr errors
            puts stderr "ERROR in $label: $err"
            puts stderr "  [lindex [split $::errorInfo \n] end]"
        }
    }
    if { $frames > $before } { snap $label }
}

proc fire { type subtype { data 0 } } {
    set ts [::viztest::resolve $type $subtype]
    if { $ts eq "" } {
        incr ::viztest::errors
        puts stderr "fire: unknown event '$type $subtype'"
        return
    }
    lassign $ts tid sid
    set label [::viztest::evt_label $tid $sid]
    if { $data ne "" } { append label " ($data)" }
    set n 0
    foreach h $::viztest::handlers {
        lassign $h ht hs script
        if { $ht == $tid && ($hs == -1 || $hs == $sid) } {
            ::viztest::run_script $label [list {*}$script $tid $sid $data]
            incr n
        }
    }
    return $n
}

proc dpoint { name data } {
    dict set ::viztest::dps $name $data
    if { ![dict exists $::viztest::subs $name] } { return 0 }
    foreach script [dict get $::viztest::subs $name] {
        ::viztest::run_script "$name $data" [list {*}$script $name $data]
    }
    return 1
}

proc setdp { name value } { dict set ::viztest::dps $name $value }

proc zoom { z } {
    setdp ess/viz/zoom $z
    set p [vns]::redraw
    if { ![llength [info procs $p]] } {
        puts stderr "zoom: [vns] defines no redraw -- zoom applies at the next draw"
        return 0
    }
    ::viztest::run_script "zoom $z" [list $p]
    return 1
}

proc snap { label } {
    if { $::viztest::frames == $::viztest::last_snapped } {
        # nothing new since the last snapshot: relabel it, don't duplicate
        lset ::viztest::snaps end 0 "[lindex $::viztest::snaps end 0] / $label"
        return
    }
    set ::viztest::last_snapped $::viztest::frames
    lappend ::viztest::snaps [list $label $::viztest::last]
    if { [dict get $::viztest::opts verbose] } {
        set sm [summary]
        if { $sm eq "" } { set sm "(empty)" }
        if { [string length $sm] > 160 } { set sm "[string range $sm 0 156]..." }
        puts "  \[$label\] $sm"
    }
}

proc frame {} { return $::viztest::last }

# Shapes and text only, rounded, in the frame's pixel coordinates.
proc summary { { json "" } } {
    if { $json eq "" } { set json $::viztest::last }
    set out {}
    set re {"cmd":"(fcircle|circle|fpoly|poly|filledrect|line|drawtext)","args":\[([^\]]*)\]}
    foreach { - c a } [regexp -all -inline $re $json] {
        set vals {}
        foreach v [split $a ,] {
            lappend vals [expr {[string is double -strict $v] ? round($v) : $v}]
        }
        lappend out "$c [join $vals { }]"
    }
    return [join $out "; "]
}

proc check { cond msg } {
    incr ::viztest::checks
    if { [uplevel 1 [list expr $cond]] } {
        puts "  ok   $msg"
    } else {
        incr ::viztest::failed
        puts "  FAIL $msg"
    }
}

proc handlers {} { return $::viztest::handlers }
proc vns {} { return $::viztest::vns }

######################################################################
#                         Loading the config                         #
######################################################################

# The body of the first `set_viz_config { ... }` in a protocol file.
proc ::viztest::viz_body_from_file { file } {
    set f [open $file]; set src [read $f]; close $f
    set i [string first "set_viz_config" $src]
    if { $i < 0 } { error "viztest: no set_viz_config in $file" }
    set i [expr {[string first "\{" $src $i] + 1}]
    # Count braces RAW, as Tcl parses a braced word: braces inside
    # comments count too (`info complete` would skip those and stop
    # early), only a backslash escapes one.
    set n [string length $src]
    set depth 1
    for { set j $i } { $j < $n } { incr j } {
        switch -- [string index $src $j] {
            "\\" { incr j }
            "\{"  { incr depth }
            "\}"  {
                if { [incr depth -1] == 0 } {
                    return [string range $src $i [expr {$j-1}]]
                }
            }
        }
    }
    error "viztest: unbalanced set_viz_config in $file"
}

proc ::viztest::smoke {} {
    variable handlers; variable subs; variable dps
    puts "smoke test: every handler once, STIMTYPE first (data = row 0)"
    variable smoking 1
    fire STIMTYPE * 0
    # redraw right after STIMTYPE, while the state is still real -- the
    # fake payloads below can leave it in a shape no trial would
    if { [llength [info procs [vns]::redraw]] } { zoom 1.5; setdp ess/viz/zoom 2.0 }
    set done {}
    foreach h $handlers {
        lassign $h t s script
        if { $t == [dict get $::viztest::evt_type_ids STIMTYPE] } continue
        set key "$t $s"
        if { $key in $done } continue
        lappend done $key
        fire $t [expr {$s == -1 ? 1 : $s}] 0
    }
    dict for { dp scripts } $subs {
        if { [dict exists $dps $dp] } {
            dpoint $dp [dict get $dps $dp]
        } else {
            puts "  (no value for subscribed $dp -- not delivered; use a scenario)"
        }
    }
    set smoking 0
}

proc ::viztest::write_html { out } {
    variable snaps
    set f [open [dserv_file www/js/GraphicsRenderer.js]]; set js [read $f]; close $f
    set items {}
    foreach s $snaps {
        lassign $s label json
        lappend items "{label:[json_str $label],frame:$json}"
    }
    set data "\[[join $items ,]\]"
    set html [string map [list @JS@ $js @DATA@ $data @TITLE@ [json_str $::viztest::target]] {<!doctype html>
<html><head><meta charset="utf-8"><title>viztest</title>
<style>
 body { background:#111; color:#ccc; font:13px Helvetica,Arial,sans-serif; margin:16px; }
 h1 { font-size:15px; font-weight:normal; color:#eee; }
 .grid { display:flex; flex-wrap:wrap; gap:14px; }
 figure { margin:0; } figcaption { padding:4px 2px; max-width:480px; word-break:break-all; }
 canvas { width:480px; height:360px; border:1px solid #333; display:block; }
</style></head><body>
<h1>viztest: <span id="t"></span> &mdash; <span id="n"></span> snapshots</h1>
<div class="grid" id="g"></div>
<script>@JS@</script>
<script>
 const snaps = @DATA@;
 document.getElementById('t').textContent = @TITLE@;
 document.getElementById('n').textContent = snaps.length;
 const g = document.getElementById('g');
 snaps.forEach((s, i) => {
   const fig = document.createElement('figure');
   const c = document.createElement('canvas');
   const cap = document.createElement('figcaption');
   cap.textContent = (i + 1) + '. ' + s.label;
   fig.appendChild(c); fig.appendChild(cap); g.appendChild(fig);
   const r = new GraphicsRenderer(c, { width: 640, height: 480, backgroundColor: '#1a1a2a' });
   r.renderCommands(s.frame);
 });
</script></body></html>
}]
    set f [open $out w]; puts -nonewline $f $html; close $f
}

proc ::viztest::json_str { s } {
    return "\"[string map {\\ \\\\ \" \\\" \n \\n \r \\r \t \\t < \\u003c} $s]\""
}

######################################################################
#                               Main                                 #
######################################################################

proc ::viztest::main { argv } {
    variable opts; variable target; variable dps; variable vns
    parse_args $argv
    load_dlsh
    if { [dict get $opts variant] ne "" } {
        if { $target eq "live" } { usage "-variant needs a protocol file, not live" }
        set pf [file normalize $target]
        set pd [file dirname $pf]
        stimdg_from_variant [file dirname [file dirname $pd]] \
            [file tail [file dirname $pd]] [file tail $pd] [dict get $opts variant]
    }
    load_viz_framework
    install_stubs

    # event tables: the running ess's if asked (it includes any events a
    # system adds), otherwise the definitions in ess-2.0.tm
    if { [dict get $opts live] } {
        set ::viztest::evt_type_ids    [ess {dservGet ess/evt_type_ids}]
        set ::viztest::evt_subtype_ids [ess {dservGet ess/evt_subtype_ids}]
        foreach dp {ess/params ess/screen_halfx ess/screen_halfy ess/viz/zoom ess/viz/fontsize} {
            if { ![catch { ess "dservGet $dp" } v] && $v ne "" } { dict set dps $dp $v }
        }
    } else {
        load_evt_tables_from_source
        lassign [dict get $opts screen] hx hy
        dict set dps ess/screen_halfx $hx
        dict set dps ess/screen_halfy $hy
    }
    dict set dps ess/evt_type_ids    $::viztest::evt_type_ids
    dict set dps ess/evt_subtype_ids $::viztest::evt_subtype_ids

    # the config and the namespace it runs in
    set system [dict get $opts system]
    if { $target eq "live" } {
        set body [ess {dservGet ess/viz_config}]
        # the same lookups vizconf's on_viz_config_received makes
        if { $system eq "" } { set system [ess {getVar ess ::ess::current(system)}] }
        if { [catch {
            set sp   [ess {getVar ess ::ess::system_path}]
            set proj [ess {getVar ess ::ess::current(project)}]
            ::tcl::tm::add [file join $sp $proj lib]
        } err] } {
            puts stderr "viztest: could not find the live project lib: $err"
        }
    } else {
        set file [file normalize $target]
        set body [viz_body_from_file $file]
        set protodir [file dirname $file]
        if { $system eq "" } { set system [file tail [file dirname $protodir]] }
        # <project>/lib, as vizconf adds [file join system_path project lib]
        set lib [file join [file dirname [file dirname $protodir]] lib]
        if { [file isdirectory $lib] } { ::tcl::tm::add $lib }
    }
    catch { ::tcl::tm::add [file join [file dirname [dserv_file config/vizconf.tcl]] .. lib] }
    set vns ::viz::$system

    # the stimdg
    switch -- [dict get $opts stimdg] {
        ""      {}
        live    { stimdg_from_live }
        default { stimdg_from_file [dict get $opts stimdg] }
    }
    if { [dg_exists stimdg] } {
        puts "stimdg: [dl_length stimdg:[lindex [dg_tclListnames stimdg] 0]] rows,\
              [llength [dg_tclListnames stimdg]] columns"
    }

    puts "config: $target -> $vns"
    run_script "setup" [list namespace eval $vns $body]
    puts "handlers: [llength $::viztest::handlers] events,\
          datapoints: [dict keys $::viztest::subs],\
          redraw: [expr {[llength [info procs ${vns}::redraw]] ? "yes" : "no"}]"

    if { [dict get $opts scenario] ne "" } {
        if { [catch { uplevel #0 [list source [dict get $opts scenario]] } err] } {
            incr ::viztest::errors
            puts stderr "ERROR in scenario: $err\n$::errorInfo"
        }
    } elseif { [dg_exists stimdg] } {
        smoke
    } else {
        puts "no stimdg and no scenario: only setup was run (try -stimdg live)"
    }

    puts "frames: $::viztest::frames, snapshots: [llength $::viztest::snaps],\
          errors: $::viztest::errors, warnings: $::viztest::warnings,\
          checks: $::viztest::checks\
          ($::viztest::failed failed)"
    if { [dict get $opts verbose] == 0 && $::viztest::last ne "" } {
        set sm [summary]
        if { $sm eq "" } { set sm "(empty)" }
        if { [string length $sm] > 300 } { set sm "[string range $sm 0 296]..." }
        puts "last frame: $sm"
    }
    if { [dict get $opts json] ne "" } {
        set f [open [dict get $opts json] w]; puts $f $::viztest::last; close $f
    }
    if { [dict get $opts html] ne "" } {
        write_html [dict get $opts html]
        puts "html: [file normalize [dict get $opts html]]"
    }
    exit [expr {($::viztest::errors + $::viztest::failed) > 0}]
}

::viztest::main $argv

# extio_models-1.0.tm -- named box MODELS: what a kind of box should be
#
# A MODEL is a box we build more than one of (the astick: a Teensy 4.0, a
# thumbstick on A5/A4, its push-select on T6). It says three things:
#
#   box   the box's own configuration, in exactly the form extio_cfg_export
#         writes ("<leaf> <value>" lines), so provisioning a unit is a replay
#         through extio_cfg_set and judging one is a diff of its live export
#   rig   the rig settings the device wants ("<sub> <key> <value>" lines,
#         each a Tcl list -- brace a multi-word value), applied through the
#         settings module exactly as the gear would
#   board / match   who gets OFFERED the model: board globs (state/board),
#         and name globs that suggest it for an unconfigured box
#
# The point of the rig half is DISCOVERABILITY. The gear made every knob
# settable; it did not make anyone know which four of them a thumbstick
# needs. A model carries that knowledge with the device instead.
#
# FILES: config/models/*.tcl (shipped) then local/models/*.tcl (this rig's
# own; a same-named model there wins). Declarative, parsed in a SAFE interp
# that exposes one command, so a bad file costs a breadcrumb, never a boot:
#
#     model astick {
#         desc  "Teensy 4.0 analog thumbstick with push-select"
#         board teensy40
#         match astick*
#         box {
#             ain/enable 1
#             ...
#         }
#         rig {
#             slider source extio
#             joystick transport stick
#         }
#     }
#
# WHAT IS COMPARED. Only the device's SHAPE -- pin/, group/, ain/, obs/,
# sync/ leaves -- counts as "extra" on a box, because those are what a model
# describes. A box's name, network, dserv target, channel and debug level are
# about the unit or the rig, not the model, and a model that never mentions
# them must still match. (A leaf a model DOES list is always compared.)
#
# This file is pure Tcl down to the "dserv side" marker below, so the parsing
# and judging are testable in plain tclsh (tests/test_extio_models.tcl).

package provide extio_models 1.0

namespace eval ::extio_models {
    variable models [dict create]   ;# name -> {desc board match box rig file}
    variable errors {}              ;# load breadcrumbs
    variable dirs   {}              ;# search order; later wins
    variable shape_rx {^(pin|group|ain|obs|sync)/}
}

# ---------------------------------------------------------------- parsing

# "<leaf> <value>" lines -> dict. The value is the REST of the line, verbatim,
# because that is what the export writes (a desc keeps its spaces).
proc ::extio_models::parse_box {text} {
    set d [dict create]
    foreach line [split $text \n] {
        set line [string trim $line]
        if { $line eq "" || [string index $line 0] eq "#" } continue
        set sp [string first " " $line]
        if { $sp < 0 } { dict set d $line ""; continue }
        dict set d [string range $line 0 $sp-1] \
            [string trim [string range $line $sp+1 end]]
    }
    return $d
}

# "<sub> <key> <value>" lines -> list of {sub key value}. Each line is a Tcl
# list so `dial sources {ring touch}` keeps its value whole.
proc ::extio_models::parse_rig {text} {
    set out {}
    foreach line [split $text \n] {
        set line [string trim $line]
        if { $line eq "" || [string index $line 0] eq "#" } continue
        if { [catch { llength $line } n] || $n != 3 } {
            error "rig line '$line': want <sub> <key> <value> (brace a\
                   multi-word value)"
        }
        lappend out [lrange $line 0 2]
    }
    return $out
}

proc ::extio_models::_model_cmd {file name body} {
    variable models
    if { ![regexp {^[A-Za-z0-9][A-Za-z0-9._-]*$} $name] } {
        error "model name '$name': letters, digits, . _ - only"
    }
    if { [catch { dict size $body }] } {
        error "model $name: body must be key/value pairs (desc board match box rig)"
    }
    set m [dict create desc "" board {} match {} box {} rig {} file $file]
    dict for {k v} $body {
        switch -exact -- $k {
            desc - board - match { dict set m $k $v }
            box  { dict set m box [parse_box $v] }
            rig  { dict set m rig [parse_rig $v] }
            default { error "model $name: unknown section '$k'\
                             (desc board match box rig)" }
        }
    }
    dict set models $name $m
    return
}

proc ::extio_models::config {args} {
    variable dirs
    foreach {opt val} $args {
        switch -- $opt {
            -dirs   { set dirs $val }
            default { error "extio_models::config: unknown option '$opt'" }
        }
    }
}

# (Re)read every model file. Returns the number of models known.
proc ::extio_models::load {} {
    variable models; variable errors; variable dirs
    set models [dict create]
    set errors {}
    foreach dir $dirs {
        foreach f [lsort [glob -nocomplain -directory $dir *.tcl]] {
            set si [interp create -safe]
            interp alias $si model {} ::extio_models::_model_cmd $f
            if { [catch {
                set fh [open $f r]; set src [read $fh]; close $fh
                $si eval $src
            } e] } {
                lappend errors "[file tail $f]: $e"
            }
            interp delete $si
        }
    }
    return [dict size $models]
}

proc ::extio_models::names {} { variable models; return [lsort [dict keys $models]] }
proc ::extio_models::get {name} {
    variable models
    if { ![dict exists $models $name] } { error "no model '$name' (have: [names])" }
    return [dict get $models $name]
}
proc ::extio_models::errors {} { variable errors; return $errors }

# ---------------------------------------------------------------- judging

proc ::extio_models::shape_leaf {leaf} {
    variable shape_rx
    return [regexp $shape_rx $leaf]
}

# want/have are leaf dicts. Returns {missing {leaf want ...} differ {leaf
# {want have} ...} extra {leaf have ...}}.
proc ::extio_models::box_diff {want have} {
    set missing [dict create]; set differ [dict create]; set extra [dict create]
    dict for {leaf w} $want {
        if { ![dict exists $have $leaf] } { dict set missing $leaf $w; continue }
        set h [dict get $have $leaf]
        if { [string trim $h] ne [string trim $w] } {
            dict set differ $leaf [list $w $h]
        }
    }
    dict for {leaf h} $have {
        if { ![dict exists $want $leaf] && [shape_leaf $leaf] } {
            dict set extra $leaf $h
        }
    }
    return [dict create missing $missing differ $differ extra $extra]
}

proc ::extio_models::diff_count {d} {
    expr {[dict size [dict get $d missing]] + [dict size [dict get $d differ]] \
          + [dict size [dict get $d extra]]}
}

proc ::extio_models::_glob_any {globs s} {
    foreach g $globs { if { [string match -nocase $g $s] } { return 1 } }
    return 0
}

# Rank every board-compatible model against a box. Returns a list of dicts
# {name diff count named} best first: an exact match, then a model whose
# `match` names this box, then fewest differences.
proc ::extio_models::rank {box board have} {
    variable models
    set out {}
    dict for {name m} $models {
        set boards [dict get $m board]
        if { [llength $boards] && ![_glob_any $boards $board] } continue
        set d [box_diff [dict get $m box] $have]
        set n [diff_count $d]
        set named [_glob_any [dict get $m match] $box]
        lappend out [list [expr {$n != 0}] [expr {!$named}] $n $name $d $named]
    }
    # stable sorts, least significant first: name, count, named, exact
    set ranked {}
    foreach r [lsort -index 0 -integer [lsort -index 1 -integer \
                  [lsort -index 2 -integer [lsort -index 3 $out]]]] {
        lassign $r - - n name d named
        lappend ranked [dict create name $name diff $d count $n named $named]
    }
    return $ranked
}

# A box with no shape leaves at all is BLANK: fresh firmware, or a factory
# reset. That, or a name a model claims, is when offering a model is welcome;
# a box someone has configured by hand into something else is left alone.
proc ::extio_models::is_blank {have} {
    dict for {leaf v} $have { if { [shape_leaf $leaf] } { return 0 } }
    return 1
}

# The verdict on one box, given its live config dict. Pure. Returns
#   state      matches | differs | blank | unknown
#   model      the best model (or "")
#   offer      a model worth offering unprompted (or "")
#   diff       box diff against `model`
#   candidates the ranked list (name count named)
proc ::extio_models::verdict {box board have {declared ""}} {
    set ranked [rank $box $board $have]
    set cands {}
    foreach r $ranked {
        lappend cands [dict create name [dict get $r name] \
            count [dict get $r count] named [dict get $r named]]
    }
    set blank [is_blank $have]
    set best ""; set diff {}
    if { $declared ne "" } {
        foreach r $ranked {
            if { [dict get $r name] eq $declared } { set best $r; break }
        }
    }
    if { $best eq "" && [llength $ranked] } { set best [lindex $ranked 0] }

    if { $best eq "" } {
        set state [expr {$blank ? "blank" : "unknown"}]
        return [dict create state $state model "" offer "" diff {} candidates $cands]
    }
    set model [dict get $best name]
    set diff  [dict get $best diff]
    if { [dict get $best count] == 0 } {
        set state matches
    } elseif { $blank } {
        set state blank
    } elseif { $declared eq $model || [dict get $best named] } {
        set state differs
    } else {
        # configured, unclaimed, and not this model: say nothing about it
        set state unknown
    }
    set offer ""
    if { $state in {blank differs} } { set offer $model }
    if { $state eq "unknown" } { set model ""; set diff {} }
    return [dict create state $state model $model offer $offer diff $diff \
                candidates $cands]
}

# Model text for a box's live config: the SNAPSHOT a new model starts from.
proc ::extio_models::snapshot_text {name desc board have} {
    set lines {}
    dict for {leaf v} $have {
        if { [shape_leaf $leaf] } { lappend lines "        $leaf $v" }
    }
    return "# model $name -- snapshot of a live box, [clock format [clock seconds] -format {%Y-%m-%d %H:%M}]
# The box section is the box's own export (shape leaves only). The rig
# section is yours to fill: the settings this device wants on a rig, one
# `<sub> <key> <value>` per line, e.g. `joystick transport stick`.
model $name {
    desc  [list $desc]
    board [list $board]
    match [list ${name}*]
    box {
[join $lines \n]
    }
    rig {
    }
}
"
}

# ====================================================== dserv side (extio interp)

# The rig half reads the SHARED settings tree -- every interp's knobs are
# there -- and writes through the interp that declared each knob (its schema
# names it). Returns a list of dicts {sub key want have source interp ok}.
proc ::extio_models::rig_diff {rig} {
    set out {}
    foreach item $rig {
        lassign $item sub key want
        set dp settings/$sub/$key
        set have ""; set source undeclared; set interp ""
        if { [dservExists $dp] } {
            set have [dservGet $dp]
            catch { set source [dservGet $dp/source] }
            catch { set interp [dict get [dservGet $dp/schema] interp] }
        }
        set ok [expr {$source ne "undeclared" && [_same $want $have]}]
        lappend out [dict create sub $sub key $key want $want have $have \
                         source $source interp $interp ok $ok]
    }
    return $out
}

# Compare as lists, so `{ring touch}` and `ring touch` agree.
proc ::extio_models::_same {a b} {
    if { [catch { expr {[join $a " "] eq [join $b " "]} } r] } { return [expr {$a eq $b}] }
    return $r
}

# Put one knob through its owner. -persist = keep (rig.tcl); without it the
# value is a runtime override that settings::clear undoes.
proc ::extio_models::rig_put {sub key value interp persist} {
    set cmd [list settings::put $sub $key $value]
    if { $persist } { lappend cmd -persist }
    if { $interp eq "" } { error "$sub $key: not declared on this rig" }
    if { $interp eq "dserv" } {
        error "$sub $key is owned by the main interp -- set it from the gear"
    }
    if { $interp eq [_self] } {
        return [uplevel #0 $cmd]
    }
    set r [send $interp $cmd]
    if { [string first "!TCL_ERROR" $r] == 0 } { error [string trim [string range $r 10 end]] }
    return $r
}

proc ::extio_models::rig_clear {sub key interp} {
    set cmd [list settings::clear $sub $key]
    if { $interp eq "" || $interp eq "dserv" } { return }
    if { $interp eq [_self] } { return [uplevel #0 $cmd] }
    set r [send $interp $cmd]
    if { [string first "!TCL_ERROR" $r] == 0 } { error [string trim [string range $r 10 end]] }
    return $r
}

proc ::extio_models::_self {} {
    if { [info exists ::dserv_interp] } { return $::dserv_interp }
    return ""
}

# ---- JSON for pages (yajltcl, the house generator: never make JS parse Tcl)

proc ::extio_models::_json_status {box board st rig} {
    package require yajltcl
    set j [yajl create #auto]
    $j map_open
    $j string box string $box
    $j string board string $board
    foreach k {state model offer} { $j string $k string [dict get $st $k] }
    $j string desc string [expr {[dict get $st model] ne "" ?
        [dict get [get [dict get $st model]] desc] : ""}]

    $j string diff map_open
    set d [dict get $st diff]
    foreach part {missing extra} {
        $j string $part array_open
        if { [dict exists $d $part] } {
            dict for {leaf v} [dict get $d $part] {
                $j map_open string leaf string $leaf string value string $v map_close
            }
        }
        $j array_close
    }
    $j string differ array_open
    if { [dict exists $d differ] } {
        dict for {leaf wh} [dict get $d differ] {
            lassign $wh w h
            $j map_open string leaf string $leaf string want string $w \
                string have string $h map_close
        }
    }
    $j array_close
    $j map_close

    $j string rig array_open
    foreach r $rig {
        $j map_open
        foreach k {sub key want have source interp} { $j string $k string [dict get $r $k] }
        $j string ok bool [dict get $r ok]
        $j map_close
    }
    $j array_close

    $j string candidates array_open
    foreach c [dict get $st candidates] {
        set m [get [dict get $c name]]
        $j map_open string name string [dict get $c name] \
            string desc string [dict get $m desc] \
            string count number [dict get $c count] \
            string named bool [dict get $c named] map_close
    }
    $j array_close
    $j map_close
    set out [$j get]
    $j delete
    return $out
}

proc ::extio_models::publish_catalog {} {
    package require yajltcl
    set j [yajl create #auto]
    $j array_open
    foreach n [names] {
        set m [get $n]
        $j map_open string name string $n string desc string [dict get $m desc] \
            string board string [dict get $m board] \
            string file string [dict get $m file] \
            string box_leaves number [dict size [dict get $m box]] \
            string rig_items number [llength [dict get $m rig]] map_close
    }
    $j array_close
    dservSet extio/models/catalog [$j get]
    $j delete
    if { [llength [errors]] } {
        dservSet extio/models/errors [join [errors] \n]
    } else {
        dservSet extio/models/errors ""
    }
}

# ---- the live loop -------------------------------------------------------
#
# A verdict needs the box's config, and the only honest source of that is the
# box's own export (cmd/dump 2) -- reconstructing it from state/* would be a
# second copy of firmware knowledge. So judging is ASYNC: ask for a dump, judge
# when it lands, publish extio/models/<box> (JSON). The dump rides extio's
# one-request-per-box slot (extio_request); a page export racing it simply
# wins, and the next judge retries.

array set ::extio_models::live {}   ;# box -> last export text

proc ::extio_models::judge {box} {
    if { ![dservExists extio/$box/state/build] } { return "no box '$box'" }
    extio_request $box "model judge" dump 2 cfg/dump_dp {ne ""} 15000 \
        ::extio_models::_judge_done ::extio_models::_judge_failed {cfg/dump_dp cfg/dump_rc}
    return "judging $box"
}

proc ::extio_models::_judge_failed {box why} {
    dservSet extio/models/$box/error "could not read config: $why"
}

proc ::extio_models::_judge_done {box text} {
    variable live
    set live($box) $text
    judge_text $box $text
}

# The model a box DECLARES (meta/model, once firmware carries it) outranks
# any inference.
proc ::extio_models::declared {box} {
    set d ""
    catch { set d [dservGet extio/$box/state/meta/model] }
    return [string trim [lindex [split $d \x00] 0]]
}

proc ::extio_models::judge_text {box text} {
    set board ""
    catch { set board [lindex [split [dservGet extio/$box/state/board] \x00] 0] }
    set have [parse_box $text]
    set st [verdict $box $board $have [declared $box]]
    set rig {}
    if { [dict get $st model] ne "" } {
        set rig [rig_diff [dict get [get [dict get $st model]] rig]]
    }
    dservSet extio/models/$box [_json_status $box $board $st $rig]
    dservSet extio/models/$box/error ""
    return [dict get $st state]
}

# What applying <model> to <box> WOULD change, as the same JSON shape as the
# published verdict -- for a page letting someone pick a model other than the
# one inferred. Uses the last config read; publishes nothing.
proc ::extio_models::preview {box model} {
    variable live
    get $model                                      ;# errors helpfully if unknown
    if { ![info exists live($box)] } { error "no config read from '$box' yet" }
    set board ""
    catch { set board [lindex [split [dservGet extio/$box/state/board] \x00] 0] }
    set st [verdict $box $board [parse_box $live($box)] $model]
    if { [dict get $st model] ne $model } {
        error "model '$model' is for board [dict get [get $model] board], not '$board'"
    }
    set rig [rig_diff [dict get [get $model] rig]]
    return [_json_status $box $board $st $rig]
}

# A box just appeared. Give its announce burst a moment to land first.
proc ::extio_models::on_box {box} {
    dservAfter 2500 [list ::extio_models::judge $box]
}

# ---- acting on a verdict ------------------------------------------------
#
#   apply <box> <model> ?-box? ?-rig? ?-keep?
#
# Default is BOTH halves, as a TRY: box leaves set live (not saved -- a box
# reboot is the undo), rig knobs as runtime overrides (revert undoes). -keep
# makes it stick: the box saves to flash, the knobs persist to rig.tcl.
# Only leaves/knobs that differ are touched. Never removes a box's extra
# pins -- that is a judgment a person makes on the config page.
proc ::extio_models::apply {box model args} {
    set do_box [expr {"-box" in $args}]
    set do_rig [expr {"-rig" in $args}]
    if { !$do_box && !$do_rig } { set do_box 1; set do_rig 1 }
    set keep [expr {"-keep" in $args}]
    set m [get $model]
    set report {}

    if { $do_box } {
        variable live
        set have {}
        if { [info exists live($box)] } { set have [parse_box $live($box)] }
        set d [box_diff [dict get $m box] $have]
        set n 0; set refused {}
        foreach part {missing differ} {
            dict for {leaf v} [dict get $d $part] {
                set want [expr {$part eq "differ" ? [lindex $v 0] : $v}]
                if { [catch { extio_cfg_set $box $leaf $want } e] } {
                    lappend refused "$leaf ($e)"
                } else { incr n }
            }
        }
        lappend report "box: $n leaf(s) set"
        if { [llength $refused] } { lappend report "box REFUSED: [join $refused {; }]" }
        if { $keep } {
            lappend report [extio_cfg_save $box]
        }
    }

    if { $do_rig } {
        set n 0; set refused {}
        foreach r [rig_diff [dict get $m rig]] {
            if { [dict get $r ok] && !$keep } continue
            if { [dict get $r ok] && [dict get $r source] eq "file" } continue
            if { [catch { rig_put [dict get $r sub] [dict get $r key] \
                              [dict get $r want] [dict get $r interp] $keep } e] } {
                lappend refused "[dict get $r sub] [dict get $r key] ($e)"
            } else { incr n }
        }
        lappend report "rig: $n setting(s) [expr {$keep ? {persisted} : {tried}}]"
        if { [llength $refused] } { lappend report "rig REFUSED: [join $refused {; }]" }
    }

    # re-judge once the box has had a moment to take the writes
    dservAfter [expr {$keep ? 4000 : 1500}] [list ::extio_models::judge $box]
    return [join $report \n]
}

# Undo a TRY on the rig side: drop the runtime overrides this model's knobs
# carry, so each falls back to rig.tcl / its default. (Box side: unsaved box
# changes are undone by extio_cfg_reboot.)
proc ::extio_models::revert {box model} {
    set n 0
    foreach r [rig_diff [dict get [get $model] rig]] {
        if { [dict get $r source] eq "runtime" } {
            catch { rig_clear [dict get $r sub] [dict get $r key] [dict get $r interp] }
            incr n
        }
    }
    dservAfter 1500 [list ::extio_models::judge $box]
    return "rig: $n runtime override(s) cleared"
}

# Write a new model from a live box into the rig's local/models/.
proc ::extio_models::snapshot {box name {desc ""}} {
    variable live; variable dirs
    if { ![info exists live($box)] } {
        error "no config read from '$box' yet -- run extio_model_judge $box first"
    }
    if { ![regexp {^[A-Za-z0-9][A-Za-z0-9._-]*$} $name] } {
        error "model name '$name': letters, digits, . _ - only"
    }
    set board ""
    catch { set board [lindex [split [dservGet extio/$box/state/board] \x00] 0] }
    set dir [lindex $dirs end]
    file mkdir $dir
    set path [file join $dir $name.tcl]
    if { [file exists $path] } { error "$path exists -- pick another name or edit it" }
    set tmp $path.tmp
    set f [open $tmp w]
    puts -nonewline $f [snapshot_text $name $desc $board [parse_box $live($box)]]
    close $f
    file rename -force $tmp $path
    load
    publish_catalog
    judge $box
    return $path
}

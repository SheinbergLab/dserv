# settings-1.0.tm -- declarative rig settings (STRAWMAN)
#
# The DECLARED half of the settings pair. settingsdb-1.0.tm is the LEARNED
# half. Router rule: humans DECLARE -> a file this module reads; the system
# LEARNS -> settingsdb's sqlite. A wrong declared value should be rejected
# at the door with a message that teaches; a lost learned value should
# degrade to a default.
#
# WHY A MODULE, NOT A SUBPROCESS: settings are data, not activity -- there is
# nothing to run. Every subprocess interp `package require settings` and
# reads the rig file itself (no boot ordering, no cross-interp reads -- the
# exact hazard class that truncated boots before). The shared view is the
# datapoint tree this module publishes, which the fleet page / agent read
# like any other datapoints. The only shared *resource* is the file, written
# rarely and atomically (tmp + rename); at human write rates that needs no
# arbiter.
#
# THE FILE: local/rig.tcl -- one place to look, but declarative-only, so the
# mega-file blast radius does not apply: it is parsed in a SAFE interp that
# exposes exactly one command, per-statement, so a bad line costs a recorded
# breadcrumb and a fallback to default, never an aborted boot. Lines look
# like:
#
#     setting extio obs_autobind auto
#     setting sound feedback_volume 90
#
# Comments and blank lines as in any Tcl file. Later lines win.
#
# API (put, not set -- a proc named `set` inside this namespace would
# shadow the builtin for every proc here):
#
#   settings::declare <sub> <key> ?-default v? ?-values {..}? ?-type t?
#                     ?-doc str? ?-validate script? ?-apply script?
#       Declare a knob. Call from the owning conf/module at load time.
#       -values   allowed list; entries like <boxname> are wildcards
#                 documenting "any name" (any non-empty value passes)
#       -type     bool | int | double  (bool normalizes on/off/1/0/..to 1/0)
#       -validate script appended with the value; returns normalized value
#                 or errors. Runs BEFORE -values/-type (it is the
#                 domain normalizer: e.g. autobind maps 1/on -> auto)
#       -apply    script appended with the value; runs on put/reload change
#
#   settings::get <sub> <key>            effective value (runtime > file >
#                                        default); publishes value + source
#   settings::source_of <sub> <key>      default | file | runtime -- for
#                                        one-time migrations from older stores
#   settings::interp_of <sub> <key>      name of the interp that declared it
#   settings::put <sub> <key> <v> ?-persist?
#       Validated runtime override. -persist writes the file line (surgical:
#       comments and unrelated lines byte-preserved) and reclassifies the
#       value as file-declared.
#   settings::clear <sub> <key> ?-all?
#       The inverse. Removes the layer the effective value comes from -- the
#       runtime override, else the file line -- so it undoes what /source
#       reports; -all strips both. Fires -apply only if the value changed.
#   settings::reload                     re-read file; -apply fires for keys
#                                        whose effective value changed
#   settings::example ?sub?              generated EXAMPLE text -- docs that
#                                        cannot drift from code
#   settings::audit                      file lines nobody owns (typo'd
#                                        subsystem, or its subprocess died)
#   settings::errors                     parse/validation breadcrumbs
#   settings::dump                       human table: sub key value source
#   settings::config -file <path>        override file location (tests)
#
# Publication (guarded -- absent outside dserv, so this file is testable in
# plain tclsh):  settings/<sub>/<key>          effective value
#                settings/<sub>/<key>/source   default | file | runtime
#                settings/<sub>/<key>/schema   the declaration dict
#                settings/parse_errors/<interp>  breadcrumbs THAT interp saw
#
# WHO OWNS A KNOB. Those datapoints are the shared view, so a page can READ
# every knob on the rig from one tree; but it cannot write one without
# knowing where to send the put -- `joystick transport` lives in ess,
# `juicer destination` in juicer, and only the declaring interp has the
# schema (_effective errors otherwise). So the schema dict carries an
# `interp` field, stamped from ::dserv_interp (dserv sets it per interp;
# see TclServer.cpp) at declare time. Reading it:
#
#     <name>   send <name> {settings::put <sub> <key> {<v>} -persist}
#     dserv    the main interp -- `send` refuses it; evaluate directly
#     (empty)  not addressable (a nameless one-off interp, or plain tclsh)
#
# Deliberately NOT an option of declare: a mistyped -interp at one call
# site would route writes to a live interp where the knob is not declared,
# and `send` to a live interp succeeds, so the setting would simply not
# exist there. Nothing that enumerates knobs may ask an interp for the
# list, either -- each knows only its own declarations. Walk the datapoint
# tree.

package provide settings 1.0

namespace eval ::settings {
    variable schema   [dict create]  ;# sub -> key -> declaration dict
    variable filevals [dict create]  ;# sub -> key -> validated file value
    variable runtime  [dict create]  ;# sub -> key -> runtime override
    variable rawlines {}             ;# every file `setting` line, in order
    variable errlist  {}             ;# parse/validation breadcrumbs
    variable loaded   0
    variable file     ""
}

proc ::settings::config {args} {
    variable file; variable loaded
    foreach {opt val} $args {
        switch -- $opt {
            -file   { set file $val; set loaded 0 }
            default { error "settings::config: unknown option '$opt'" }
        }
    }
}

proc ::settings::_file {} {
    variable file
    if { $file ne "" } { return $file }
    if { [info exists ::dspath] } { return [file join $::dspath local rig.tcl] }
    return [file join [pwd] local rig.tcl]
}

# This interp's own name, as `send` spells it. dserv sets ::dserv_interp
# when it creates the interp; outside dserv (plain tclsh, tests) there is no
# name and no route, and empty says exactly that.
proc ::settings::_interp_name {} {
    if { [info exists ::dserv_interp] } { return $::dserv_interp }
    return ""
}

proc ::settings::declare {sub key args} {
    variable schema; variable filevals; variable loaded; variable errlist
    set d [dict create default "" values {} type "" doc "" validate "" apply ""]
    foreach {opt val} $args {
        set name [string range $opt 1 end]
        if { ![dict exists $d $name] } {
            error "settings::declare $sub $key: unknown option '$opt'"
        }
        dict set d $name $val
    }
    # Stamped, never passed: `interp` is absent from the template above, so
    # -interp is an unknown option at every call site by construction (see
    # WHO OWNS A KNOB in the header).
    dict set d interp [_interp_name]
    dict set schema $sub $key $d
    _dp settings/$sub/$key/schema $d

    # A file line read BEFORE this knob existed was stored raw -- nobody
    # could judge it (_file_line's else branch). Judge it now, or validation
    # would depend on which declaration happened to come first: whichever
    # knob triggers the first load makes every later one's file value
    # unchecked. Same outcome as parsing it late: breadcrumb, fall back to
    # the default, never an error out of `declare`.
    if { $loaded && [dict exists $filevals $sub $key] } {
        if { [catch { _validate $sub $key [dict get $filevals $sub $key] } v] } {
            dict unset filevals $sub $key
            lappend errlist $v
            _publish_errors
        } else {
            dict set filevals $sub $key $v
        }
    }

    # Publish the effective value too. _publish_declared only runs at load,
    # so a knob declared AFTER the first load had a schema in the tree and no
    # value and no source -- which is how joystick box_device and box_group
    # looked to the settings panel: present, unreadable, unwritable. Wrapped
    # because this is the first thing that can trigger a file read, and a
    # declaration must not fail on one.
    catch {
        lassign [_effective $sub $key] v src
        _dp settings/$sub/$key $v
        _dp settings/$sub/$key/source $src
    }
    return
}

# Normalize-and-check. The error message IS the documentation: allowed
# values + doc string arrive at the moment of the mistake.
proc ::settings::_validate {sub key value} {
    variable schema
    if { ![dict exists $schema $sub] } {
        return $value               ;# another interp's subsystem: not ours to judge
    }
    if { ![dict exists $schema $sub $key] } {
        error "unknown setting '$key' for '$sub'\
 (known: [lsort [dict keys [dict get $schema $sub]]])"
    }
    set d [dict get $schema $sub $key]
    set vscript [dict get $d validate]
    if { $vscript ne "" } {
        set value [uplevel #0 $vscript [list $value]]
    }
    switch -- [dict get $d type] {
        bool {
            switch -nocase -- $value {
                1 - on - true - yes  { set value 1 }
                0 - off - false - no { set value 0 }
                default { error "setting $sub $key: '$value' is not a\
 boolean (on/off, 1/0, true/false)" }
            }
        }
        int {
            if { ![string is integer -strict $value] } {
                error "setting $sub $key: '$value' is not an integer"
            }
        }
        double {
            if { ![string is double -strict $value] } {
                error "setting $sub $key: '$value' is not a number"
            }
        }
    }
    set allowed [dict get $d values]
    if { [llength $allowed] } {
        set wildcard 0
        foreach a $allowed {
            if { $a eq $value } { return $value }
            if { [string match {<*>} $a] } { set wildcard 1 }
        }
        if { $wildcard && $value ne "" } { return $value }
        set msg "setting $sub $key: '$value' not one of {$allowed}"
        if { [dict get $d doc] ne "" } { append msg " -- [dict get $d doc]" }
        error $msg
    }
    return $value
}

# Receives each `setting` line from the safe parse interp.
proc ::settings::_file_line {sub key value} {
    variable filevals; variable rawlines; variable errlist; variable schema
    lappend rawlines [list $sub $key $value]
    # Judge it only if THIS KEY is already declared. Judging on the SUB was
    # wrong and cost a rig its declared value: `juicer destination` is
    # declared before `juicer hand_ml`, so a load triggered between the two
    # -- which is every load now that declare publishes an effective value
    # -- saw a known sub, an unknown key, and threw the line away. The rig
    # then ran on the default with only a breadcrumb to say so.
    #
    # So an undeclared key is HELD, exactly like an undeclared sub, and
    # declare validates what it finds held. Validity must not depend on
    # which declaration happened to trigger the first read. A line nobody
    # ever declares stays held and unused; settings::audit is what reports
    # those, and a typo'd key is the case it exists for.
    if { [dict exists $schema $sub $key] } {
        if { [catch { _validate $sub $key $value } v] } {
            lappend errlist $v
            _publish_errors
            return
        }
        dict set filevals $sub $key $v
    } else {
        dict set filevals $sub $key $value
    }
}

proc ::settings::load {} {
    variable filevals [dict create]
    variable rawlines {}
    variable errlist {}
    variable loaded 1
    set f [_file]
    if { ![file exists $f] } { _publish_declared; return 0 }
    set fd [open $f]; set txt [read $fd]; close $fd
    set ip [interp create -safe]
    interp alias $ip setting {} ::settings::_file_line
    # Per-STATEMENT eval: one bad line = one breadcrumb + default fallback;
    # the rest of the file still lands. Contrast: a thrown `source` of a
    # local/*.tcl aborts that subprocess's whole config (local/README).
    set stmt ""; set n 0; set first 0
    variable errlist
    foreach line [split $txt \n] {
        incr n
        if { $stmt eq "" } { set first $n }
        append stmt $line \n
        if { ![info complete $stmt] } continue
        set s [string trim $stmt]; set stmt ""
        if { $s eq "" || [string index $s 0] eq "#" } continue
        if { [catch { $ip eval $s } err] } {
            lappend errlist "[file tail $f]:$first: $err"
        }
    }
    if { [string trim $stmt] ne "" } {
        lappend errlist "[file tail $f]:$first: unterminated command (unbalanced braces?)"
    }
    interp delete $ip
    _publish_declared
    return 1
}

proc ::settings::_effective {sub key} {
    variable schema; variable filevals; variable runtime; variable loaded
    if { !$loaded } { load }
    if { ![dict exists $schema $sub $key] } {
        error "settings::get $sub $key: not declared in this interp\
 (settings::declare first)"
    }
    if { [dict exists $runtime $sub $key] } {
        return [list [dict get $runtime $sub $key] runtime]
    }
    if { [dict exists $filevals $sub $key] } {
        return [list [dict get $filevals $sub $key] file]
    }
    return [list [dict get $schema $sub $key default] default]
}

proc ::settings::get {sub key} {
    lassign [_effective $sub $key] v src
    _dp settings/$sub/$key $v
    _dp settings/$sub/$key/source $src
    return $v
}

# Where does the effective value come from: default | file | runtime.
# Exists for migrations: "has a human declared this yet, or are we still on
# the default?" is the question a one-time carryover from an older store
# must ask before writing the file line.
proc ::settings::source_of {sub key} {
    lassign [_effective $sub $key] v src
    return $src
}

# Where a put for this knob has to run. Same value the schema datapoint
# carries; here for anyone driving settings from essctrl rather than a page.
proc ::settings::interp_of {sub key} {
    variable schema
    if { ![dict exists $schema $sub $key] } {
        error "settings::interp_of $sub $key: not declared in this interp"
    }
    return [dict get $schema $sub $key interp]
}

#
# Take a value BACK. Every other verb here only ever adds: put writes a
# runtime override or a file line, and nothing removed either, so a rig could
# not return a knob to the state it shipped in without hand-editing the file
# this module exists to stop people hand-editing.
#
# `clear` removes the layer the effective value COMES FROM -- the runtime
# override if there is one, otherwise the file line -- so it undoes exactly
# what /source is reporting, and a knob carrying both takes two calls with a
# visible step in between. -all strips both at once.
#
# The -apply chain fires only if the effective value actually changed, the
# same rule reload uses: clearing a runtime override that happened to match
# the file value must not re-bind hardware for nothing.
#
proc ::settings::clear {sub key args} {
    variable runtime; variable filevals; variable schema; variable loaded
    if { !$loaded } { load }
    set all 0
    foreach opt $args {
        switch -- $opt {
            -all    { set all 1 }
            default { error "settings::clear: unknown option '$opt'" }
        }
    }
    if { ![dict exists $schema $sub $key] } {
        error "settings::clear $sub $key: not declared in this interp"
    }
    set before [lindex [_effective $sub $key] 0]

    set popped 0
    if { [dict exists $runtime $sub $key] } {
        dict unset runtime $sub $key
        set popped 1
    }
    if { $all || !$popped } {
        if { [dict exists $filevals $sub $key] } {
            dict unset filevals $sub $key
        }
        _unpersist_line $sub $key
    }

    lassign [_effective $sub $key] v src
    _dp settings/$sub/$key $v
    _dp settings/$sub/$key/source $src
    if { $v ne $before } {
        set apply [dict get $schema $sub $key apply]
        if { $apply ne "" } { uplevel #0 $apply [list $v] }
    }
    return $v
}

# The inverse of _persist_line: drop every active `setting sub key ..` line,
# and the "# persisted ..." breadcrumb this module wrote directly above one
# (never a human's comment -- only that exact generated prefix, and only
# where it sits immediately above the line being removed). Everything else
# is byte-preserved, atomic via tmp + rename, as on the way in.
proc ::settings::_unpersist_line {sub key} {
    set f [_file]
    if { ![file exists $f] } return
    set fd [open $f]; set lines [split [read $fd] \n]; close $fd
    if { [lindex $lines end] eq "" } { set lines [lrange $lines 0 end-1] }

    set out {}
    foreach l $lines {
        set t [string trim $l]
        if { [string index $t 0] ne "#" && ![catch { llength $t } nw] && $nw >= 3
             && [lindex $t 0] eq "setting" && [lindex $t 1] eq $sub
             && [lindex $t 2] eq $key } {
            if { [llength $out] &&
                 [string match "# persisted *via settings::put" \
                      [string trim [lindex $out end]]] } {
                set out [lrange $out 0 end-1]
            }
            continue
        }
        lappend out $l
    }
    if { $out eq $lines } return                ;# nothing of ours in there

    set tmp ${f}.tmp[pid]
    set fd [open $tmp w]
    puts -nonewline $fd [expr {[llength $out] ? "[join $out \n]\n" : ""}]
    close $fd
    file rename -force $tmp $f
}

proc ::settings::put {sub key value args} {
    variable runtime; variable filevals; variable schema; variable loaded
    if { !$loaded } { load }
    set persist 0
    foreach opt $args {
        switch -- $opt {
            -persist { set persist 1 }
            default  { error "settings::put: unknown option '$opt'" }
        }
    }
    set v [_validate $sub $key $value]      ;# throws the teaching error
    if { $persist } {
        _persist_line $sub $key $v
        dict set filevals $sub $key $v
        # Guarded: nested `dict unset` THROWS when the intermediate key is
        # absent, i.e. on every put for a sub that never had a runtime
        # override -- which is the common case. Unguarded, the file was
        # already written but the publish and -apply below never ran: a
        # persisted value the running system silently did not adopt.
        if { [dict exists $runtime $sub $key] } {
            dict unset runtime $sub $key
        }
        _dp settings/$sub/$key $v
        _dp settings/$sub/$key/source file
    } else {
        dict set runtime $sub $key $v
        _dp settings/$sub/$key $v
        _dp settings/$sub/$key/source runtime
    }
    if { [dict exists $schema $sub $key] } {
        set apply [dict get $schema $sub $key apply]
        if { $apply ne "" } { uplevel #0 $apply [list $v] }
    }
    return $v
}

# Surgical file write: replace the (last) active `setting sub key ..` line,
# or append one; every other byte of the file -- comments included -- is
# preserved. Atomic via tmp + rename.
proc ::settings::_persist_line {sub key value} {
    set f [_file]
    file mkdir [file dirname $f]
    set lines {}
    if { [file exists $f] } {
        set fd [open $f]; set lines [split [read $fd] \n]; close $fd
        if { [lindex $lines end] eq "" } { set lines [lrange $lines 0 end-1] }
    }
    set newline "setting $sub $key [list $value]"
    set done 0
    for {set i 0} {$i < [llength $lines]} {incr i} {
        set l [string trim [lindex $lines $i]]
        if { [string index $l 0] eq "#" } continue
        if { [catch { llength $l } nw] || $nw < 3 } continue
        if { [lindex $l 0] eq "setting" && [lindex $l 1] eq $sub
             && [lindex $l 2] eq $key } {
            set lines [lreplace $lines $i $i $newline]
            set done 1              ;# rewrite every occurrence: last wins at
        }                           ;# source time, so all must agree
    }
    if { !$done } {
        lappend lines "# persisted [clock format [clock seconds] \
            -format {%Y-%m-%d %H:%M:%S}] via settings::put"
        lappend lines $newline
    }
    set tmp ${f}.tmp[pid]
    set fd [open $tmp w]
    puts -nonewline $fd [join $lines \n]\n
    close $fd
    file rename -force $tmp $f
}

proc ::settings::reload {} {
    variable schema
    set before [dict create]
    dict for {sub keys} $schema {
        dict for {key d} $keys {
            dict set before $sub $key [lindex [_effective $sub $key] 0]
        }
    }
    load
    dict for {sub keys} $schema {
        dict for {key d} $keys {
            lassign [_effective $sub $key] v src
            _dp settings/$sub/$key $v
            _dp settings/$sub/$key/source $src
            if { $v ne [dict get $before $sub $key] } {
                set apply [dict get $d apply]
                if { $apply ne "" } { uplevel #0 $apply [list $v] }
            }
        }
    }
    return
}

# EXAMPLE text generated from declarations -- the docs cannot drift.
proc ::settings::example {{sub ""}} {
    variable schema
    set subs [expr {$sub ne "" ? [list $sub] : [lsort [dict keys $schema]]}]
    set out ""
    foreach s $subs {
        if { ![dict exists $schema $s] } {
            append out "# no settings declared for '$s' in this interp\n"
            continue
        }
        append out "# ---- $s ----\n"
        dict for {key d} [dict get $schema $s] {
            foreach dl [split [dict get $d doc] \n] {
                if { $dl ne "" } { append out "# $dl\n" }
            }
            if { [llength [dict get $d values]] } {
                append out "#   allowed: [dict get $d values]\n"
            }
            if { [dict get $d type] ne "" } {
                append out "#   type: [dict get $d type]\n"
            }
            set def [dict get $d default]
            append out "#   default: [expr {$def eq "" ? "(empty)" : $def}]\n"
            append out "# setting $s $key $def\n\n"
        }
    }
    return $out
}

# File lines nobody owns: a typo'd subsystem, or one whose subprocess never
# booted. Judged against this interp's declarations first, then against
# settings/* datapoints other interps published. Run after boot completes.
proc ::settings::audit {} {
    variable rawlines; variable schema; variable loaded
    if { !$loaded } { load }
    set orphans {}
    foreach rl $rawlines {
        lassign $rl sub key value
        if { [dict exists $schema $sub $key] } continue
        if { [info commands dservExists] ne ""
             && ![catch { dservExists settings/$sub/$key } e] && $e } continue
        lappend orphans $rl
    }
    return $orphans
}

proc ::settings::errors {} { variable errlist; return $errlist }

proc ::settings::dump {} {
    variable schema
    set out ""
    dict for {sub keys} $schema {
        dict for {key d} $keys {
            lassign [_effective $sub $key] v src
            append out [format "%-10s %-20s %-14s %s\n" $sub $key \
                            [expr {$v eq "" ? "(empty)" : $v}] $src]
        }
    }
    return $out
}

proc ::settings::_publish_declared {} {
    variable schema
    _publish_errors
    dict for {sub keys} $schema {
        dict for {key d} $keys {
            lassign [_effective $sub $key] v src
            _dp settings/$sub/$key $v
            _dp settings/$sub/$key/source $src
        }
    }
}

#
# Breadcrumbs are PER INTERP, and the datapoint name has to say so.
#
# Every interp parses the whole rig file, but each judges only its own
# declarations -- so `ess` sees the bad joystick line and `juicer` does not,
# and both used to publish their list to one flat settings/parse_errors.
# Whoever wrote last won, which for a page reading it meant the errors it
# showed depended on boot order. One datapoint per interp instead; a reader
# enumerates settings/parse_errors/* the way it enumerates everything else.
#
proc ::settings::_publish_errors {} {
    variable errlist
    set who [_interp_name]
    if { $who eq "" } { set who unknown }
    _dp settings/parse_errors/$who $errlist
}

proc ::settings::_dp {name value} {
    if { [info commands dservSet] eq "" } return
    catch { dservSet $name $value }
}

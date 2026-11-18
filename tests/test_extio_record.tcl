#
# test_extio_record.tcl
#
#  The rig's `extio record` setting (config/extioconf.tcl) and its merge into
#  a data file's matches (::ess::rig_record_merge, lib/ess-2.0.tm).
#
#  Names resolve by LABEL against LIVE boxes only: an analog group, a
#  labelled DI line, a chord group; ambiguous across boxes until qualified as
#  <box>/<name>; a name nothing answers is reported, never fatal. The merge
#  tags each analog stream with who asked (param | rig | both) and never
#  drops a protocol's record_streams entry.
#
#  A UNIT test: plain tclsh, dserv and settings STUBBED. The code under test
#  is cut out of the two files verbatim (between the extio/record block's
#  first comment and the next section; the rig_record_merge proc), so it
#  cannot drift from what ships.
#
set repo [file normalize [file join [file dirname [info script]] ..]]

# ---- stub datapoint table -------------------------------------------------
array set DP {}
proc dservGet {k} { if {![info exists ::DP($k)]} { error "no $k" }; return $::DP($k) }
proc dservExists {k} { info exists ::DP($k) }
proc dservSet {k v} { set ::DP($k) $v; lappend ::SETS $k }
proc dservKeys {{pat *}} { lsort [array names ::DP $pat] }
set ::SETS {}

# ---- stub settings --------------------------------------------------------
namespace eval ::settings { variable vals; array set vals {}; variable decl {} }
proc ::settings::declare {sub key args} { lappend ::settings::decl [list $sub $key {*}$args] }
proc ::settings::get {sub key} {
    if {[info exists ::settings::vals($sub,$key)]} { return $::settings::vals($sub,$key) }
    return ""
}
proc setrec {v} { set ::settings::vals(extio,record) [extio_record_norm $v] }

# ---- extract the record block from extioconf -------------------------------
set f [open $repo/config/extioconf.tcl]; set src [read $f]; close $f
set a [string first "# ---- extio/record: what THIS RIG" $src]
set b [string first "# ---- STAGE-1 OTA orchestrator" $src]
if {$a < 0 || $b < 0} { error "markers not found" }
set ::extio_ain_streams_last ""
eval [string range $src $a [expr {$b - 1}]]

# ---- extract ::ess::rig_record_merge -------------------------------------
set f [open $repo/lib/ess-2.0.tm]; set src [read $f]; close $f
set a [string first "    proc rig_record_merge" $src]
set e [string first " \{\n" $src $a]
while {![info complete [string range $src $a $e]] || [string index $src $e] ne "\}"} { incr e }
namespace eval ::ess {}
namespace eval ::ess [string range $src $a $e]

# ---- checks ----------------------------------------------------------------
set fails 0; set n 0
proc check {what got want} {
    incr ::n
    if {$got ne $want} { incr ::fails; puts "FAIL $what\n  got:  $got\n  want: $want" } else { puts "ok   $what" }
}
proc checkerr {what script pat} {
    incr ::n
    if {![catch {uplevel 1 $script} err]} { incr ::fails; puts "FAIL $what: no error"; return }
    if {![string match $pat $err]} { incr ::fails; puts "FAIL $what: $err" } else { puts "ok   $what" }
}

# norm
check "norm commas+dupes" [extio_record_norm " photodiode, tlock  photodiode "] "photodiode tlock"
check "norm none" [extio_record_norm none] ""
check "norm qualified" [extio_record_norm "box01/eye"] "box01/eye"
checkerr "norm rejects braces" [list extio_record_norm "a\{b"] "*not a name*"
checkerr "norm rejects two slashes" {extio_record_norm "a/b/c"} "*not a name*"

# a rig: box01 live with analog group pd (+ eye), DI pin 3 labelled tlock,
# DI pin 4 unlabelled, chord group resp; box02 dead but lingering with a pd
set ::extio_known(box01) 1
set ::extio_ain_streams_last [dict create \
    box01/pd  {box box01 group pd  dpoint extio/box01/state/ain/pd  chans 2 labels photodiode rate_hz 1000} \
    box01/eye {box box01 group eye dpoint extio/box01/state/ain/eye chans {0 1} labels {eye_x eye_y}}]
set DP(extio/box01/state/pins/in) "3,4,5\x00"
set DP(extio/box01/state/label/3) "tlock\x00"
set DP(extio/box01/state/label/4) ""
set DP(extio/box01/state/group/resp/pins) "5"
set DP(extio/box01/state/group/old/pins) ""          ;# tombstoned
set DP(extio/box02/state/pins/in) "3"
set DP(extio/box02/state/label/3) "pd"                ;# dead box: must not count

setrec "pd tlock resp old nothere"
set r [extio_record_resolve]
set s [dict get $r streams]
check "declared" [dict get $r declared] "pd tlock resp old nothere"
check "pd -> ain" [list [dict get $s pd status] [dict get $s pd kind] [dict get $s pd dpoint]] \
    "ok ain extio/box01/state/ain/pd"
check "tlock -> di" [list [dict get $s tlock status] [dict get $s tlock kind] [dict get $s tlock dpoint]] \
    "ok di extio/box01/state/di/3"
check "resp -> group" [list [dict get $s resp status] [dict get $s resp dpoint]] \
    "ok extio/box01/state/group/resp"
check "tombstoned group missing" [dict get $s old status] missing
check "nothere missing" [dict get $s nothere status] missing

extio_record_publish
check "health error names" [dservGet extio/health] "error recording: old missing, nothere missing"
setrec "pd tlock"
extio_record_publish
check "health ok" [dservGet extio/health] "ok 1 box live (box01); recording pd, tlock"
set ::SETS {}
extio_record_publish
check "publish on change only" $::SETS ""

# ambiguity: box02 comes alive, its DI pin 3 is labelled pd
set ::extio_known(box02) 1
setrec "pd"
set s [dict get [extio_record_resolve] streams]
check "pd ambiguous across boxes" [dict get $s pd status] ambiguous
setrec "box01/pd"
set s [dict get [extio_record_resolve] streams]
check "qualified resolves" [list [dict get $s box01/pd status] [dict get $s box01/pd kind]] "ok ain"
setrec "box02/pd"
set s [dict get [extio_record_resolve] streams]
check "qualified DI on box02" [dict get $s box02/pd dpoint] extio/box02/state/di/3

# no boxes, empty list
array unset ::extio_known
set ::extio_ain_streams_last ""
setrec ""
extio_record_publish
check "health off" [dservGet extio/health] "off no box is live"

# candidates
set ::extio_known(box01) 1
set ::extio_known(box02) 1
set ::extio_ain_streams_last [dict create \
    box01/pd {box box01 group pd dpoint extio/box01/state/ain/pd chans 2 labels photodiode rate_hz 1000}]
setrec "gone"
set c [::extio::candidates record]
set routes {}
foreach x $c { lappend routes [dict get $x route] }
check "candidate routes (pd qualified: box02 has a DI line pd)" $routes \
    "box01/pd tlock resp box02/pd gone"
check "all multi" [lsort -unique [lmap x $c {dict get $x multi}]] 1
check "declared-missing unresolved" [dict get [lindex $c end] status] unresolved
checkerr "unknown kind" {::extio::candidates foo} "*unknown kind*"

# ---- ess merge ------------------------------------------------------------
array unset ::extio_known
set ::extio_known(box01) 1
array unset DP extio/box02/*
set ::extio_ain_streams_last [dict create \
    box01/pd  {box box01 group pd  dpoint extio/box01/state/ain/pd  chans 2 labels photodiode} \
    box01/eye {box box01 group eye dpoint extio/box01/state/ain/eye chans {0 1} labels {eye_x eye_y}}]
setrec "pd eye tlock nothere"
extio_record_publish
set ain [dict create want eye \
    recorded [dict create box01/eye [dict get $::extio_ain_streams_last box01/eye]] \
    declined [dict create box01/pd {group pd reason not-in-record_streams}] \
    missing {} ambiguous {}]
set m [::ess::rig_record_merge $ain]
set ma [dict get $m ain]
check "eye source both" [dict get $ma recorded box01/eye source] both
check "pd source rig"   [dict get $ma recorded box01/pd source] rig
check "pd no longer declined" [dict exists $ma declined box01/pd] 0
check "digital" [dict get $m digital] [list [list extio/box01/state/di/3 di box01]]
check "unresolved" [lindex [lindex [dict get $m unresolved] 0] 0] nothere

unset DP(extio/record)
set m [::ess::rig_record_merge $ain]
check "no extio/record: param only" [dict get $m ain recorded box01/eye source] param
check "no extio/record: no digital" [dict get $m digital] ""

if {$fails} { puts "\n$n checks, $fails FAILED"; exit 1 }
puts "\n$n checks: all checks passed"

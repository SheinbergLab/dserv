#
# test_ess_sync_libs_gone.tcl
#
#  What ess_sync's sync_libs does with libraries the registry no longer has
#  (lib/ess_sync-1.0.tm: sync_libs, _lib_registry_gone).
#
#  The lib base manifest (.sync_base.json) is the only record that a file in
#  lib/ is a synced copy rather than something made by hand. `dservctl libs
#  prune` removes a lib only when that record says it was synced and the
#  file is unchanged since. So sync must not drop the record while the file
#  is still there -- it used to, on every pull, which left prune nothing to
#  go on. It now:
#
#    keeps the entry      file present (unchanged or edited)
#    drops the entry      file gone as well
#    reports              unchanged copies as deleted_upstream (prunable),
#                         edited ones as deleted_upstream_edited (kept)
#    ignores              files with no entry (made by hand)
#
#  The same rule is dservctl's planLibsPrune (tools/dservctl).
#
#  A UNIT test: plain tclsh, no server, no rig, no network. The registry
#  calls (https_get, json_to_dict) and the manifest file I/O are stubbed;
#  the procs under test are extracted from the module as in
#  test_ess_workgroup_pair.tcl.
#
#  Run as: tclsh tests/test_ess_sync_libs_gone.tcl   (or: ctest -R ess_sync_libs_gone)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

set fh [open [file join $::REPO lib ess_sync-1.0.tm]]
set src [read $fh]
close $fh

# Same linear extraction as test_ess_binds -- see there for why not a regex.
proc extract_proc { src name } {
    set start [string first "\n    proc $name " $src]
    if { $start < 0 } { return "" }
    set end [string first "\n    \}\n" $src $start]
    if { $end < 0 } { return "" }
    set nl   [string first "\n" $src [expr {$start + 1}]]
    set head [string trimright [string range $src [expr {$start + 1}] \
                                    [expr {$nl - 1}]] " \{"]
    set body [string range $src [expr {$nl + 1}] [expr {$end - 1}]]
    return [list [lindex $head 2] $body]
}

namespace eval ess {
    variable system_path {}
    variable registry_url https://registry.test
    variable registry_workgroup wg
    variable current
    set current(project) ess
}

foreach p {sync_libs _lib_registry_gone _base_entry_get _base_entry_set
           _base_entry_unset _base_decide} {
    set got [extract_proc $src $p]
    if { $got eq "" } { puts "FAIL: could not extract proc $p"; exit 1 }
    proc ::ess::$p [lindex $got 0] [lindex $got 1]
}

# ---- stubs ------------------------------------------------------------

# The manifest lives in memory; read/write go through ::MANIFEST.
proc ::ess::_base_lib_manifest_path { project } { return MANIFEST }
proc ::ess::_base_manifest_read { path } { return $::MANIFEST }
proc ::ess::_base_manifest_write { path m } { set ::MANIFEST $m }
proc ::ess::_manifest_workgroup { path } { return "" }
proc ::ess::_refuse_foreign_tree { args } {}
proc ::ess::_sync_displace_file { args } {}

# The registry: only the lib list is ever fetched (every lib still in the
# registry is unchanged locally, so nothing is pulled).
proc https_get { url } { return $url }
proc json_to_dict { raw } {
    if { [string match *lib/* $raw] } { error "unexpected lib pull: $raw" }
    return [dict create libs $::REG_LIBS]
}

proc sha256 { opt path } { lindex [exec shasum -a 256 $path] 0 }
proc mkdir_matching_owner { d } { file mkdir $d }
proc fix_file_ownership { f } {}
set ::LOG {}
proc ess_info    { msg {cat {}} } { lappend ::LOG [list info $msg] }
proc ess_warning { msg {cat {}} } { lappend ::LOG [list warning $msg] }
proc ess_error   { msg {cat {}} } { lappend ::LOG [list error $msg] }

set FAIL 0
proc ok   { label } { puts "  ok   $label" }
proc fail { label } { puts "  FAIL $label"; set ::FAIL 1 }
proc check { label cond } {
    if { [uplevel 1 [list expr $cond]] } { ok $label } else { fail $label }
}

# ---- fixture ----------------------------------------------------------
#
#   kept-1.0.tm      synced, still in the registry
#   gone-1.0.tm      synced, deleted from the registry, unchanged  -> prunable
#   edited-1.0.tm    synced, deleted from the registry, edited     -> kept, reported
#   handmade-1.0.tm  no base entry (never synced)                  -> ignored
#   vanished-1.0.tm  base entry only; file gone everywhere         -> entry dropped

set ROOT [file join [pwd] .test_libsgone_[pid]]
file delete -force $ROOT
set LIB [file join $ROOT ess lib]
file mkdir $LIB
set ess::system_path $ROOT

proc put { name content } {
    set f [open [file join $::LIB $name] w]
    puts -nonewline $f $content
    close $f
    return [sha256 -file [file join $::LIB $name]]
}
set k_kept   [put kept-1.0.tm kept]
set k_gone   [put gone-1.0.tm gone]
set k_edited [put edited-1.0.tm "edited locally"]
put handmade-1.0.tm mine
set k_orig_edited [exec sh -c "printf edited | shasum -a 256" | awk {{print $1}}]

set ::MANIFEST [dict create entries [dict create \
    kept-1.0.tm     [dict create checksum $k_kept version 1.0] \
    gone-1.0.tm     [dict create checksum $k_gone version 1.0] \
    edited-1.0.tm   [dict create checksum [lindex $k_orig_edited 0] version 1.0] \
    vanished-1.0.tm [dict create checksum deadbeef version 1.0]]]

set ::REG_LIBS [list [dict create filename kept-1.0.tm checksum $k_kept name kept version 1.0]]

# ---- the classification on its own -------------------------------------

set gone [ess::_lib_registry_gone $LIB $::MANIFEST [dict create kept-1.0.tm 1]]
check "_lib_registry_gone: prune = the unchanged synced copy" \
    {[dict get $gone prune] eq {gone-1.0.tm}}
check "_lib_registry_gone: edited = the edited synced copy" \
    {[dict get $gone edited] eq {edited-1.0.tm}}
check "_lib_registry_gone: vanished = entry with no file" \
    {[dict get $gone vanished] eq {vanished-1.0.tm}}

# ---- sync_libs end to end ----------------------------------------------

set res [ess::sync_libs]
set entries [dict get $::MANIFEST entries]

check "sync reports the prunable lib" {[dict get $res deleted_upstream] eq {gone-1.0.tm}}
check "sync reports the edited lib" {[dict get $res deleted_upstream_edited] eq {edited-1.0.tm}}
check "sync keeps the entry of an unchanged copy still on disk" {[dict exists $entries gone-1.0.tm]}
check "sync keeps the entry of an edited copy still on disk" {[dict exists $entries edited-1.0.tm]}
check "sync drops the entry whose file is gone" {![dict exists $entries vanished-1.0.tm]}
check "sync keeps the entry of a lib still in the registry" {[dict exists $entries kept-1.0.tm]}
check "sync adds nothing for a hand-made file" {![dict exists $entries handmade-1.0.tm]}
check "sync touches no files" {[llength [glob -directory $LIB *.tm]] == 4}
check "sync warns, naming prune" \
    {[llength [lsearch -all -glob $::LOG {warning *gone-1.0.tm*libs prune*}]] == 1}
check "callers' keys are unchanged" \
    {[dict get $res pulled] == 0 && [dict get $res unchanged] == 1 && [dict get $res errors] eq {}}

# An empty or failed lib list must never act on anything.
set before $::MANIFEST
set ::REG_LIBS {}
set res [ess::sync_libs]
check "an empty registry list changes no entries" {$::MANIFEST eq $before}
check "an empty registry list reports nothing" {[dict get $res deleted_upstream] eq {}}

file delete -force $ROOT

if { $FAIL } { puts "\nFAILURES"; exit 1 }
puts "\nall checks passed"

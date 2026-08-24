#
# test_ess_workgroup_pair.tcl
#
#  A scripts tree BELONGS to a workgroup, and the (workgroup, tree) pair is
#  the unit. These are the guards in lib/ess_sync-1.0.tm that refuse to act
#  when the two disagree.
#
#  The hazard they close: pointing ESS_SYSTEM_PATH at a tree pulled for
#  ANOTHER workgroup -- the obvious move when borrowing a colleague's
#  scripts -- used to be silently destructive. dserv boots, scriptsconf arms
#  initial_sync, and five seconds later sync_base acts on the disagreement
#  in BOTH directions at once: our systems are written into their tree (they
#  aren't in it, so nothing even looks like a conflict), and every shared
#  file has its base manifest rejected as foreign, decides `cold`, and is
#  displaced to .sync_displaced and overwritten.
#
#  Two guards, because one is not enough, and the second assertion below is
#  the whole reason:
#
#    _require_system_workgroup  one directory -- catches displacement
#    _require_tree_workgroup    the whole tree -- catches the systems that
#                               are NOT there yet, which the per-system
#                               check cannot see
#
#  A UNIT test: plain tclsh, no server, no rig, no network. Fixture trees
#  are built under a temp dir, so it does not care what is in anyone's
#  ~/systems. json_to_dict stands in for tcljson (not available to bare
#  tclsh) -- the guards read exactly one plain top-level string from each
#  manifest, so the stub only has to find that.
#
#  Run as: tclsh tests/test_ess_workgroup_pair.tcl   (or: ctest -R ess_workgroup_pair)
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
    variable registry_workgroup {}
    variable current
    set current(project) ess
}

foreach p {_manifest_workgroup _tree_workgroups _refuse_foreign_tree
           _require_tree_workgroup _require_system_workgroup
           _base_manifest_path} {
    set got [extract_proc $src $p]
    if { $got eq "" } { puts "FAIL: could not extract proc $p"; exit 1 }
    proc ::ess::$p [lindex $got 0] [lindex $got 1]
}

# Stand-in for tcljson. The guards ask one thing of a manifest: the value of
# the top-level "workgroup" string. Anything else in the file is irrelevant
# to them, so this does not need to be a JSON parser.
proc json_to_dict { raw } {
    set d [dict create]
    if { [regexp {"workgroup"\s*:\s*"([^"]*)"} $raw -> wg] } {
        dict set d workgroup $wg
    }
    return $d
}

set FAIL 0
proc ok   { label } { puts "  ok   $label" }
proc fail { label } { puts "  FAIL $label"; set ::FAIL 1 }

proc allows { label script } {
    if { [catch { uplevel 1 $script } res] } {
        fail "$label -- refused unexpectedly: $res"
    } else { ok $label }
}
proc refuses { label script want } {
    if { ![catch { uplevel 1 $script } res] } {
        fail "$label -- allowed, should have refused"
    } elseif { ![string match $want $res] } {
        fail "$label -- wrong message: $res"
    } else { ok $label }
}

# ---- fixtures -------------------------------------------------------

set ROOT [file join [pwd] .test_wgpair_[pid]]
file delete -force $ROOT

# A tree with a base manifest in each system dir plus lib/, as a real sync
# leaves it. Only the workgroup field matters here.
proc make_tree { root project workgroup systems } {
    foreach d [concat [list lib] $systems] {
        set dir [file join $root $project $d]
        file mkdir $dir
        set f [open [file join $dir .sync_base.json] w]
        puts -nonewline $f "{\"schemaVersion\":1,\"workgroup\":\"$workgroup\",\
\"defaultVersion\":\"main\",\"entries\":{}}"
        close $f
    }
}

make_tree [file join $ROOT ours]   ess brown-sheinberg {planko emcalib detection}
make_tree [file join $ROOT theirs] ess jhu-monosov     {planko emcalib}
file mkdir [file join $ROOT fresh ess]                 ;# never synced

# ---- what a tree claims ---------------------------------------------

puts "reading the tree's own declaration:"
set ess::system_path [file join $ROOT ours]
if { [ess::_tree_workgroups ess] eq "brown-sheinberg" } {
    ok "our tree names brown-sheinberg"
} else { fail "our tree: got '[ess::_tree_workgroups ess]'" }

set ess::system_path [file join $ROOT theirs]
if { [ess::_tree_workgroups ess] eq "jhu-monosov" } {
    ok "borrowed tree names jhu-monosov"
} else { fail "borrowed tree: got '[ess::_tree_workgroups ess]'" }

set ess::system_path [file join $ROOT fresh]
if { [ess::_tree_workgroups ess] eq "" } {
    ok "a never-synced tree claims nothing"
} else { fail "fresh tree: got '[ess::_tree_workgroups ess]'" }

# ---- the mismatched pair --------------------------------------------

puts "\nconfigured brown-sheinberg, pointed at the borrowed tree:"
set ess::registry_workgroup brown-sheinberg
set ess::system_path [file join $ROOT theirs]

refuses "tree-wide (what initial_sync hits at boot)" \
    { ess::_require_tree_workgroup ess sync } \
    "*belongs to workgroup jhu-monosov*brown-sheinberg*"

refuses "a system both trees have (would be displaced)" \
    { ess::_require_system_workgroup ess planko "sync planko" } \
    "*jhu-monosov*"

# The reason sync_base does not rely on the per-system guard alone. Their
# tree has no `detection` dir and so no manifest to disagree with -- the
# per-system check cannot see this one, and it is how OUR systems used to
# get written into THEIR tree, one clean "pull" at a time.
allows "a system only we have -- per-system guard cannot see it" \
    { ess::_require_system_workgroup ess detection "sync detection" }
refuses "...but the tree-wide guard still refuses it" \
    { ess::_require_tree_workgroup ess sync } "*jhu-monosov*"

# ---- the pairs that must keep working -------------------------------

puts "\npairs that must NOT be refused:"
set ess::system_path [file join $ROOT ours]
allows "our workgroup, our tree"            { ess::_require_tree_workgroup ess sync }
allows "our workgroup, our tree, one system" \
    { ess::_require_system_workgroup ess planko "sync planko" }

set ess::registry_workgroup jhu-monosov
set ess::system_path [file join $ROOT theirs]
allows "their workgroup, their tree (a legitimate borrow)" \
    { ess::_require_tree_workgroup ess sync }

set ess::registry_workgroup brown-sheinberg
set ess::system_path [file join $ROOT fresh]
allows "a never-synced tree (first provision)" \
    { ess::_require_tree_workgroup ess sync }

# An undeclared workgroup is the not-configured state, not a mismatch: it
# must gate nothing, or a rig with no registry could not sync at all.
set ess::registry_workgroup {}
set ess::system_path [file join $ROOT theirs]
allows "no workgroup declared gates nothing" \
    { ess::_require_tree_workgroup ess sync }

file delete -force $ROOT

if { $FAIL } { puts "\nFAILURES"; exit 1 }
puts "\nall checks passed"

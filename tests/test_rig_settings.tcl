#
# test_rig_settings.tcl
#
#  config/rig_settings.tcl -- the knobs MAIN declares because several interps
#  read them (docs/settings_panel_plan.md §3): stim host, registry url,
#  registry workgroup.
#
#  What makes this worth a test rather than a rig restart: the file runs on
#  the BOOT path of every rig, it WRITES local/rig.tcl the first time it
#  adopts a legacy ESS_RMT_HOST/ESS_REGISTRY_URL/ESS_WORKGROUP, and the
#  environment it exports is what every subprocess inherits. A mistake there
#  is a rig that boots pointing at the wrong stim2, or at no registry, with
#  the hand-edited file it used to read still sitting there looking correct.
#
#  Each scenario runs in its own CHILD INTERP with a fresh scratch $dspath,
#  because the module keeps its schema in namespace state -- one process
#  cannot honestly play "first boot" twice. dservSet and sendNoReply are
#  aliased back here so the fan-out can be inspected.
#
#  A UNIT test: plain tclsh, no server, no rig, no network.
#
#  Run as: tclsh tests/test_rig_settings.tcl   (or: ctest -R rig_settings)
#

set ::REPO [file normalize [file join [file dirname [info script]] ..]]

set FAIL 0
proc ok   { label } { puts "  ok   $label" }
proc fail { label } { puts "  FAIL $label"; set ::FAIL 1 }
proc check { label expected actual } {
    if { $expected eq $actual } {
        ok $label
    } else {
        fail "$label -- expected '$expected', got '$actual'"
    }
}
proc check_match { label pattern actual } {
    if { [string match $pattern $actual] } {
        ok $label
    } else {
        fail "$label -- '$actual' does not match '$pattern'"
    }
}

# what the child pushed, and what it published
proc _sent  { who script } { lappend ::SENT [list $who $script] }
proc _dp    { name value } { set ::DP($name) $value }

set ::SCRATCH [file join [file dirname [info script]] .rig_[pid]]

#
# One boot of one rig. `rigfile` is the local/rig.tcl content to start from
# ("" = the file does not exist yet), `envs` the legacy pre-*.tcl exports.
# Returns the child, still live, so a scenario can drive settings::put in it.
#
proc boot { name rigfile envs } {
    set root [file join $::SCRATCH $name]
    file delete -force $root
    file mkdir [file join $root local]
    if { $rigfile ne "" } {
        set fd [open [file join $root local rig.tcl] w]
        puts $fd $rigfile
        close $fd
    }

    set ::SENT {}
    array unset ::DP
    array set ::DP {}

    set ip [interp create]
    interp alias $ip sendNoReply {} _sent
    interp alias $ip dservSet    {} _dp
    $ip eval [list set ::dspath $root]
    $ip eval [list set ::REPO $::REPO]
    # what the binary stamps into every interp (TclServer.cpp); "dserv" is
    # main, and these knobs are main's by design
    $ip eval [list set ::dserv_interp dserv]
    $ip eval {
        tcl::tm::add [file join $::REPO lib]
        foreach v {ESS_RMT_HOST ESS_REGISTRY_URL ESS_WORKGROUP} {
            catch { unset ::env($v) }
        }
    }
    foreach {k v} $envs { $ip eval [list set ::env($k) $v] }
    $ip eval [list source [file join $::REPO config rig_settings.tcl]]
    return $ip
}

proc rigfile_of { ip } {
    set f [file join [$ip eval {set ::dspath}] local rig.tcl]
    if { ![file exists $f] } { return "" }
    set fd [open $f]; set txt [read $fd]; close $fd
    return $txt
}

proc sent_to { who } {
    set out {}
    foreach s $::SENT {
        if { [lindex $s 0] eq $who } { lappend out [lindex $s 1] }
    }
    return $out
}

file delete -force $::SCRATCH

#
# 1. FIRST BOOT of a rig still carrying local/pre-remote.tcl and
#    local/pre-registry.tcl. The legacy exports are adopted into rig.tcl and
#    the environment comes out identical to what it was -- the whole point:
#    the restart that migrates a rig must change nothing about how it runs.
#
puts "first boot, legacy env adopted"
set ip [boot adopt "" {
    ESS_RMT_HOST     192.168.88.50
    ESS_REGISTRY_URL https://dserv.net
    ESS_WORKGROUP    brown-sheinberg
}]
check "stim host exported unchanged" 192.168.88.50 [$ip eval {set ::env(ESS_RMT_HOST)}]
check "registry url exported unchanged" https://dserv.net \
    [$ip eval {set ::env(ESS_REGISTRY_URL)}]
check "workgroup exported unchanged" brown-sheinberg \
    [$ip eval {set ::env(ESS_WORKGROUP)}]
check "source is now file" file [$ip eval {settings::source_of stim host}]
check_match "rig.tcl gained the stim line" "*setting stim host 192.168.88.50*" [rigfile_of $ip]
check_match "rig.tcl gained the workgroup line" "*setting registry workgroup brown-sheinberg*" \
    [rigfile_of $ip]
check "knobs are declared in main" dserv [$ip eval {settings::interp_of stim host}]
interp delete $ip

#
# 2. SECOND BOOT: the declaration is authoritative even though the legacy
#    file is still there and still exports. Otherwise a rig would go on
#    obeying the file the operator was just told to stop editing.
#
puts "second boot, declaration beats the legacy export"
set ip [boot declared "setting stim host stimbox.local\nsetting registry workgroup other-lab" {
    ESS_RMT_HOST     192.168.88.50
    ESS_REGISTRY_URL https://dserv.net
    ESS_WORKGROUP    brown-sheinberg
}]
check "declared stim host wins" stimbox.local [$ip eval {set ::env(ESS_RMT_HOST)}]
check "declared workgroup wins" other-lab [$ip eval {set ::env(ESS_WORKGROUP)}]
check "undeclared url still adopted" https://dserv.net \
    [$ip eval {set ::env(ESS_REGISTRY_URL)}]
interp delete $ip

#
# 3. A rig with nothing declared and no legacy files behaves as it always
#    did: stim2 on this box, no registry. An empty default that quietly
#    became https://dserv.net would enroll every unconfigured rig.
#
puts "bare rig keeps today's behavior"
set ip [boot bare "" {}]
check "stim host defaults to localhost" localhost [$ip eval {set ::env(ESS_RMT_HOST)}]
check "registry url stays unset" 0 [$ip eval {info exists ::env(ESS_REGISTRY_URL)}]
check "workgroup stays unset" 0 [$ip eval {info exists ::env(ESS_WORKGROUP)}]
check "no rig.tcl was created" "" [rigfile_of $ip]
interp delete $ip

#
# 4. A declared EMPTY registry unsets a stale legacy export. The declaration
#    has to win in both directions or "no registry" is unsayable.
#
puts "declared empty clears a stale export"
set ip [boot cleared "setting registry url {}\nsetting registry workgroup {}" {
    ESS_REGISTRY_URL https://dserv.net
    ESS_WORKGROUP    brown-sheinberg
}]
check "url export cleared" 0 [$ip eval {info exists ::env(ESS_REGISTRY_URL)}]
check "workgroup export cleared" 0 [$ip eval {info exists ::env(ESS_WORKGROUP)}]
interp delete $ip

#
# 5. Validation. The errors are the documentation, so they are checked for
#    what they TEACH, not just for throwing.
#
puts "validation"
set ip [boot validate "" {}]
check "a URL is not a stim host" 1 \
    [catch { $ip eval {settings::put stim host http://192.168.88.50} } e]
check_match "and says so" "*give the host alone*" $e
check "empty stim host refused" 1 [catch { $ip eval {settings::put stim host {}} } e]
check_match "and names the alternative" "*localhost*" $e
check "a bare host becomes https" https://dserv.net \
    [$ip eval {settings::put registry url dserv.net}]
check "trailing slash trimmed" https://dserv.net \
    [$ip eval {settings::put registry url https://dserv.net/}]
check "empty registry url is legal" "" [$ip eval {settings::put registry url {}}]
check "empty workgroup is legal" "" [$ip eval {settings::put registry workgroup {}}]
check "a workgroup with a slash is refused" 1 \
    [catch { $ip eval {settings::put registry workgroup brown/sheinberg} } e]
check_match "and shows the shape" "*brown-sheinberg*" $e
interp delete $ip

#
# 6. The fan-out. A put has to reach the interps already running, or the
#    panel writes a value that only the next boot honors.
#
puts "apply fans out to live interps"
set ip [boot fanout "" {}]
set ::SENT {}
$ip eval {settings::put stim host 192.168.88.77}
check "stim told to reopen" "stimOpen 192.168.88.77" [lindex [sent_to stim] 0]
check_match "ess variable updated" "*::ess::rmt_host 192.168.88.77*" [lindex [sent_to ess] 0]
check_match "ess datapoint updated" "*dservSet ess/rmt_host*" [lindex [sent_to ess] 0]
check "main's own env updated" 192.168.88.77 [$ip eval {set ::env(ESS_RMT_HOST)}]

set ::SENT {}
$ip eval {settings::put registry workgroup brown-sheinberg}
check_match "trialsync's env pushed" "set ::env(ESS_WORKGROUP) brown-sheinberg" \
    [lindex [sent_to trialsync] 0]
check "configs reconfigured" 1 [expr {[llength [sent_to configs]] > 0}]
check "mesh gets the PAIR" 1 \
    [string match "mesh_configure * brown-sheinberg" [lindex [sent_to mesh] 0]]

set ::SENT {}
$ip eval {settings::put registry workgroup {}}
check_match "clearing unsets the child's env too" "catch {unset ::env(ESS_WORKGROUP)}" \
    [lindex [sent_to ess] 0]
interp delete $ip

#
# 7. Nothing here may be fatal on the boot path. A rig.tcl line that no
#    longer validates -- a workgroup that was legal under an older rule, say
#    -- costs a breadcrumb and a fallback, never the rest of dsconf.tcl.
#
puts "a bad line is not fatal"
set rc [catch {
    set ip [boot badline "setting registry workgroup {no spaces allowed}" {}]
} e]
check "boot survived" 0 $rc
if { !$rc } {
    check "fell back to the default" "" [$ip eval {settings::get registry workgroup}]
    check "and left a breadcrumb" 1 [expr {[llength [$ip eval {settings::errors}]] > 0}]
    interp delete $ip
}

file delete -force $::SCRATCH

if { $FAIL } {
    puts "FAILURES"
    exit 1
}
puts "all checks passed"

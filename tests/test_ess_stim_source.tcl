# test_ess_stim_source.tcl -- a stim script that fails to source on stim2
# must be REPORTED by configure_stim, never logged as "Loaded".
#
# Runs the real ess-2.0.tm (the source tree's, via ESS_TEST_ESS_LIB or the
# lib/ next to this tests/ dir) under the dlsh ess_test harness against a
# fake stim2: rmtOpen says connected, rmtSend answers screen queries with a
# number and fails the stim script itself with the "!TCL_ERROR <msg>" string
# the real rmtSend returns (it does not throw). Needs dlsh and a systems
# tree (~/systems/ess, remap/doublestep) -- ctest registers it only when
# both exist.
#
#   dlsh tests/test_ess_stim_source.tcl
#
# Background (2026-09-10): a stim2 whose dlsh.zip predated traj 1.1 rejected
# steps_stim.tcl at its `package require`; ESS reported "Loaded stimulus
# script" anyway and the previous protocol's stim stayed resident, so the
# steps run showed the doublestep protocol's red square target.

package require ess_test

set here [file dirname [file normalize [info script]]]
set ess_lib [expr {[info exists ::env(ESS_TEST_ESS_LIB)] ? $::env(ESS_TEST_ESS_LIB) : [file join [file dirname $here] lib]}]
set systems_root [file join $::env(HOME) systems ess]

proc ::rmtOpen {args} { return 1 }
proc ::rmtClose {args} {}
set ::rmt_fail 1
set ::rmt_log {}
proc ::rmtSend {script} {
    lappend ::rmt_log [string range $script 0 40]
    if { [string match "*screen_set*" $script] } { return 60 }
    if { $::rmt_fail && [string match "*proc nexttrial*" $script] } {
        return "!TCL_ERROR version conflict for package \"traj\": have 1.0, need 1.1"
    }
    return ""
}
ess_test::real_ess -systems_root $systems_root -ess_lib $ess_lib -autostub 1

set sys remap ; set proto doublestep ; set variant classic

# 1. stim not required: the system loads, the failure is a published WARNING
::ess::set_stim_required 0
set r [ess_test::load_system $sys $proto $variant]
set le [dservGet ess/load_error]
set se [dservGet ess/stim_script_error]
ess_test::assert {[dict get $r ok] == 1} "load still succeeds when stim is optional"
ess_test::assert {[string match {*"severity":"warning"*} $le]} "load_error carries a warning"
ess_test::assert {[string match {*"operation":"configure_stim"*} $le]} "load_error names configure_stim"
ess_test::assert {[string match "*${proto}_stim.tcl failed to load*" $se]} "stim_script_error names the file"
ess_test::assert {[string match {*previous protocol*} $se]} "message says the old stimulus is still showing"
ess_test::assert {[string match {*need 1.1*} $le]} "the remote error text is carried into the report"
ess_test::assert {[lsearch -glob $::rmt_log "*stim_init*"] < 0} "stim_init is NOT run after a failed source"

# 2. stim required: the load FAILS, load_error is an error, status restored
::ess::set_stim_required 1
set r [ess_test::load_system $sys $proto $variant]
set le [dservGet ess/load_error]
ess_test::assert {[dict get $r ok] == 0} "load fails when stim is required"
ess_test::assert {[string match {*"severity":"error"*} $le]} "load_error severity error"
ess_test::assert {[string match {*failed to load on*} $le]} "load_error names the stim failure"
ess_test::assert {[dservGet ess/status] eq "stopped"} "status restored to stopped"

# 3. a clean source clears the flag and runs stim_init
set ::rmt_fail 0
set ::rmt_log {}
::ess::set_stim_required 0
set r [ess_test::load_system $sys $proto $variant]
ess_test::assert {[dict get $r ok] == 1} "clean load ok"
ess_test::assert {[dservGet ess/stim_script_error] eq ""} "stim_script_error cleared on success"
ess_test::assert {[lsearch -glob $::rmt_log "*stim_init*"] >= 0} "stim_init runs after a clean source"

if { [ess_test::summary] != 0 } { exit 1 }
puts "all checks passed"

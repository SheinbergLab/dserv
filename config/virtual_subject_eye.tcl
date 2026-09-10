#
# virtual_subject_eye.tcl -- dserv subprocess that plays an EYE-MOVEMENT
# subject for fixation / saccade paradigms, so an eye-gated ESS system can be
# exercised end-to-end (and its datafile extracted) with no tracker.
#
# It drives the existing `virtual_eye` subprocess (virtualeyeconf.tcl):
# `send virtual_eye {set_eye h v}` parks the virtual gaze in degrees, and the
# em subprocess (em/settings source virtual) turns that into
# eyetracking/position, which feeds the windows processor -- the same path
# the real tracker takes, so fixation acquisition, holds, breaks and landings
# all happen for real in the state machine.
#
# Spawn + drive from dservctl (see the RUN RECIPE at the bottom):
#   dservctl -c 'subprocess vsubj_eye "source /usr/local/dserv/config/virtual_subject_eye.tcl"'
#   dservctl vsubj_eye {vseye::set_policy {world retino elsewhere world}}
#
# HOW IT KNOWS WHERE TO LOOK: on each state entry (ess/action_state ->
# "<state>_a") it asks the ess interp for the current trial's geometry by
# protocol VARIABLE name (::ess::get_variable), so it needs no stimdg
# knowledge -- only the variable names each system's protocols already keep
# (fix_x/fix2_x/target_x/retino_x for remap; fix_targ_x/y for pursuit).
# Moves are scheduled on a countdown off one always-on periodic tick (dserv
# one-shot timers do not fire callbacks -- see virtual_subject.tcl).
#
# PARADIGMS (dispatched on ess/system):
#   remap    fixon_a   -> gaze to the fixation spot
#            fixjump_a -> after jump_delay_ms, gaze to the (possibly moved) spot
#            cue_on_a  -> after resp_delay_ms, gaze to this trial's policy
#                         target: world | retino | elsewhere | hold
#                         (retino falls back to world on a no-remap trial;
#                          elsewhere = 5 deg from the fixation spot toward the
#                          screen center, outside every window; hold = never
#                          leave, so the trial times out as no_response)
#   pursuit  fixon_a   -> gaze to the fixation spot; the target is then
#            followed freely (reward is for completion, so the parked gaze
#            is enough to exercise the trial; set follow 1 to have the gaze
#            step to each vertex on TARGET SET for a steps protocol)
#
# OBSERVABILITY: puts is rerouted in a subprocess; tail $logpath instead.
#

package require dlsh
tcl::tm::add $dspath/lib
load $dspath/modules/dserv_timer[info sharedlibextension]
proc exit {args} { error "exit not available for this subprocess" }
errormon enable

namespace eval vseye {
    variable logfd      ""
    variable logpath    /tmp/virtual_subject_eye.log
    variable tick_ms    10
    variable jump_delay_ms 120     ;# after FIXSPOT SET, saccade to the moved spot
    variable resp_delay_ms 250     ;# after go, latency of the response saccade
    variable policy     {world retino elsewhere world}
    variable pidx       -1
    variable system     ""
    variable pending    {}         ;# list of {ticks_left h v label}
    variable nticks     0
    variable ntrials    0

    proc log { msg } {
        variable logfd
        if { $logfd ne "" } { catch { puts $logfd "[now] $msg" } }
    }

    proc set_policy { p } { variable policy $p ; dservSet vseye/policy $p ; log "POLICY $p" }

    proc action_for_trial {} {
        variable policy ; variable pidx
        if { $pidx < 0 } { return hold }
        return [lindex $policy [expr {$pidx % [llength $policy]}]]
    }

    # a protocol/system variable from the live ess interp
    proc var { name } { return [send ess [list ::ess::get_variable $name]] }

    proc gaze { h v label } {
        send virtual_eye [list set_eye $h $v]
        dservSet vseye/gaze [list $h $v]
        log "GAZE $label -> [format %.2f $h] [format %.2f $v]"
    }

    proc schedule { ms h v label } {
        variable pending ; variable tick_ms
        lappend pending [list [expr {int(round($ms/double($tick_ms)))}] $h $v $label]
        log "  scheduled $label in $ms ms"
    }

    proc on_tick {args} {
        variable pending ; variable nticks
        dservSet vseye/ticks [incr nticks]
        if { ![llength $pending] } { return }
        set keep {}
        foreach p $pending {
            lassign $p n h v label
            if { $n <= 0 } { gaze $h $v $label } else { lappend keep [list [expr {$n-1}] $h $v $label] }
        }
        set pending $keep
    }

    # --- remap (doublestep, shapes): fixate, follow the jump, then respond ---
    proc remap_state { state } {
        variable pidx ; variable pending ; variable jump_delay_ms ; variable resp_delay_ms
        switch -- $state {
            start_obs_a {
                incr pidx ; set pending {}
                dservSet vseye/action "obs $pidx -> [action_for_trial]"
                log "TRIAL obs=$pidx action=[action_for_trial]"
            }
            fixon_a   { gaze [var fix_x] [var fix_y] fix }
            fixjump_a { schedule $jump_delay_ms [var fix2_x] [var fix2_y] fix2 }
            cue_on_a {
                set fx [var fix2_x] ; set fy [var fix2_y]
                switch -- [action_for_trial] {
                    world  { schedule $resp_delay_ms [var target_x] [var target_y] world }
                    retino { schedule $resp_delay_ms [var retino_x] [var retino_y] retino }
                    elsewhere {
                        # 5 deg from the spot toward the screen center: out of
                        # the fixation window, nowhere near a target window
                        set dy [expr {$fy > 0 ? -5.0 : 5.0}]
                        schedule $resp_delay_ms $fx [expr {$fy + $dy}] elsewhere
                    }
                    default { log "  hold: no response this trial" }
                }
            }
        }
    }

    # --- pursuit (steps, pendulum, ...): acquire and hold the fixation spot ---
    proc pursuit_state { state } {
        variable pidx ; variable pending
        switch -- $state {
            start_obs_a { incr pidx ; set pending {} ; log "TRIAL obs=$pidx" }
            fixon_a     { gaze [var fix_targ_x] [var fix_targ_y] fix }
        }
    }

    proc on_state { dpoint data } {
        variable system
        if { $system eq "" } { catch { set system [dservGet ess/system] } }
        switch -- $system {
            remap   { remap_state $data }
            pursuit { pursuit_state $data }
            default { }
        }
    }

    proc on_system { dpoint data } { variable system $data ; log "SYSTEM $data" }

    proc init {} {
        variable tick_ms ; variable nticks ; variable logfd ; variable logpath
        variable pending ; variable pidx ; variable policy
        set nticks 0 ; set pending {} ; set pidx -1
        catch { close $logfd } ; set logfd ""
        catch { set logfd [open $logpath w] ; fconfigure $logfd -buffering line }
        log "INIT policy=$policy tick=${tick_ms}ms"
        dservRemoveAllMatches
        dservAddExactMatch ess/action_state
        dservAddExactMatch ess/system
        dpointSetScript ess/action_state [namespace current]::on_state
        dpointSetScript ess/system       [namespace current]::on_system
        timerPrefix vseyeTimer
        dservAddExactMatch vseyeTimer/0
        dpointSetScript vseyeTimer/0 [namespace current]::on_tick
        timerTickInterval $tick_ms $tick_ms
        # make sure the virtual eye is publishing
        catch { send virtual_eye start }
        gaze 0.0 0.0 init
        dservSet vseye/ready 1
        dservSet vseye/policy $policy
        puts "virtual_subject_eye ready: policy = $policy"
    }
}

vseye::init

#
# RUN RECIPE (all from a shell; setup while STOPPED):
#   1. dservctl -c 'subprocess vsubj_eye "source /usr/local/dserv/config/virtual_subject_eye.tcl"'
#      To reprogram a LIVE one:  dservctl vsubj_eye "source <same path>"
#      (a second `subprocess` with an existing name does NOT re-source it,
#      so init -- and its `send virtual_eye start` -- never reruns; if the
#      publisher was stopped, fixation is never seen and every trial aborts.
#      Check: dservGet eyetracking/virtual_enabled must be 1.)
#   2. run a config from the MAIN interp:  dservctl -c 'send configs {queue_run_config <name>}'
#      then Go:                             dservctl ess '::ess::start'
#      (or load_system + file_open + start by hand)
#   3. tail /tmp/virtual_subject_eye.log ; dservctl -c 'dservGet ess/obs_id'
#   4. the queue closes the datafile when the stimdg is exhausted; the df
#      subprocess then writes <base>.obs.dgz and runs the extractors.
#

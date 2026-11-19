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
#   transsacc  real main-sequence saccades from the virtual eye (see
#            transsacc_state below): to the spot on fixon, to the target on
#            target_on after a jittered latency, then a report (buttons or
#            touch, whichever the protocol reads) per
#            ts_policy (suppress | correct | wrong | random | hold | nosac).
#            Tune with  vseye::ts_set lat_ms 200 gain 0.9 overshoot 0.1 ...
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

    # run script after ms, counted on the tick
    proc after_ms { ms script label } {
        variable pending ; variable tick_ms
        lappend pending [list [expr {int(round($ms/double($tick_ms)))}] $script]
        log "  scheduled $label in $ms ms"
    }

    proc schedule { ms h v label } {
        after_ms $ms [list [namespace current]::gaze $h $v $label] $label
    }

    proc on_tick {args} {
        variable pending ; variable nticks
        dservSet vseye/ticks [incr nticks]
        if { ![llength $pending] } { return }
        set keep {} ; set due {}
        foreach p $pending {
            lassign $p n script
            if { $n <= 0 } { lappend due $script } else { lappend keep [list [expr {$n-1}] $script] }
        }
        set pending $keep
        foreach script $due {
            if { [catch $script err] } { log "  ERROR $err" }
        }
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

    # --- transsacc (displace): real saccades, and a report ---------------
    #
    # Every eye movement here is a main-sequence saccade from the virtual
    # eye (`saccade`), not a jump, so the saccade detector sees what it
    # would see from a tracker.  The time is set by the eye itself
    # (-delay), exactly, not by this process's tick.
    #
    # The report follows `ts_policy`, one word per trial, cycled:
    #   suppress  a model of saccadic suppression of displacement: an intra
    #             step goes unseen (random answer), a blank or late step is
    #             seen (correct answer).  The data should then show the
    #             intra/blank/late split the paradigm looks for.
    #   correct | wrong | random   regardless of mode
    #   hold      no report: the trial ends as no_response
    #   nosac     never leave fixation: the trial aborts as nosaccade
    variable ts_policy       {suppress}
    variable ts_fix_lat_ms   150       ;# fixon -> saccade to the spot
    variable ts_lat_ms       180       ;# target on -> saccade (mean)
    variable ts_lat_jitter   40        ;# +/- uniform, ms
    variable ts_gain         0.95      ;# landing gain (hypometric)
    variable ts_scatter      0.3       ;# landing scatter sd, deg
    variable ts_overshoot    0.0       ;# dynamic overshoot fraction
    variable ts_resp_ms      400       ;# response window on -> report

    proc ts_set { args } {
        foreach {k v} $args {
            variable ts_$k
            if { ![info exists ts_$k] } { error "no setting ts_$k" }
            set ts_$k $v
        }
        log "TS $args"
    }

    proc ts_action {} {
        variable ts_policy ; variable pidx
        if { $pidx < 0 } { return hold }
        return [lindex $ts_policy [expr {$pidx % [llength $ts_policy]}]]
    }

    proc nrand { sd } {
        set u [expr {max(1e-12, rand())}]
        return [expr {$sd*sqrt(-2.0*log($u))*cos(6.283185307179586*rand())}]
    }

    proc look { delay_ms h v label {overshoot 0.0} } {
        set t [send virtual_eye [list saccade $h $v -delay $delay_ms \
                                     -overshoot $overshoot]]
        dservSet vseye/gaze [list $h $v]
        log "SACCADE $label -> [format %.2f $h] [format %.2f $v] in $delay_ms ms ($t)"
    }

    # the report on whatever the protocol reads: its buttons (press and
    # release in one send, as virtual_subject.tcl does), else a touch pad
    proc report { which } {
        if { [var use_buttons] || [var use_joystick] } {
            send ess "::ess::button_simulate $which 1; ::ess::button_simulate $which 0"
            log "REPORT button $which"
        } else {
            send ess [list ::ess::touch_win_simulate $which]
            log "REPORT window $which"
        }
    }

    proc transsacc_state { state } {
        variable pidx ; variable pending
        variable ts_fix_lat_ms ; variable ts_lat_ms ; variable ts_lat_jitter
        variable ts_gain ; variable ts_scatter ; variable ts_overshoot
        variable ts_resp_ms
        switch -- $state {
            start_obs_a {
                incr pidx ; set pending {}
                dservSet vseye/action "obs $pidx -> [ts_action]"
                log "TRIAL obs=$pidx action=[ts_action]"
            }
            fixon_a {
                look $ts_fix_lat_ms [var fix_x] [var fix_y] fix
            }
            target_on_a {
                if { [ts_action] eq "nosac" } { log "  nosac: stay on fixation" ; return }
                set lat [expr {$ts_lat_ms + $ts_lat_jitter*(2.0*rand() - 1.0)}]
                set fx [var fix_x] ; set fy [var fix_y]
                set tx [var target_x] ; set ty [var target_y]
                set h [expr {$fx + $ts_gain*($tx - $fx) + [nrand $ts_scatter]}]
                set v [expr {$fy + $ts_gain*($ty - $fy) + [nrand $ts_scatter]}]
                look [expr {int(round($lat))}] $h $v target $ts_overshoot
            }
            response_on_a {
                set a [ts_action]
                set want [var correct_resp]
                if { $a eq "suppress" } {
                    set a [expr {[var mode] eq "intra" ? "random" : "correct"}]
                }
                if { $want < 0 && $a in {correct wrong} } { set a random }
                switch -- $a {
                    correct { set w $want }
                    wrong   { set w [expr {1 - $want}] }
                    random  { set w [expr {int(rand()*2)}] }
                    default { log "  $a: no report" ; return }
                }
                after_ms $ts_resp_ms [list [namespace current]::report $w] "report $w ($a)"
            }
        }
    }

    proc on_state { dpoint data } {
        variable system
        if { $system eq "" } { catch { set system [dservGet ess/system] } }
        switch -- $system {
            remap     { remap_state $data }
            pursuit   { pursuit_state $data }
            transsacc { transsacc_state $data }
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

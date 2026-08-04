#
# virtual_subject_colormatch.tcl -- a virtual subject whose HANDS are real
# extio output pins (match_to_sample/colormatch, button-response version)
#
# virtual_subject.tcl injects with ::ess::button_simulate -- a software tap
# directly into the ess button layer. THIS subject instead presses buttons by
# scheduling REAL DO pulses on an extio box whose outputs are loopback-wired
# to the DI pins of the box's "response" chord group, so every press travels
# the full production input path:
#
#   box at-scheduler -> DO edge -> jumper wire -> DI IRQ (us-stamped)
#     -> chord group event -> ess button dispatcher -> ess/button/<chan>
#     -> state machine transition
#
# Wiring assumed (office box "box", extio_test loopback map):
#   out 8  -> in 12  labeled resp_left   (button chan 0 via group "response")
#   out 7  -> in 13  labeled resp_right  (button chan 1 via group "response")
# plus rig bindings in the ess interp:
#   ::ess::button_bind 0 {} box {box response left}
#   ::ess::button_bind 1 {} box {box response right}
#
# Spawn:
#   dservctl -c 'subprocess virtual_subject "source <path>/virtual_subject_colormatch.tcl"'
# Re-source a live instance to reprogram (init re-subscribes cleanly).
#
# The subject is EVENT-DRIVEN (evtSetScript is a TclServer builtin, so any
# subprocess can watch the event stream the way vizconf does): STIMTYPE tells
# it the trial row, CHOICES ON cues a press scheduled rt_ms later via
# ::ess::box_schedule_pulse evaluated in the ess interp (obs-relative at
# command; the box fires the edge with us precision). The policy cycles
# correct / incorrect / no-response so every outcome cell appears in the
# datafile. It reads ONLY stimdg + the event stream -- nothing internal to
# the state machine -- so it can't cheat.
#

package require dlsh
tcl::tm::add $dspath/lib
proc exit {args} { error "exit not available for this subprocess" }
errormon enable

namespace eval vsubj {
    variable logfd    ""
    variable logpath  /tmp/virtual_subject_colormatch.log
    variable box      box     ;# extio device name
    variable pin_left  8      ;# DO wired to the resp_left DI
    variable pin_right 7      ;# DO wired to the resp_right DI
    variable rt_base_ms 280   ;# press latency after CHOICES ON ...
    variable rt_jit_ms  160   ;# ... plus uniform jitter
    # 8-trial cycle: 5 correct, 2 incorrect, 1 no-response
    variable policy {correct correct incorrect correct correct noresp correct incorrect}
    variable trial_n  -1      ;# trials seen (policy index)
    variable row      -1      ;# current stimdg row (from STIMTYPE)
    variable armed    0       ;# a press is scheduled for this trial

    proc log { msg } {
        variable logfd
        if { $logfd ne "" } { catch { puts $logfd "[clock format [clock seconds] -format %T] $msg" } }
    }

    # --- event-name resolution (the vizconf evtSetScriptByName recipe) ------
    proc evt_bind { type_name subtype_name script } {
        set types [dservGet ess/evt_type_ids]
        if { ![dict exists $types $type_name] } {
            error "unknown event type '$type_name'"
        }
        set type_id [dict get $types $type_name]
        set subtype_id -1
        if { $subtype_name ne "*" } {
            set subs [dservGet ess/evt_subtype_ids]
            if { [dict exists $subs $type_name $subtype_name] } {
                set subtype_id [dict get $subs $type_name $subtype_name]
            } else {
                error "unknown subtype '$subtype_name' for '$type_name'"
            }
        }
        evtSetScript $type_id $subtype_id $script
    }

    # --- per-trial flow -----------------------------------------------------
    proc action_for_trial {} {
        variable policy; variable trial_n
        if { $trial_n < 0 } { return correct }
        return [lindex $policy [expr {$trial_n % [llength $policy]}]]
    }

    proc on_stimtype { type subtype data } {
        variable row; variable trial_n; variable armed
        set row $data
        incr trial_n
        set armed 0
        dservSet vsubj/action "trial $trial_n row $row -> [action_for_trial]"
        log "TRIAL n=$trial_n row=$row action=[action_for_trial]"
    }

    # CHOICES ON: decide which button is correct purely from stimdg geometry
    # (match_x vs nonmatch_x) + reward_rule, then schedule the press on the
    # box. resp 0 = left button, 1 = right (colormatch responded contract).
    proc on_choices_on { type subtype data } {
        variable row; variable armed; variable box
        variable pin_left; variable pin_right
        variable rt_base_ms; variable rt_jit_ms
        if { $row < 0 || $armed } { return }
        set act [action_for_trial]
        if { $act eq "noresp" } {
            dservSet vsubj/press "trial_row $row noresp"
            log "  noresp: letting response_timeout expire"
            return
        }
        set geom [send ess "list \[dl_get stimdg:match_x $row\] \[dl_get stimdg:nonmatch_x $row\] \[dl_get stimdg:reward_rule $row\]"]
        lassign $geom match_x nonmatch_x rule
        # button that yields a CORRECT outcome under the reward rule
        if { $rule eq "nonmatch" } {
            set good [expr {$match_x < $nonmatch_x ? 1 : 0}]
        } else {
            set good [expr {$match_x < $nonmatch_x ? 0 : 1}]
        }
        set resp [expr {$act eq "correct" ? $good : 1 - $good}]
        set pin  [expr {$resp == 0 ? $pin_left : $pin_right}]
        set rt   [expr {$rt_base_ms + int(rand() * $rt_jit_ms)}]
        set ok   [send ess "::ess::box_schedule_pulse $box $pin $rt"]
        set armed 1
        dservSet vsubj/press "trial_row $row act $act resp $resp pin $pin rt_ms $rt sched $ok"
        log "  press: act=$act resp=$resp pin=$pin rt=${rt}ms sched=$ok (rule=$rule match_x=$match_x nonmatch_x=$nonmatch_x)"
    }

    proc on_endobs { type subtype data } {
        variable row
        set row -1
    }

    proc init {} {
        variable logfd; variable logpath; variable policy
        catch { close $logfd }; set logfd ""
        catch { set logfd [open $logpath w]; fconfigure $logfd -buffering line }
        log "INIT policy=$policy"
        # the event stream is a datapoint: evtSetScript handlers only fire
        # for events that ARRIVE here, so subscribe (the vizconf recipe)
        dservAddExactMatch eventlog/events
        # event subscriptions (idempotent: evtSetScript replaces per type/sub)
        evt_bind STIMTYPE * [namespace current]::on_stimtype
        evt_bind CHOICES ON [namespace current]::on_choices_on
        evt_bind ENDOBS  *  [namespace current]::on_endobs
        dservSet vsubj/ready 1
        dservSet vsubj/policy $policy
        puts "virtual_subject_colormatch ready: policy = $policy"
    }
}

vsubj::init

#
# RUN RECIPE (.50 office rig, box "box" as obs leader):
#   1. box:  group "response" pins 12,13 labeled resp_left/resp_right;
#            outs 8->12, 7->13 jumpered (extio_test loop wiring)
#   2. ess:  ::ess::button_bind 0 {} box {box response left}
#            ::ess::button_bind 1 {} box {box response right}
#   3. spawn this subprocess; load match_to_sample colormatch easy
#   4. ::ess::obs_schedule_bind auto auto 80   (box-driven BEGINOBS)
#   5. params: sample_time 600, delay_time 500, response_timeout 3000
#   6. file_open + start; watch /tmp/virtual_subject_colormatch.log
#

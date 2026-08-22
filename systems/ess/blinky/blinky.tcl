#
#  SYSTEM
#    blinky
#
#  DESCRIPTION
#    The smallest system that is still a real one: a spot on the stimulus
#    display that blinks, an observation per burst, and a live parameter to
#    stop and start the blinking while it runs.
#
#    It exists so a FRESH install has something that loads. A rig with no
#    systems synced from the registry yet -- a new box, an offline box, a
#    box being brought up -- should come up working rather than empty, and
#    should give whoever is bringing it up something that proves the whole
#    chain end to end: state machine -> stim2 -> events -> the viz panel in
#    ess_control. Nothing here needs hardware: no juice, no buttons, no eye.
#
#    It is also the shortest worked example of the shape every other system
#    has, which is why it is commented more heavily than its size warrants.
#

package require ess

namespace eval blinky {
    proc create {} {
        set sys [::ess::create_system [namespace tail [namespace current]]]

        ######################################################################
        #                          System Parameters                         #
        ######################################################################

        $sys add_param interblock_time 500 time int
        $sys add_param n_obs_total      20 variable int

        ##
        ## Local variables
        ##
        $sys add_variable n_obs 20
        $sys add_variable obs_count 0
        $sys add_variable stimtype 0
        $sys add_variable blinks_left 0
        $sys add_variable first_time 1
        $sys add_variable finale_delay 300

        ######################################################################
        #                            System States                           #
        ######################################################################

        $sys set_start start

        $sys add_state start {} { return inter_obs }

        #
        # inter_obs -- the gap between observations
        #
        $sys add_action inter_obs {
            set n_obs [my n_obs]
            if { !$first_time } { set delay $interblock_time } \
                else { set first_time 0; set delay 0 }
            timerTick $delay
            my nexttrial
        }
        $sys add_transition inter_obs {
            if { [my finished] } { return pre_finale }
            if { [timerExpired] } { return start_obs }
        }

        #
        # start_obs
        #
        $sys add_action start_obs {
            ::ess::begin_obs $obs_count $n_obs
            set blinks_left [my n_blinks]
            # every system logs which stimulus this observation is, and the
            # viz reads it to colour the spot the same as the display --
            # without it the panel has no idea which row of stimdg is live
            ::ess::evt_put STIMTYPE STIMID [now] $stimtype
        }
        $sys add_transition start_obs {
            # a box-driven BEGINOBS may still be in flight; hold until it
            # lands, or the first blink is timed against the wrong onset
            if { [::ess::obs_onset_pending] } { return }
            return blink_on
        }

        #
        # blink_on / blink_off -- the whole task
        #
        # `blink` is a LIVE param: turned off mid-run the machine keeps
        # cycling and the spot simply stays dark, which is what makes it a
        # usable smoke test ("is anything still running?") rather than a
        # thing you have to stop and restart.
        #
        $sys add_action blink_on {
            if { $blink } {
                my stim_on
                ::ess::evt_put PATTERN ON [now]
            }
            timerTick $on_ms
        }
        $sys add_transition blink_on {
            if { [timerExpired] } { return blink_off }
        }

        $sys add_action blink_off {
            my stim_off
            ::ess::evt_put PATTERN OFF [now]
            timerTick $off_ms
            incr blinks_left -1
        }
        $sys add_transition blink_off {
            if { ![timerExpired] } { return }
            if { $blinks_left > 0 } { return blink_on }
            return post_trial
        }

        #
        # post_trial / finish
        #
        $sys add_action post_trial {
            # correct/rt are meaningless here; log the trial anyway so the
            # datafile has the same shape every other system produces
            ::ess::save_trial_info 1 0 $stimtype
        }
        $sys add_transition post_trial { return finish }

        $sys add_action finish {
            my endobs
            ::ess::end_obs COMPLETE
        }
        $sys add_transition finish { return inter_obs }

        #
        # pre_finale / finale / end
        #
        $sys add_action pre_finale { timerTick $finale_delay }
        $sys add_transition pre_finale {
            if { [timerExpired] } { return finale }
        }
        $sys add_action finale { my finale }
        $sys add_transition finale { return end }

        $sys set_end {}

        ######################################################################
        #                         System Callbacks                           #
        ######################################################################

        $sys set_init_callback   { ::ess::init }
        $sys set_deinit_callback {}
        $sys set_reset_callback  {
            set n_obs [my n_obs]
            set obs_count 0
        }
        $sys set_start_callback  { set first_time 1 }
        $sys set_quit_callback   { ::ess::end_obs QUIT }
        $sys set_end_callback    {}
        $sys set_file_open_callback  {}
        $sys set_file_close_callback {}
        $sys set_subject_callback    {}

        ######################################################################
        #                          System Methods                            #
        ######################################################################

        $sys add_method n_obs {} { return $n_obs_total }

        $sys add_method n_blinks {} {
            set n 3
            catch { set n [dl_get stimdg:n_blinks $stimtype] }
            return $n
        }

        $sys add_method nexttrial {} {
            set stimtype $obs_count
            if { [dg_exists stimdg] } {
                catch { rmtSend "nexttrial $stimtype" }
            }
        }

        $sys add_method stim_on  {} { catch { rmtSend "blink_on" } }
        $sys add_method stim_off {} { catch { rmtSend "blink_off" } }

        $sys add_method finished {} {
            return [expr {$obs_count >= $n_obs}]
        }

        $sys add_method endobs {} { incr obs_count }
        $sys add_method finale {} { catch { rmtSend "reset" } }

        return $sys
    }
}

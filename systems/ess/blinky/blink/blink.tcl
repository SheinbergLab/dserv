#
# PROTOCOL
#   blinky blink
#
# DESCRIPTION
#   One spot, blinking. The protocol owns the stim connection, the live
#   parameters, and the experimenter view.
#

namespace eval blinky::blink {
    variable params_defaults { nr 20 }

    proc protocol_init { s } {
        $s set_protocol [namespace tail [namespace current]]

        $s add_param rmt_host $::ess::rmt_host stim ipaddr

        #
        # LIVE PARAMS -- the point of the demo.
        #
        # `blink` is the on/off switch: flip it in ESS Control while the
        # system runs and the spot stops and starts. It has an -apply,
        # because a live param without one only stores a value; the running
        # trial would keep whatever it read at init. Here apply pushes the
        # state so the viz panel follows immediately rather than at the next
        # blink boundary.
        #
        # [namespace current], not a hardcoded ::blinky::... -- ESS sources
        # system files INSIDE ::ess, so this namespace is really
        # ::ess::blinky::blink and an absolute name written the obvious way
        # points at a proc that does not exist. The failure is silent: the
        # param stores and logs, and only the apply never runs.
        $s add_live_param blink   1   bool [list [namespace current]::apply_blink]
        $s add_live_param on_ms   400 int
        $s add_live_param off_ms  400 int

        $s set_protocol_init_callback {
            ::ess::init

            # open a connection to stim2 and upload ${protocol}_stim.tcl.
            # Everything visual below runs THERE, not here.
            my configure_stim $rmt_host
        }

        $s set_protocol_deinit_callback {
            catch { rmtClose }
        }

        $s set_reset_callback {
            set obs_count 0
            catch { rmtSend reset }
        }

        $s set_start_callback { set first_time 1 }
        $s set_quit_callback  { catch { rmtSend reset } }
        $s set_end_callback   { catch { rmtSend reset } }

        $s set_file_open_callback  {}
        $s set_file_close_callback {}

        ##################################################################
        #                 Experimenter Visualization                     #
        #                                                                #
        #  The same spot, in the ESS Control viz panel: filled while the  #
        #  stimulus is on, an outline while it is off. That is the whole  #
        #  point of the bootstrap system -- one glance says the state     #
        #  machine is running, stim2 is being driven, and events are      #
        #  reaching the panel, which are three different things that all  #
        #  look identical when they are broken.                           #
        ##################################################################

        $s set_viz_config {
            variable on 0
            variable trial -1
            variable count 0

            proc setup {} {
                evtSetScript 28 -1 [namespace current]::pattern    ;# PATTERN
                evtSetScript 29 -1 [namespace current]::stimtype   ;# STIMTYPE
                evtSetScript 7  0  [namespace current]::stopped    ;# STOPPED
                evtSetScript 3  2  [namespace current]::reset      ;# USER RESET

                clearwin
                setbackground [dlg_rgbcolor 25 25 25]
                # 16x9, like every other viz in the tree (extio_test's
                # loopback is the reference). The window sets the scale for
                # everything below: a -size in user units means one thing in
                # a 16-wide window and something quite different in a
                # 20-wide one, which is how the spot ended up filling the
                # panel while the text looked microscopic.
                setwindow 0 0 16 9
                draw_
            }

            proc draw_ {} {
                variable on; variable trial; variable count
                clearwin
                # filled when on, a ring when off -- readable at a glance
                # from across the room, which is what this view is for
                # dlg_markers, not a circle proc -- and -size is a
                # DIAMETER in user units when paired with -scaletype x
                # (docs/dlsh_idioms.md). fcircle filled / circle hollow.
                set m [expr {$on ? "fcircle" : "circle"}]
                set c [expr {$on ? [my_color] : "gray"}]
                dlg_markers 8 5.2 -marker $m -size 3 -scaletype x -color $c
                dlg_text 8 8.2 "blinky" -color white -size 14 -just 0
                dlg_text 8 1.6 "obs $trial   blinks $count" \
                    -color gray -size 12 -just 0
                flushwin
            }

            proc pattern  { t s d } {
                variable on; variable count
                set on $s
                if { $s } { incr count }
                draw_
            }
            proc stimtype { t s d } { variable trial; set trial $d; draw_ }
            proc stopped  { t s d } { variable on 0; draw_ }
            proc reset    { t s d } {
                variable on 0; variable count 0; variable trial -1
                draw_
            }

            # the trial's colour, read from stimdg so the panel and the
            # display cannot disagree about what is being shown
            proc my_color {} {
                variable trial
                if { $trial < 0 } { return gray }
                # the same three numbers the stim draws with. Passing the
                # NAME here worked only for names dlsh happens to know:
                # `green` drew, `amber` drew nothing at all.
                if { [catch {
                    set r [dl_get stimdg:blink_r $trial]
                    set g [dl_get stimdg:blink_g $trial]
                    set b [dl_get stimdg:blink_b $trial]
                } ] } { return green }
                return [dlg_rgbcolor [expr {int(255*$r)}] \
                            [expr {int(255*$g)}] [expr {int(255*$b)}]]
            }

            setup
        }

        return
    }

    #
    # -apply for the `blink` live param. Runs at global scope with the new
    # value; keep it short and never throw -- it fires from the settings
    # path, mid-trial.
    #
    proc apply_blink { v } {
        catch { dservSet blinky/blink $v }
        if { !$v } { catch { rmtSend "blink_off" } }
        return
    }
}

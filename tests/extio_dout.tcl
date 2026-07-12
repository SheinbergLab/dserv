#
# extio_dout.tcl -- latency of driving an extio box output OVER DSERV,
#                   relative to a directly-wired local (RPi) GPIO.
#
# METHOD
#   On every tick we flip two lines at (essentially) the same instant:
#     * a LOCAL RPi output, driven directly    -> gpioLineSetValue
#     * an EXTIO output, driven over dserv      -> dservSet extio/<box>/cmd/do/<n>
#   The extio box senses BOTH resulting edges on two of its inputs and stamps
#   each one with its OWN clock at the input interrupt. Because both timestamps
#   come from the same box clock, their difference cancels host-side USB read
#   jitter (and needs no obs/clock-sync -- a constant box-clock offset cancels).
#   What is left is purely the extra latency of the dserv->extio command path:
#
#     delay = ts(state/di/<loopback_in>) - ts(state/di/<host_in>)   [microseconds]
#
# WIRING  (extio box named by ::do_test::box, default "pico")
#     RPi GPIO <host_out_pin>       ---->  extio pin <host_in_pin>       (reference edge)
#     extio pin <loopback_out_pin>  ---->  extio pin <loopback_in_pin>   (path-under-test, loopback)
#   The two input pins are set to equal (zero) debounce below so their input-
#   sense latency cancels in the difference.
#
# RUN
#     source extio_dout.tcl
#     do_test::setup           ;# configure box pins + wire the captures
#     do_test::run 200         ;# 200 toggles at 100 ms; timer-driven, returns at once
#     ...wait ~ n*every ms...
#     do_test::results         ;# -> per-direction n / mean / median / min / max (us)
#     do_test::teardown        ;# unwire; leave the RPi line low
#   Just re-run() to take a fresh set (it resets the buffers first).
#
# REPRESENTATIVE NUMBER
#   results reports STEADY-STATE latency: it drops the first and last toggle
#   (results {drop N} to trim more; {drop 0} for raw). Those endpoints are
#   issued into an IDLE trigger pipeline and run much faster than the rest --
#   e.g. a setup/first edge can read ~130 us while mid-run toggles sit ~315 us.
#   The steady-state figure is the honest one: in a real experiment, commands
#   to extio are issued from ess state-machine / timer callbacks (trigger
#   scripts) while the interp is busy, i.e. the loaded path, not the idle one.
#   The gap between the two is dserv-side trigger-thread scheduling, not the wire.
#
namespace eval do_test {
    variable box   pico          ;# extio device name -> extio/<box>/...
    variable timer_id 7          ;# dserv software-timer slot

    # -- pins --
    variable host_out_pin     27 ;# LOCAL RPi output, driven directly (the reference)
    variable host_in_pin      10 ;# extio input wired to host_out_pin
    variable loopback_out_pin 11 ;# extio output, driven over dserv (the path under test)
    variable loopback_in_pin  12 ;# extio input wired to loopback_out_pin

    variable state 0
    variable count 0
    variable start 0

    # Configure the box pins this test needs (transient; not saved to flash) and
    # wire a capture on each input edge. dservSet is asynchronous, so give the
    # box a moment to apply the config before the first run().
    proc setup {} {
        variable box; variable state 0
        variable host_out_pin; variable host_in_pin
        variable loopback_out_pin; variable loopback_in_pin

        dservSet extio/$box/config/pin/$loopback_out_pin/mode out
        foreach p [list $host_in_pin $loopback_in_pin] {
            dservSet extio/$box/config/pin/$p/mode in
            dservSet extio/$box/config/pin/$p/debounce_ms 0   ;# so sense latency cancels
        }

        # Local RPi output; start both lines low. (No output-release proc exists,
        # so a re-request after a prior run is tolerated.)
        catch { gpioLineRequestOutput $host_out_pin }
        dservSet extio/$box/cmd/do/$loopback_out_pin $state
        gpioLineSetValue $host_out_pin $state

        foreach pin [list $host_in_pin $loopback_in_pin] {
            dservAddExactMatch extio/$box/state/di/$pin
            dpointSetScript    extio/$box/state/di/$pin do_test::process
        }
        reset
    }

    # Fresh sample buffers. ts_<pin> = box-clock edge time (us, relative to start;
    # only differences are meaningful). val_<pin> = edge level (1 rising, 0 falling).
    proc reset {} {
        variable start; variable host_in_pin; variable loopback_in_pin
        if { [dg_exists pin_test] } { dg_delete pin_test }
        dg_create pin_test
        foreach pin [list $host_in_pin $loopback_in_pin] {
            dl_set pin_test:ts_$pin  [dl_flist]
            dl_set pin_test:val_$pin [dl_ilist]
        }
        set start [now]
        return
    }

    # Box-published input edge -> record the box-clock timestamp and the level.
    proc process { dpoint data } {
        variable start
        set pin [file tail $dpoint]
        dl_append pin_test:ts_$pin  [expr {[dservTimestamp $dpoint] - $start}]
        dl_append pin_test:val_$pin [expr {$data ? 1 : 0}]
    }

    # Flip both outputs ~simultaneously. dserv command first, so the path under
    # test is not inflated by the (sub-microsecond) local GPIO write.
    proc on_timer { args } {
        variable box; variable state; variable count; variable timer_id
        variable loopback_out_pin; variable host_out_pin
        set count [expr {max(0, $count - 1)}]
        if { $count == 0 } { timerStop $timer_id }
        set state [expr {1 - $state}]
        dservSet extio/$box/cmd/do/$loopback_out_pin $state
        gpioLineSetValue $host_out_pin $state
    }

    proc run { n {every 100} } {
        variable timer_id; variable count
        reset      ;# discard any setup priming edge captured before now -> clean start
        set count $n
        timerTickInterval $timer_id $every $every
        timerSetScript    $timer_id do_test::on_timer
        return "running $n toggles at ${every} ms"
    }

    # Paired box-clock delay of the dserv-commanded edge vs the local edge (us).
    # Reports the STEADY-STATE latency by trimming `drop` toggles from each end:
    # the first warms up and the last drains into an idle trigger pipeline, so
    # both understate what a command issued from live experiment code costs.
    # Pass `drop 0` to see every sample (endpoints included). Split by direction
    # since rise vs fall can differ.
    proc results { {drop 1} } {
        variable host_in_pin; variable loopback_in_pin
        set n [dl_length pin_test:ts_$host_in_pin]
        if { $n != [dl_length pin_test:ts_$loopback_in_pin] } {
            error "edge-count mismatch: an edge was dropped or debounced, so\
                   index-pairing is invalid -- reset and rerun (or slow the rate)"
        }
        if { $n <= 2 * $drop } {
            error "only $n samples -- need more than [expr {2 * $drop}] to drop $drop from each end"
        }
        dl_local keep [dl_fromto $drop [expr {$n - $drop}]]   ;# keep middle indices
        dl_local ref  [dl_choose pin_test:ts_$host_in_pin     $keep]
        dl_local cmd  [dl_choose pin_test:ts_$loopback_in_pin  $keep]
        dl_local dir  [dl_choose pin_test:val_$host_in_pin     $keep]
        dl_local d    [dl_sub $cmd $ref]                       ;# us, one per toggle

        set out "steady-state ([dl_length $d] of $n toggles; dropped $drop each end)"
        append out "\n[_fmt all $d]"
        foreach {label level} {rising 1 falling 0} {
            dl_local s [dl_select $d [dl_eq $dir $level]]
            if { [dl_length $s] } { append out "\n[_fmt $label $s]" }
        }
        return $out
    }

    proc _fmt { label d } {
        return [format "%-7s n=%d  mean=%.1f  median=%.1f  min=%.1f  max=%.1f  us" \
            $label [dl_length $d] [dl_mean $d] [dl_median $d] [dl_min $d] [dl_max $d]]
    }

    # Stop the timer, drop the capture scripts/matches, and leave the RPi line low.
    proc teardown {} {
        variable box; variable timer_id; variable host_out_pin
        variable host_in_pin; variable loopback_in_pin
        catch { timerStop $timer_id }
        foreach pin [list $host_in_pin $loopback_in_pin] {
            catch { dpointSetScript extio/$box/state/di/$pin {} }
            catch { dservRemoveMatch extio/$box/state/di/$pin }
        }
        catch { gpioLineSetValue $host_out_pin 0 }
        return
    }
}

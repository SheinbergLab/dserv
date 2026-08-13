#
# virtual_extio.tcl -- dserv subprocess that plays "extio box" (name: vbox)
#
# A virtual extio box for headless end-to-end testing of extio_test (and any
# extio consumer): it honors the wire contract's command surface and
# publishes the same state datapoints a real box would, with EXACT chosen
# timestamps (dservSetData lets an edge carry its true due-time even though
# delivery rides a 10 ms tick). Virtual jumpers mirror the loopback harness:
# out 8 -> in 12, out 7 -> in 13, out 11 -> in 3 + analog ch0.
#
# Spawned on demand, never from dsconf:
#   dservctl -c 'subprocess virtual_extio "source /Users/sheinb/src/dserv/config/virtual_extio.tcl"'
# Re-source a live instance to reprogram (init re-subscribes cleanly).
#
# Contract coverage:
#   cmd/do/<n> 0|1            level set (publishes state/do + jumpered DI edges)
#   cmd/do/<n>/pulse_us W     immediate pulse (rise now, fall now+W)
#   cmd/do/<n>/at D           pulse at beginobs+D us (width config/pin/<n>/pulse_us)
#   cmd/do/<n>/at_abs T       absolute-time HIGH; replies state/sched/abs_err
#                             armed|late (+abs_lead_us / abs_late_us)
#   config/pin/<n>/pulse_us, config/ain/{rate,enable},
#   config/ain/group/0/{channels,label,batch,mode,decimate}
#   analog: continuous blocks in the real box_ain_group wire format, with the
#   REAL 200 blocks/s publish throttle (drops + counts state/ain/dbg/throttled)
#
# FAULT INJECTION (the test of the test) via datapoints, live:
#   extio_vbox/fault/drop_edge_pct    silently skip N% of DI edge publishes
#   extio_vbox/fault/extra_latency_ms add to every DI echo's timestamp
#   extio_vbox/fault/ain_gap_pct      silently skip N% of analog blocks
#   extio_vbox/fault/atabs_refuse     reply "late" to every at_abs
# All default 0 -- a clean box. The analysis pipeline MUST catch each one.
#

package require dlsh
tcl::tm::add $dspath/lib
load $dspath/modules/dserv_timer[info sharedlibextension]
proc exit {args} { error "exit not available for this subprocess" }
errormon enable

namespace eval vbox {
    variable name       vbox
    variable logpath    /tmp/virtual_extio.log
    variable logfd      ""
    variable tick_ms    10
    variable prop_us    50      ;# synthetic out->in propagation (IRQ latency)

    # virtual jumpers: out pin -> in pin (loopback harness defaults)
    variable jumper
    array set jumper {8 12  7 13  11 3}
    variable ain_src_pin 11     ;# this out pin also feeds analog ch0
    variable ain_chan    0
    variable dac_ain_chan 1     ;# DAC0 (box pin 1 / D1) jumpered to ch1 (A1)
    variable dac_counts  0      ;# current DAC level (cmd/dac/0)

    variable dolevel;  array set dolevel {}
    variable pulsew;   array set pulsew {}     ;# config/pin/<n>/pulse_us
    variable pending   {}                      ;# {due_us kind pin level} DI/DO events
    variable levhist   {}                      ;# {t level} history of ain_src_pin
    variable obs_anchor 0
    variable obs_mode mirror        ;# +31: mirror | leader (config/obs/mode)
    variable obs_pin 10             ;# +31: the obs line (config/obs/pin)

    # analog stream state
    variable ain
    array set ain {enable 0 rate 0 label loop batch 12 chans 0 mode continuous decimate 1}
    variable ainlabel; array set ainlabel {}   ;# ain channel -> role label (v28)
    variable ain_next 0
    variable ain_buf {}
    variable ain_t0 0
    variable ain_sec 0
    variable ain_blocks_sec 0

    # counters
    variable ctr
    array set ctr {watchdog 0 cmds_rx 0 throttled 0 dropped 0 late 0 sweeps 0 blocks 0}
    variable nticks 0

    proc log { msg } {
        variable logfd
        if { $logfd ne "" } { catch { puts $logfd "[now] $msg" } }
    }

    proc fault { key } {
        if { [dservExists extio_vbox/fault/$key] } {
            return [dservGet extio_vbox/fault/$key]
        }
        return 0
    }

    proc state_name { leaf } {
        variable name
        return extio/$name/state/$leaf
    }

    # publish an INT datapoint with an explicit timestamp (matches the box's
    # DSERV_INT frames, so decoders stay honest)
    proc pub_int { leaf ts val } {
        dservSetData [state_name $leaf] $ts 5 [binary format i $val]
    }
    proc pub_str { leaf ts val } {
        dservSetData [state_name $leaf] $ts 1 [encoding convertto utf-8 $val]
    }

    # ---------------- edge machinery ----------------

    # queue the physical consequences of out pin <pin> going to <level> at <due>
    proc drive { pin level due } {
        variable jumper; variable pending; variable levhist
        variable ain_src_pin; variable prop_us; variable dolevel

        if { [info exists dolevel($pin)] && $dolevel($pin) == $level } { return }
        set dolevel($pin) $level

        lappend pending [list $due do $pin $level]
        if { [info exists jumper($pin)] } {
            set lat [expr {int([fault extra_latency_ms] * 1000)}]
            lappend pending [list [expr {$due + $prop_us + $lat}] di \
                                 $jumper($pin) $level]
        }
        if { $pin == $ain_src_pin } {
            lappend levhist [list $due $level]
        }
    }

    proc level_at { t } {
        variable levhist
        set lev 0
        foreach rec $levhist {
            lassign $rec ts l
            if { $ts > $t } { break }
            set lev $l
        }
        return $lev
    }

    proc drain { now } {
        variable pending
        if { ![llength $pending] } { return }
        set due_now {}
        set later {}
        foreach ev $pending {
            if { [lindex $ev 0] <= $now } { lappend due_now $ev } \
                else { lappend later $ev }
        }
        set pending $later
        foreach ev [lsort -integer -index 0 $due_now] {
            lassign $ev due kind pin level
            if { $kind eq "di" } {
                if { [fault drop_edge_pct] > 0 &&
                     rand() * 100 < [fault drop_edge_pct] } {
                    log "FAULT dropped di/$pin=$level @$due"
                    continue
                }
                pub_int di/$pin $due $level
            } else {
                pub_int do/$pin $due $level
            }
        }
    }

    # ---------------- analog machinery ----------------

    proc ain_start { now } {
        variable ain; variable ain_next; variable ain_buf; variable ain_t0
        set ain_next $now
        set ain_buf {}
        set ain_t0 0
        log "AIN start rate=$ain(rate) batch=$ain(batch) label=$ain(label)"
    }

    # what the configured channel would read at time t: the DAC-fed channel
    # reads the commanded DAC level (with the silicon's documented ~code-641
    # floor below code ~700); every other channel reads the digital drive
    proc sample_at { t } {
        variable ain; variable dac_ain_chan; variable dac_counts
        if { [lindex [split $ain(chans) ,] 0] == $dac_ain_chan } {
            return [expr {$dac_counts < 700 ? 641 : $dac_counts}]
        }
        return [expr {[level_at $t] ? 4095 : 0}]
    }

    proc ain_step { now } {
        variable ain; variable ain_next; variable ain_buf; variable ain_t0
        variable ctr
        if { !$ain(enable) || $ain(rate) <= 0 } { return }
        set interval [expr {int(1000000.0 / $ain(rate))}]
        while { $ain_next <= $now } {
            if { ![llength $ain_buf] } { set ain_t0 $ain_next }
            lappend ain_buf [sample_at $ain_next]
            incr ctr(sweeps)
            set ain_next [expr {$ain_next + $interval}]
            if { [llength $ain_buf] >= $ain(batch) } {
                ain_emit $interval
            }
        }
    }

    proc ain_emit { interval } {
        variable ain; variable ain_buf; variable ain_t0
        variable ain_sec; variable ain_blocks_sec; variable ctr
        set chan [lindex [split $ain(chans) ,] 0]

        set cnt [llength $ain_buf]
        set samples $ain_buf
        set t0 $ain_t0
        set ain_buf {}
        set ain_t0 0

        # the real box's publish throttle: >200 blocks/s are DISCARDED and
        # counted -- reproduce it so overload trials are detected by the
        # same mechanism as on hardware
        set sec [expr {$t0 / 1000000}]
        if { $sec != $ain_sec } {
            set ain_sec $sec
            set ain_blocks_sec 0
        }
        incr ain_blocks_sec
        if { $ain_blocks_sec > 200 } {
            incr ctr(throttled)
            return
        }

        if { [fault ain_gap_pct] > 0 && rand() * 100 < [fault ain_gap_pct] } {
            log "FAULT dropped ain block @$t0"
            return
        }

        # ALL the group's channels, scan-major in ascending channel order --
        # the column contract the real firmware emits and every decoder assumes.
        # This box has one physical signal, so column 0 carries it and the rest
        # are deterministic derivations of it: enough to exercise nchan > 1
        # decoding, column/label mapping and any consumer that needs an h AND a
        # v (em::process_analog returns outright on nchan < 2), without
        # pretending to be independent inputs.
        set chlist [lsort -integer [split $ain(chans) ,]]
        set nch [llength $chlist]
        set mask 0
        foreach c $chlist { set mask [expr {$mask | (1 << $c)}] }
        set wide {}
        foreach v $samples {
            for {set j 0} {$j < $nch} {incr j} {
                lappend wide [expr {$j == 0 ? $v : (4095 - $v)}]
            }
        }
        set payload [binary format cccc 1 $mask $nch $cnt]
        append payload [binary format i $interval]
        append payload [binary format ss 0 0]
        append payload [binary format s[llength $wide] $wide]
        dservSetData [state_name ain/$ain(label)] $t0 0 $payload
        incr ctr(blocks)
    }

    # ---------------- command / config handlers ----------------

    proc on_cmd { dp data } {
        variable name; variable ctr; variable pulsew; variable obs_anchor
        incr ctr(cmds_rx)
        set leaf [string range $dp [string length extio/$name/] end]
        set now [now]

        if { [regexp {^cmd/do/(\d+)$} $leaf -> pin] } {
            drive $pin [expr {$data != 0}] $now
        } elseif { [regexp {^cmd/do/(\d+)/pulse_us$} $leaf -> pin] } {
            drive $pin 1 $now
            drive $pin 0 [expr {$now + $data}]
        } elseif { [regexp {^cmd/do/(\d+)/at$} $leaf -> pin] } {
            if { $obs_anchor == 0 } { log "REFUSE at (no beginobs)"; return }
            set due [expr {$obs_anchor + $data}]
            set w [expr {[info exists pulsew($pin)] ? $pulsew($pin) : 1000}]
            drive $pin 1 $due
            drive $pin 0 [expr {$due + $w}]
        } elseif { [regexp {^cmd/do/(\d+)/at_abs$} $leaf -> pin] } {
            if { $data == 0 } {
                # +30 cancel verb
                pub_str sched/abs_err $now cancelled
            } elseif { [fault atabs_refuse] } {
                pub_str sched/abs_err $now late
                pub_int sched/abs_late_us $now 0
                log "FAULT at_abs refused"
            } elseif { $data <= $now } {
                pub_str sched/abs_err $now late
                pub_int sched/abs_late_us $now [expr {$now - $data}]
            } else {
                pub_str sched/abs_err $now armed
                pub_int sched/abs_lead_us $now [expr {$data - $now}]
                drive $pin 1 $data
                # +31 leader: an at_abs on the obs pin IS a scheduled onset.
                # Provisional epoch at arm (so lead-window at-commands
                # anchor), and the box-authoritative onset event -- future-
                # stamped at T with a synthetic ~40 us fire error, the
                # virtual license the +27 validation already used.
                variable obs_mode; variable obs_pin
                if { $obs_mode eq "leader" && $pin == $obs_pin } {
                    variable obs_anchor
                    set obs_anchor $data
                    pub_int in_obs $data 1
                    pub_int sched/obs_fire_err_us $now 40
                }
            }
        } elseif { [regexp {^cmd/dac/(\d+)$} $leaf -> dch] } {
            # v0.4.0+18 wire contract: immediate 12-bit DAC set, ch 0 only;
            # echo the APPLIED (clamped) code like the firmware does
            if { $dch != 0 } {
                pub_str dac/err $now "no such channel"
            } else {
                variable dac_counts
                set c $data
                if { $c < 0 }    { set c 0 }
                if { $c > 4095 } { set c 4095 }
                set dac_counts $c
                pub_int dac/0 $now $c
            }
        } elseif { $leaf eq "cmd/announce" } {
            announce
        }
        # cmd/save etc: accepted, no-op
    }

    proc on_config { dp data } {
        variable name; variable ctr; variable pulsew; variable ain
        incr ctr(cmds_rx)
        set leaf [string range $dp [string length extio/$name/] end]
        set now [now]

        if { [regexp {^config/pin/(\d+)/pulse_us$} $leaf -> pin] } {
            set pulsew($pin) $data
        } elseif { $leaf eq "config/obs/mode" } {
            variable obs_mode
            set obs_mode [expr {$data eq "leader" || $data == 1 ? "leader" : "mirror"}]
            # live role reflection, exactly like firmware CFG_OBS_MODE
            pub_str obs/mode $now $obs_mode
            pub_int obs_leader $now [expr {$obs_mode eq "leader" ? 1 : 0}]
        } elseif { $leaf eq "config/obs/pin" } {
            variable obs_pin
            set obs_pin $data
            pub_int obs_pin $now $data
        } elseif { $leaf eq "config/ain/rate" } {
            set ain(rate) $data
            ain_announce_group          ;# rate is part of what a group announces
        } elseif { $leaf eq "config/ain/enable" } {
            set was $ain(enable)
            set ain(enable) [expr {$data != 0}]
            if { $ain(enable) && !$was } { ain_start $now }
            if { !$ain(enable) && $was } { log "AIN stop" }
        } elseif { [regexp {^config/ain/label/(\d+)$} $leaf -> ch] } {
            # per-CHANNEL role label (fw persist v28). Channel-scoped, not
            # group-scoped, and echoed back on state/ so a consumer reads it
            # the same way it reads any other announced leaf. "" clears.
            variable ainlabel
            if { $data eq "" } { array unset ainlabel $ch } \
            else               { set ainlabel($ch) $data }
            pub_str ain/label/$ch $now $data
        } elseif { [regexp {^config/ain/group/0/(\w+)$} $leaf -> key] } {
            set oldname $ain(label)
            switch -exact $key {
                channels { set ain(chans) $data }
                label    { set ain(label) $data }
                batch    { set ain(batch) $data }
                mode     { set ain(mode) $data }
                decimate { set ain(decimate) $data }
            }
            # A RENAME FREES THE OLD NAME. dserv retains datapoints forever, so
            # the group's old announce leaves would otherwise linger and a host
            # would keep offering a stream nothing publishes. The firmware
            # disowns a name by announcing chans == "" once; mirror that, or
            # this box cannot exercise a consumer's tombstone handling.
            if { $key eq "label" && $oldname ne $ain(label) } {
                pub_str ain/group/$oldname/chans $now ""
            }
            ain_announce_group
        }
    }

    # Announce the analog group under its NAME, the way the firmware does: a
    # host reshapes a block from the group's announced channel list alone, so
    # without these leaves the group's data is undecodable and the group is
    # invisible to anything building a manifest.
    proc ain_announce_group {} {
        variable ain
        set now [now]
        set g $ain(label)
        if { $g eq "" } return
        pub_str ain/group/$g/chans    $now $ain(chans)
        pub_str ain/group/$g/mode     $now $ain(mode)
        pub_int ain/group/$g/batch    $now $ain(batch)
        pub_int ain/group/$g/decimate $now $ain(decimate)
        pub_int ain/rate              $now $ain(rate)
    }

    proc on_obs { dp data } {
        variable obs_anchor
        if { $data == 1 } {
            set obs_anchor [dservTimestamp $dp]
            pub_int in_obs $obs_anchor 1
        } else {
            pub_int in_obs [dservTimestamp $dp] 0
        }
    }

    # ---------------- housekeeping ----------------

    proc on_tick { args } {
        variable nticks; variable ctr; variable tick_ms; variable levhist
        set now [now]
        drain $now
        ain_step $now
        incr nticks
        if { $nticks % (1000 / $tick_ms) == 0 } {
            incr ctr(watchdog)
            pub_int watchdog $now $ctr(watchdog)
            pub_int cmds_rx $now $ctr(cmds_rx)
            foreach {leaf key} {ain/dbg/throttled throttled ain/dbg/dropped dropped
                ain/dbg/late late ain/dbg/blocks blocks ain/dbg/sweeps sweeps} {
                pub_int $leaf $now $ctr($key)
            }
            foreach leaf {dbg/pub_ev_drop dbg/pub_wire_drop} {
                pub_int $leaf $now 0
            }
            # prune stale level history (keep 5 s)
            set keep {}
            foreach rec $levhist {
                if { [lindex $rec 0] > $now - 5000000 } { lappend keep $rec }
            }
            set levhist $keep
        }
    }

    proc announce {} {
        variable jumper; variable ain_chan
        set now [now]
        set ins {}
        set outs {}
        foreach o [lsort -integer [array names jumper]] {
            lappend outs $o
            lappend ins $jumper($o)
        }
        pub_str pins/out $now [join $outs ,]
        pub_str pins/in $now [join [lsort -integer $ins] ,]
        pub_str transport $now virtual
        pub_str board $now virtual
        pub_str fw_ver $now 0.0.0+vbox
        pub_str sync/source $now sw
        pub_int dac_en $now 1
        # +31 obs-leader role: announced capability, exactly like firmware
        variable obs_mode; variable obs_pin
        pub_str obs/mode $now $obs_mode
        pub_int obs_leader $now [expr {$obs_mode eq "leader" ? 1 : 0}]
        pub_int obs_pin $now $obs_pin
        # re-announce the ain labels: like the real box these are announced
        # once per connect and never re-sent, so a page opened later needs the
        # burst to carry them or the analog rows fall back to channel numbers.
        variable ainlabel
        foreach ch [lsort -integer [array names ainlabel]] {
            pub_str ain/label/$ch $now $ainlabel($ch)
        }
        ain_announce_group
    }

    proc init {} {
        variable name; variable tick_ms; variable logfd; variable logpath
        variable nticks; variable pending; variable levhist
        set nticks 0
        set pending {}
        set levhist [list [list 0 0]]
        catch { close $logfd }; set logfd ""
        catch { set logfd [open $logpath w]; fconfigure $logfd -buffering line }
        log "INIT virtual extio box '$name' (tick ${tick_ms}ms)"

        dservRemoveAllMatches
        dservAddMatch extio/$name/cmd/*
        dservAddMatch extio/$name/config/*
        dservAddExactMatch ess/in_obs
        dpointSetScript extio/$name/cmd/* [namespace current]::on_cmd
        dpointSetScript extio/$name/config/* [namespace current]::on_config
        dpointSetScript ess/in_obs [namespace current]::on_obs

        timerPrefix vboxTimer
        dservAddExactMatch vboxTimer/0
        dpointSetScript vboxTimer/0 [namespace current]::on_tick
        timerTickInterval $tick_ms $tick_ms

        announce
        on_tick
        dservSet extio_vbox/ready 1
        puts "virtual_extio ready: box '$name', jumpers 8->12 7->13 11->3+ain0"
    }
}

vbox::init

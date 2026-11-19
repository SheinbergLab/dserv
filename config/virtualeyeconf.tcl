#
# virtual_eye process - simplified for degrees
#
# Publishes a held gaze position (eyetracking/virtual, "h v" as DSERV_FLOAT)
# on every timer tick; em::process_virtual turns each one into
# eyetracking/position and eyetracking/sample, stamped on arrival.
#
#   start ?interval_ms?     begin publishing (default 4 ms = 250 Hz)
#   stop
#   set_eye h v             jump there (cancels a saccade in flight)
#   saccade h v ?opts?      move there along a realistic trajectory
#   set_noise sd            fixational noise, deg per sample (0 = none)
#
# SACCADES.  `saccade` replaces the jump with what a tracker would record:
#   - main-sequence duration, dur = slope*amp + intercept ms (2.2 ms/deg +
#     21 ms by default), so a 10 deg saccade takes 43 ms;
#   - a minimum-jerk position profile, whose peak velocity is 1.875*amp/dur
#     (about 440 deg/s at 10 deg: in range for a human or monkey);
#   - an optional dynamic overshoot: the primary saccade lands overshoot*amp
#     past the goal and a short return movement (glissade_ms) brings it back,
#     as video trackers record at the end of most saccades.
# The position is computed from [now] on each tick, so the trajectory does
# not depend on the tick rate: a faster tick just samples it more finely.
# Options:
#   -delay ms        start this long from now (latency, timed exactly here
#                    rather than on a caller's coarser tick)
#   -dur ms          override the main-sequence duration
#   -overshoot frac  dynamic overshoot as a fraction of amplitude (0)
#   -glissade_ms ms  duration of the return movement (20)
#
# GROUND TRUTH.  Each saccade publishes eyetracking/virtual_saccade when it
# is scheduled:
#   t0 <us> t_primary <us> t_end <us> from {h v} to {h v} amp <deg> dur <ms>
#   overshoot <frac>
# t0 is the first instant of movement, t_primary the end of the primary
# saccade, t_end the end of the overshoot return (= t_primary without one),
# all on the dserv clock.  This is what the saccade detector's onset/offset
# events and the display's flip times are measured against.
#
package require dlsh
tcl::tm::add $dspath/lib

# disable exit
proc exit {args} { error "exit not available for this subprocess" }

# enable error logging
errormon enable

set virtual_eye(h) 0.0
set virtual_eye(v) 0.0
set virtual_eye(noise) 0.0

# main sequence: dur = slope * amp + intercept (ms)
set virtual_eye(ms_slope)     2.2
set virtual_eye(ms_intercept) 21.0

# the saccade in flight (active 0 = none)
array set sac { active 0 }

# load extra modules
set ess_modules "timer"
foreach f $ess_modules {
    load ${dspath}/modules/dserv_${f}[info sharedlibextension]
}

proc start { { interval 4 } } {
    timerTickInterval $interval $interval
    dservSet eyetracking/virtual_enabled 1
}

proc stop {} {
    timerStop
    dservSet eyetracking/virtual_enabled 0
}

# minimum-jerk position profile, 0..1 over x = 0..1
proc minjerk { x } {
    if { $x <= 0.0 } { return 0.0 }
    if { $x >= 1.0 } { return 1.0 }
    return [expr {$x*$x*$x*(10.0 - 15.0*$x + 6.0*$x*$x)}]
}

# the noiseless eye position at time t_us; ends a finished saccade
proc eye_at { t_us } {
    global virtual_eye sac
    if { !$sac(active) } { return [list $virtual_eye(h) $virtual_eye(v)] }
    set t [expr {($t_us - $sac(t0))/1000.0}]
    if { $t < 0 } {
        return [list $sac(from_h) $sac(from_v)]
    }
    if { $t < $sac(d1) } {
        set s [minjerk [expr {$t/$sac(d1)}]]
        return [list [expr {$sac(from_h) + $s*($sac(peak_h) - $sac(from_h))}] \
                    [expr {$sac(from_v) + $s*($sac(peak_v) - $sac(from_v))}]]
    }
    if { $t < $sac(d1) + $sac(d2) } {
        set s [minjerk [expr {($t - $sac(d1))/$sac(d2)}]]
        return [list [expr {$sac(peak_h) + $s*($sac(to_h) - $sac(peak_h))}] \
                    [expr {$sac(peak_v) + $s*($sac(to_v) - $sac(peak_v))}]]
    }
    # landed: hold the goal
    set sac(active) 0
    set virtual_eye(h) $sac(to_h)
    set virtual_eye(v) $sac(to_v)
    return [list $sac(to_h) $sac(to_v)]
}

# one normal deviate (Box-Muller)
proc gauss { sd } {
    if { $sd <= 0 } { return 0.0 }
    set u1 [expr {rand()}]
    if { $u1 < 1e-12 } { set u1 1e-12 }
    return [expr {$sd*sqrt(-2.0*log($u1))*cos(6.283185307179586*rand())}]
}

proc timer_callback { dpoint data } {
    global virtual_eye
    lassign [eye_at [now]] h v
    if { $virtual_eye(noise) > 0 } {
        set h [expr {$h + [gauss $virtual_eye(noise)]}]
        set v [expr {$v + [gauss $virtual_eye(noise)]}]
    }
    set eyevals [binary format ff $h $v]
    dservSetData eyetracking/virtual 0 2 $eyevals  ;# type 2 = DSERV_FLOAT
}

proc setup {} {
    timerPrefix virtualeyeTimer
    dservRemoveAllMatches
    dservAddExactMatch virtualeyeTimer/0
    dpointSetScript virtualeyeTimer/0 timer_callback
}

proc set_eye { h v } {
    global virtual_eye sac
    set sac(active) 0
    set virtual_eye(h) $h
    set virtual_eye(v) $v
}

proc set_noise { sd } {
    global virtual_eye
    set virtual_eye(noise) [expr {double($sd)}]
}

proc set_main_sequence { slope intercept } {
    global virtual_eye
    set virtual_eye(ms_slope) [expr {double($slope)}]
    set virtual_eye(ms_intercept) [expr {double($intercept)}]
}

proc saccade { h v args } {
    global virtual_eye sac
    set opts [dict merge {-delay 0 -dur {} -overshoot 0.0 -glissade_ms 20} $args]
    dict for {k val} $opts {
        if { $k ni {-delay -dur -overshoot -glissade_ms} } {
            error "saccade: unknown option $k (-delay -dur -overshoot -glissade_ms)"
        }
    }
    set t_now [now]
    # start from wherever the eye is now, mid-saccade included
    lassign [eye_at $t_now] fh fv
    set amp [expr {hypot($h - $fh, $v - $fv)}]
    set d1 [dict get $opts -dur]
    if { $d1 eq "" } {
        set d1 [expr {$virtual_eye(ms_slope)*$amp + $virtual_eye(ms_intercept)}]
    }
    set d1 [expr {max(1.0, double($d1))}]
    set os [expr {max(0.0, double([dict get $opts -overshoot]))}]
    set d2 [expr {$os > 0 ? max(1.0, double([dict get $opts -glissade_ms])) : 0.0}]

    array set sac [list \
        active 1 t0 [expr {$t_now + int(1000*[dict get $opts -delay])}] \
        from_h $fh from_v $fv to_h [expr {double($h)}] to_v [expr {double($v)}] \
        peak_h [expr {$fh + (1.0 + $os)*($h - $fh)}] \
        peak_v [expr {$fv + (1.0 + $os)*($v - $fv)}] \
        d1 $d1 d2 $d2]
    # the held position is the goal from here on (set_eye cancels)
    set virtual_eye(h) $sac(to_h)
    set virtual_eye(v) $sac(to_v)

    set t_primary [expr {$sac(t0) + int(round(1000*$d1))}]
    set t_end [expr {$t_primary + int(round(1000*$d2))}]
    set truth [list t0 $sac(t0) t_primary $t_primary t_end $t_end \
                   from [list $fh $fv] to [list $sac(to_h) $sac(to_v)] \
                   amp [format %.3f $amp] dur [format %.2f $d1] overshoot $os]
    dservSet eyetracking/virtual_saccade $truth
    return $truth
}

setup
puts "Virtual_eye initialized"

# `photo`: light edges from a photodiode

A photodiode over a screen patch (or over the stimulus itself), digitized by
an extio box, says when light actually changed on the panel. The `photo`
processor (`processors/photo.c`) watches that analog stream inside dserv and
publishes one small event per change of light, timed to the sample. The
data file gets a few edges per trial instead of the whole stream, and
pairing an edge with the frame that caused it becomes an in-order match.

The full contract (input, output, every parameter) is the header of
`processors/photo.c`. This page is how to bring it up on a rig.

## Where it attaches

The diode's analog group arrives as `extio/<box>/state/ain/<group>`.
`config/triggers.tcl` attaches the processor with a `*` for the box, so it
follows whichever box carries the group:

    processAttach photo extio/*/state/ain/photodiode photo

`photodiode` is the group name extio's `record` setting uses. A rig whose
group has another label attaches it again in a `local/trigger-*.tcl`, with
the same name so it replaces the default:

    processAttach photo extio/*/state/ain/<label> photo

The `*` is matched by `src/dpoint_process.c` (any run of characters,
`/` included). Names without a `*` are compared exactly, as before. A dserv
older than this change attaches the pattern literally and never matches.

dserv runs only the first processor attached to a datapoint, so the diode
needs its own analog group, not a channel in the eye's.

## Turning it on

From the ess interp (`config/essconf.tcl`):

    photoEnable 1
    photoSetParams min_step 150 smooth 0.002
    photoGetParam n_edges

It is off by default. Off costs an uncontended lock per block.

## What comes out

`proc/photo/edge`, `DSERV_DOUBLE`, ten values per edge:

| | | |
|---|---|---|
| 0 | `dir` | +1 brighter, -1 darker |
| 1 | `t50` | crossing of 50% of the change, dserv-clock us |
| 2 | `t10` | 10%: the nearest thing to "light started to change" |
| 3 | `t90` | 90% |
| 4 | `old` | level before (counts) |
| 5 | `new` | level after: settled, or the extreme reached |
| 6 | `settled` | 1, or 0 if it turned back or timed out first |
| 7 | `seq` | edges since the reset |
| 8 | `t_arm` | when it was detected |
| 9 | `t_done` | when it was reported |

The times come from the box's own sample times (block stamp + k x
interval), the clock the obs boundaries and eye samples are on. The
datapoint's timestamp is `t50`, so an edge is logged in the obs it belongs
to however late it arrived.

**Batched blocks** change none of that: they only delay when an edge is
published (after the block that finishes it). dserv takes one output per
input block, so a block that finishes two edges sends both in one
datapoint, a row each. Read the payload as rows of ten.

## Two kinds of display

**Continuous backlight (most LCDs).** The light approaches each new level
over a few ms. The edge is the change of level; its crossing times are
interpolated between samples. If the backlight is PWM-dimmed, the diode
sees a kHz ripple: set `smooth` to a few PWM periods (a window that covers
whole periods cancels it; one that does not leaves a residue that
`noise_k` absorbs into the threshold). Smoothing changes the shape of the
edge it measures, by a constant for a given panel: prefer the shortest
window that removes the ripple, and trust `t50` and `t10` over `t90`.

**Strobed backlight (Display++).** The backlight flashes once a refresh,
so even a steady patch is a pulse train, and without strobe mode every
flash is an edge. With

    photoSetParams strobe 1 period [expr {1.0/120}]

each flash (a run above dark + `pulse_thresh`) becomes one value per
refresh, its peak, stamped where it rose through half that peak; a
refresh with no flash is a 0 at the time it was due. The edge logic runs on
those values, and the times are the first REFRESH past 10/50/90%, with no
interpolation between flashes. `period` can be left at 0 to learn it from
the flashes (`period_est`), but a patch that starts black has no flashes to
learn from, so set it.

## Bringing it up

1. The stream: `dservGet extio/ain/streams` lists the group. Sample at
   5-10 kHz with the box's hardware-paced sampler: the interpolation is
   good to tens of us there, about a sample at 1 kHz.
2. The signal: keep the diode amplifier's output inside the ADC's range
   (3.3 V on the MCP3204 boxes). A clipped bright level still gives edges,
   but its `new` and `t90` mean nothing. A PDA36A2's bandwidth falls
   steeply at its high gain settings, which adds its own rise to what you
   measure: use the lowest gain that gives a usable step.
3. Turn it on and flash something under the diode. `photoGetParam
   n_blocks` rising says it is attached; `level` and `noise` say what it
   sees between edges. Set `min_step` well above `noise` and well below the
   smallest change you want.
4. Display++: `strobe 1`, then check `n_pulses` rising, `n_long` 0 (a flash
   that never ended: not strobed, or `pulse_thresh` below the dark between
   flashes), and `period_est` at the refresh.
5. Watch `proc/photo/edge` while toggling the patch: one row per change.

## Counters

`n_blocks n_samples n_edges n_rising n_falling n_unsettled n_gaps n_bad
n_stale n_multi n_dropped`, and in strobe mode `n_pulses n_missing n_long`;
`level noise base dt_est item_dt period_est last_t50` and the window
sizes `n_smooth n_base n_settle`. `reset` zeroes them and forgets the
signal; `defaults` restores the settings and leaves `active`.

## Tests

`tests/test_photo.c` (ctest `photo_processor`) drives it with analytic
signals: LCD steps, a 120 Hz toggle the panel never settles on, PWM
ripple, two channels, a replayed block and a gap, 24-sample blocks with
two edges each, a strobed flash train starting black and grey, and the `*`
attach through the real dispatch.

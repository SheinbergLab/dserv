# `photo`: light edges from a photodiode

A photodiode over a screen patch (or over the stimulus itself), digitized by
an extio box, says when light actually changed on the panel. The `photo`
processor (`processors/photo.c`) watches that analog stream inside dserv and
publishes one small event per change of light, timed to the sample. The
data file gets a few edges per trial instead of the whole stream, and
pairing an edge with the frame that caused it becomes an in-order match.

The full contract (input, output, every parameter) is the header of
`processors/photo.c`. This page is how to bring it up on a rig.

## Setting it up: the gear

The rig's settings are the `photo` section of dserv's settings gear
(declared in `config/essconf.tcl`, applied by the `photo` block of
`lib/ess-2.0.tm`). The processor keeps nothing across a dserv restart:
each key is pushed to it when it changes and once at boot.

| key | default | |
|---|---|---|
| `enabled` | 0 | detect, and log `proc/photo/edge` in every data file |
| `group` | `photodiode` | the analog group's label; `<box>/<label>` for one box; empty for every block |
| `strobe` | 0 | 1 for a strobed backlight (Display++) |
| `period_ms` | 0 | strobe: the refresh (8.3333 at 120 Hz); 0 learns it |
| `min_step` | 100 | smallest change that is an edge, ADC counts |
| `smooth_ms` | 0 | running mean before detection, for PWM ripple |

The patch (stim2's `photopatch`, stim2 `docs/photopatch.md`): a square
in a corner of the screen that changes between dark and light on chosen
frames, for the diode to sit over. Drawn only while both `enabled` and
`patch` are on, so a rig without a diode never flashes a corner at a
subject. stim2 turns it off at every script load, and ESS turns it on
again at each system load (`configure_stim`, after tlock's), so a changed
patch key takes effect at the next system load.

| key | default | |
|---|---|---|
| `patch` | 0 | draw the patch |
| `patch_mode` | `tags` | `tags`: frames carrying a matching swap tag (the ones ESS already times); `toggle`: every frame; `manual`: only `photopatch flip` |
| `patch_corner` | `tr` | `tr tl br bl`; top right is clear of tlock's row |
| `patch_size` | 60 | side, pixels |
| `patch_inset` | 0 | pixels in from both edges |
| `patch_dark` | 0 | the dark level, one grey or `r g b` (light is white); 0.1 on a strobed display keeps its flashes |
| `patch_match` | `*` | tags mode: swap tag patterns, e.g. `{SAMPLE *}` |

Each change of the patch is a `stim/photopatch` record on stim2's frame
log (swapcount, flip, level, change number, matching tags, why), logged
with `stim/frame`: a level 1 change should come back as a rising edge, a
level 0 one as a falling edge, in order.

`enabled` logs the edges, not the diode's raw signal. To keep that too (a
validation session, so detection can be re-run offline), name the group in
extio's `record` setting as well.

To try values without changing the rig's settings, use the session-only
helpers in the ess interp (`config/essconf.tcl`):

    photoSetParams min_step 150 smooth 0.002     ;# seconds here, not ms
    photoGetParam noise

## Where it attaches

`config/triggers.tcl` attaches the processor once at boot, by pattern, to
every analog group on every box:

    processAttach photo extio/*/state/ain/* photo

and its `group` parameter picks the one it reads; blocks from other groups
cost a name compare (`n_other`). So `group` is changed live from the gear,
never by attaching again: re-attaching frees the name the dispatch thread
may be comparing against. The `*` is matched by `src/dpoint_process.c`
(any run of characters, `/` included); names without a `*` are compared
exactly, as before. A dserv older than that attaches the pattern literally
and never matches.

dserv runs only the first processor attached to a datapoint, so while
`photo` holds every analog group nothing else can be attached to one
(nothing is today: the analog eye is read by a datapoint script). For the
same reason the diode needs its own analog group, not a channel in the
eye's.

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
flash is an edge. With `strobe` on (and `period_ms` at the refresh),
each flash (a run above dark + `pulse_thresh`) becomes one value per
refresh, its peak, stamped where it rose through half that peak; a
refresh with no flash is a 0 at the time it was due. The edge logic runs on
those values, and the times are the first REFRESH past 10/50/90%, with no
interpolation between flashes. `period_ms` can be left at 0 to learn it from
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
3. In the gear: `photo enabled` on, `group` the diode's group. Flash
   something under the diode. `photoGetParam n_blocks` rising says it is
   reading the group (`n_other` rising alone: the group name is wrong);
   `level` and `noise` say what it sees between edges. Set `min_step` well
   above `noise` and well below the smallest change you want.
4. Display++: `strobe` on and `period_ms` at the refresh, then check
   `n_pulses` rising, `n_long` 0 (a flash that never ended: not strobed,
   or `pulse_thresh` below the dark between flashes), and `period_est`
   agreeing with the setting.
5. Watch `proc/photo/edge` while toggling the patch: one row per change.
   With `patch` on and the diode over its corner, `patch_mode toggle`
   gives an edge every refresh (reload the system after changing it);
   `rmtSend "photopatch flip"` gives one on demand.

## Counters

`n_blocks n_other n_samples n_edges n_rising n_falling n_unsettled n_gaps n_bad
n_stale n_multi n_dropped`, and in strobe mode `n_pulses n_missing n_long`;
`level noise base dt_est item_dt period_est last_t50` and the window
sizes `n_smooth n_base n_settle`. `reset` zeroes them and forgets the
signal; `defaults` restores the settings and leaves `active`.

## Tests

`tests/test_photo.c` (ctest `photo_processor`) drives it with analytic
signals: LCD steps, a 120 Hz toggle the panel never settles on, PWM
ripple, two channels, a replayed block and a gap, 24-sample blocks with
two edges each, a strobed flash train starting black and grey, the `*`
attach through the real dispatch, and `group`. `tests/test_ess_photo.tcl`
(ctest `ess_photo`) checks the gear section: every key pushed, ms as
seconds, `enabled` last at boot, the data-file match, and the patch
(drawn only with both keys on, its settings sent before `on`, a warning
from a stim2 without the module).

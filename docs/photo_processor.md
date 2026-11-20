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
| `settle_ms` | 4 | how long the light must be still to finish an edge; ~10 for a slow LCD rise (below) |

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

## The green light

The gear's `photo` section shows a dot from `photo/health`. It reports
whether the detector is hearing the diode, which the knobs alone can't
say:

| | |
|---|---|
| off | `enabled` is off |
| error | this dserv has no `photo` processor |
| | no block from the group: with other analog groups streaming, the group name is wrong; with none, the diode's group isn't streaming |
| | the group's last block is more than 5 s old: the stream stopped |
| | `patch` is on and stim2 cannot draw it (no photopatch module) |
| | the patch changed and no edge came back, or the last edge is more than 2 s older than the last change: the diode is not over the patch, or `min_step` is above the patch's step |
| ok | blocks arriving; with the patch on, its changes answered (or none yet) |

The patch's first appearance doesn't count as a change, since over a
dark scene it gives no edge. Like tlock's light, it is worked out when a
key changes, at a system load, at a data file's open and at each end of
an obs. There is no subscription to the stream. So after a boot, a box
that connects late leaves the light red until the next of those.

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

## Pairing edges with frames

`tools/photopair/photopair.tcl` reads a session's `.ess` and pairs each
edge with the frame or event that caused it:

```bash
dlsh tools/photopair/photopair.tcl FILE.ess
```

It prints how many paired and the latency from each mark to the light
(t10, t50, t90; median, mean, sd, min, max, in ms), split by direction.
`-csv FILE` writes one row per mark.

**Edges** are the logged `proc/photo/edge`. When a file has none (a
session before `photo enabled`, with the diode's group recorded raw), or
with `-replay`, the same detector runs over the recorded blocks
(`photo_replay`, built with dserv as `build/processors/photo_replay`),
with `-p key=value` for its parameters. One recording can then be
detected again with other settings.

**Marks** (`-marks`):

| | |
|---|---|
| `photopatch` | stim2's patch changes (the default when the file has them); each says which direction to expect |
| `tlock` | the Display++ trigger's edges on the `tlock din` input: the box clock, no stim2 clock involved |
| `di:VARNAME` | rising edges of any digital input |
| `evt:T:S` | events, e.g. `evt:30:1` (SAMPLE ON) |
| `cmd:GLOB` | stim2 commands (`stim/cmd`) at the flip stim2 reported, e.g. `cmd:!sample_on` |

`-dir rise|fall|any` sets the expected direction for marks that don't
carry one.

stim2's flip times are on its host's wall clock. The `photo` block logs
`ess/dserv_wall_us` (dserv's clock minus the wall clock) in each data
file, and photopair adds it to put those flips on the edges' clock.
Older files have none. There the offset is estimated from when stim2's
records arrived: an upper bound, so latencies read short by the shortest
delivery time (tens of us between hosts).

The pairing:

1. Each mark is matched with its own next edge of the right direction.
   The median of those gaps is the typical latency, so one missed edge
   doesn't shift every later pair, even when the latency is longer than
   a refresh.
2. Each mark then takes the unused edge nearest its time plus that
   latency, within `-window` ms (default 4).

Edges no mark explains stay unpaired: the stimulus changing the light by
itself, or noise. A mark whose light never changed gets no edge.

### At 1 kHz

Both validation sessions sampled the diode at 1 kHz. Replayed and paired:

| session | marks | paired | t50 latency, ms | sd, ms |
|---|---|---|---|---|
| Display++ 2611181646 (strobe, period 8.249 ms, min_step 60) | tlock rising | 10 | 2.457 | 0.098 |
| | SAMPLE ON (evt:30:1) | 10 of 20 | 10.465 | 0.089 |
| LCD 2611191206 (officepi) | `!sample_off` | 11 of 11 | 5.599 | 0.054 |
| | `!sample_on` | 12 of 12 | 5.506 | 0.403 (two sample colours) |

1 kHz works:

- **Continuous LCD:** t50 is interpolated between samples, and the
  spread is tens of us.
- **Display++:** each flash lands on one or two samples, so where its
  half-rise falls is known to about 0.1 ms. t10 and t50 are the first
  changed refresh. t90 is a refresh or more later, and means little.
- **2 kHz** doubles the samples per flash and per rise.
- **5–10 kHz** is for tens-of-us work on a strobed display.

**PWM-dimmed backlights** are the exception. Their ripple runs at a few
kHz, and 1–2 kHz cannot resolve it: the ripple aliases into slow wander
that `smooth` cannot remove. Sample fast enough to see the ripple, or
avoid dimmed panels.

On a Display++, set `period` from the measured refresh, not the nominal
one: this rig ran at 8.249 ms, not 8.333. `period_est` tells you, or
leave `period` at 0. `min_step` must be below the smallest change in
flash height that counts. In 2611181646, half the SAMPLE ONs gave no
rising edge at either 100 or 60: those trials' sample changed the light
under the diode too little. A photopatch avoids that, since its step is
the same on every trial.

### A photopatch on an LCD: settle_ms

Session 2611191931 on .40 was transsacc displace/pipeline with the
virtual eye, the patch in `tags` mode, and the diode recorded through
the rig's `extio record`. All 120 patch flips paired. Every
`!target_on` / `!target_off` flip is the same swap as a patch flip, so
the `!` commands' flip stamps share these latencies. Replayed:

| settle_ms | rise t50, ms (sd) | fall t50, ms (sd) |
|---|---|---|
| 4 (default) | 5.14 (0.72) | 3.32 (0.08) |
| 10 | 8.08 (0.09) | 3.40 (0.05) |
| 20 | 8.20 (0.12) | 3.42 (0.05) |

This panel's black-to-white rise has a slow tail. At 4 ms the detector
called it settled at about a third of the step (~370-800 of ~1370
counts), so t50 came early and varied with where it stopped. At 10 ms
each rise is one step. Two more things this session showed:

- **Diode gain.** In the session before (2611191926), a high gain
  clipped white at 4095. The long rise was then sometimes reported as
  two rising edges: 20 extra edges, and the falls started from 4095.
  Keep the light level well under full scale.
- **Screen position.** The latency is the patch's corner, near the top
  row. An LCD scans top to bottom, so a stimulus lower on the screen is
  lit later by its fraction of the refresh.

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
seconds, `enabled` last at boot, the data-file match, the patch
(drawn only with both keys on, its settings sent before `on`, a warning
from a stim2 without the module), and every state of the light.

# `photo`: light edges from a photodiode

A photodiode over a screen patch (or over the stimulus itself), digitized by
an extio box, says when light actually changed on the panel. The `photo`
processor (`processors/photo.c`) watches that analog stream inside dserv and
publishes one small event per change of light, timed to the sample. The
data file gets a few edges per trial instead of the whole stream, and
pairing an edge with the frame that caused it becomes an in-order match.

The full contract (input, output, every parameter) is the header of
`processors/photo.c`. This page is how to bring it up on a rig.

The light can also come from a **comparator module** instead (an LM393
board: photodiode, threshold pot, digital out) on an extio DI line, named
by the gear's `photo din`. Then no analog processing is involved: each
edge on the line is the light changing, and pairing, verify, the light and
the placement test read the line. See "A comparator module" below.

## Setting it up: the gear

The rig's settings are the `photo` section of dserv's settings gear
(declared in `config/essconf.tcl`, applied by the `photo` block of
`lib/ess_photo-1.0.tm`). The processor keeps nothing across a dserv restart:
each key is pushed to it when it changes and once at boot.

| key | default | |
|---|---|---|
| `enabled` | 0 | detect, and log `proc/photo/edge` in every data file |
| `group` | `photodiode` | the analog group's label; `<box>/<label>` for one box; empty for every block |
| `din` | (empty) | the DI line's label for a comparator module: the light from that line, not the processor (below) |
| `strobe` | 0 | 1 for a strobed backlight (Display++) |
| `invert` | 0 | 1 when more light gives FEWER counts (an LM393 module's analog out) |
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
| `patch_mode` | `marked` | `marked`: every frame a system changes with a `!` command (`rmtSend "!sample_on"`) or a stim script tags (`swapTag`), so any system drives it unchanged; `toggle`: every frame; `manual`: only `photopatch flip` |
| `patch_corner` | `tr` | `tr tl br bl`; top right is clear of tlock's row |
| `patch_size` | 60 | side, pixels |
| `patch_inset` | 0 | pixels in from both edges; or `x y`, x from the side edge and y from the top or bottom (`20 60` at `tr`: 20 in from the right, 60 down; stim2 0.30.7 or later) |
| `patch_dark` | 0 | the dark level, one grey or `r g b` (light is white); 0.1 on a strobed display keeps its flashes |

Each change of the patch is a `stim/photopatch` record on stim2's frame
log (swapcount, flip, level, change number, swap tags, why, `!` replies), logged
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

With the patch on, an `ok` light also carries the current file's verdict
(next section), e.g. `ok reading group photodiode; pass: 412 of 412
changes lit; rise 8.112 ms (sd 0.041, max 8.230); fall 3.402 ms ...`.
The dot stays green on a `fail`: it says the diode is heard, and the
verdict says how well the display kept up.

## Checking a system: photo/verify

With `enabled` and `patch` on, ESS pairs every change of the patch with
its light as the session runs (the `photo_verify` block of
`lib/ess_photo-1.0.tm`). In `marked` mode that means every frame the system
changed with a `!` command, so any system can be checked as it is:
turn the two keys on, load it, run it.

Each change is joined to its edge the way photopair does it (below):
the median of the last 50 next-edge latencies of each direction is the
typical latency (until a direction has 3, its newest one: a file's
first change is judged by its own light, not by a median that a stray
latency of the other direction can pull 5 ms off, as one did on .40,
2026-11-24; the patch appearing over a scene adds none, its next edge
may be the scene's), and a change takes the unused edge nearest its flip
plus that, within ±4 ms. A change no edge matched there, with an unused
edge of its direction within 100 ms (and before the next change of that
direction could light), is `late`: the frame lit, at another latency.
Three late in a row in a direction start that direction's typical
latency again from those three, so a display that changes how it draws
mid-session (a Mac's compositor stepping up 20 ms, 2026-11-22) costs
three late changes, not every change after it. Each `!` command is
joined to its change exactly, by swap number. A change is judged 300 ms after its flip,
when nothing more can arrive for it (at the next record, end of obs or
file close); an edge no change took by then is extra.

Two records go into the data file:

**`photo/pair`**, one per change, stamped at its flip (JSON):

| | |
|---|---|
| `serial` `swap` `level` `why` `nacks` | from `stim/photopatch` |
| `seqs` | the `!` commands on its swap (`stim/cmd` seq) |
| `status` | `lit`; `late`, lit at another latency than the session's; `dark`, no light came; `appear`, the patch's first frame (never dark: a scene as bright as the patch shows no edge) |
| `edge` `lat10` `lat50` `light` | when lit or late: the edge's seq, us from the flip to its 10% and 50%, and its 50% point on the dserv clock |
| `expect` | when late: the typical lat50 it missed, us |

**`photo/verify`**, the running verdict, at each end of obs and when the
file closes (JSON): `verdict`, `changes lit late dark appear extra
pending`, `cmds cmds_lit cmds_late cmds_dark unmarked`, and `rise` and
`fall`, each `{n median sd min max}` of lat50 in ms, late ones included. Counts restart with each data file.
The closing verdict also goes to the ESS log as one line.

| verdict | |
|---|---|
| `pass` | every change lit, none late, no extra edge, every `!` command on a change |
| `fail` | a change lit late (the display's latency moved by more than 4 ms); a change gave no light (a frame stim2 acknowledged that the panel never showed, or two changes on adjacent frames that an LCD merged); an edge nothing asked for (a frame nobody commanded, or `settle_ms` too short for the panel's rise); or an `unmarked` `!` command, one whose swap changed no patch (marked mode only; also a `!` command that drew nothing, which stim2 logs) |
| `none` | no change judged yet |

Read it live with `essctrl -s ess -c 'dservGet photo/verify'`.

The latency spread is judged only through `late`: a light more than 4
ms off the typical latency. `rise`/`fall` report the spread itself, and
an unreliable display (latency that wanders by frames) shows there. The
latency includes any constant offset between stim2's host clock and
dserv's; on a rig without PTP between them, read only its spread.

### The ENDOBS hold

A change is judged 300 ms after its flip, and its edge arrives after the
frame. `readESS` puts a record in the obs it was written between, so a
trial's last edge and `photo/pair` would land in the next obs. With the
patch drawn, `end_obs` therefore holds the logged ENDOBS until the
trial's last `!` command or patch change can be judged (300 ms after it,
at most 400 ms), on a `dservAfter` timer. Everything else happens at
once: the obs line, `ess/in_obs`, the trial boundary, the next state.
Nothing in the state machine waits.

- ENDOBS keeps `end_obs`'s time, so the obs is as long as it was.
- Any event put during the hold writes ENDOBS first (BEGINOBS, a quit
  callback's `SYSTEM_STATE STOPPED`, a file's `TIME CLOSE`), so ENDOBS is
  still the obs's last event. Unloading the system writes it too. Either
  way the timer is cancelled.
- At the flush the pairing is brought up to date first, so the trial's
  `photo/pair` records are in it, then ENDOBS, then `photo/verify` and the
  green lights.
- Data logged during the hold is in the obs with times after ENDOBS: an
  obs-limited stream such as eye samples gains up to the hold's length
  past the end, and a command sent between trials (`nexttrial`) is in
  the trial before.
- Without the patch, or with nothing left to judge, `end_obs` writes
  ENDOBS at once, as it always has.

A hold cut short by an event can still leave the last light outside its
obs. Conversion joins `photo/pair` by swap, so it doesn't depend on
where the record is.

## Events at their light

The events a system puts right after a `!` command are linked to that
command's frame (`ess/evt_display`, ESS's evt_display block). Conversion
joins each to its `photo/pair` and adds `<event>_flip`, `<event>_light`
and `display_ok` to the trials (`docs/datafiles.md`, "Display times"). An
analysis that leans on the light uses `sample_on_light` where the system
logged `sample_on`. A trial whose frame never lit has NaN there and
`display_ok` 0, never the commanded time instead.

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
datapoint's timestamp is `t50`. That doesn't decide which obs it is in:
`readESS` puts each record in the obs it was written between, so an edge
finished after ENDOBS is in the next obs (or between obs). With the patch
drawn, ESS holds ENDOBS until the trial's light is in (see "The ENDOBS
hold"); otherwise read edges by their time, as photopair does.

**Batched blocks** change none of that: they only delay when an edge is
published (after the block that finishes it). dserv takes one output per
input block, so a block that finishes two edges sends both in one
datapoint, a row each. Read the payload as rows of ten.

With `din` set, the stream checks give way to the line's: `error
photo din: ...` when no box announces a line with that label (or it names
a chord group), and the patch's changes are checked against the line's
edges, e.g. `ok reading line photopatch; pass: ...`.

## A comparator module

An LM393 light board (about $1) has a clear photodiode, a comparator, a
pot that sets its threshold, and a digital out. On an extio DI line it is
a light detector with no analog processing at all: the box stamps each
edge in hardware, as it does any input. The box model `photo-lm393`
(`config/models/photo-lm393.tcl`) sets one up on an FRDM-MCXN947 and the
rig keys with it:

| | |
|---|---|
| D0 | DO, label `photopatch`, active-low (the board pulls low in light, so 1 = light), debounce 2 ms |
| A2 | AO, group `photodiode`: the analog behind the comparator, for checking it (`photo invert 1`: AO falls with light) |
| rig | `photo enabled 1`, `photo din photopatch`, `photo group photodiode`, `photo invert 1` |

Power it from 3.3 V, not 5 V: DO is pulled up to VCC.

**The threshold.** Run the placement test (`photo_test aim`) and turn the
pot until the board's DO LED follows the patch, then to the middle of the
range where it does. With a line there are no levels to report; the test
says `aligned` when every flip came back the right way, `reversed` when
the line reads 1 in the dark (set the pin active-low), `partial` when the
threshold sits near one of the patch's levels, and `none` when nothing
came back.

**Debounce.** The LM393 has no hysteresis, so a slow LCD transition
chatters through the threshold. 2 ms of debounce gives one clean edge per
change, and costs no accuracy: the box stamps the FIRST edge of a debounce
window (`extio-zephyr/src/platform/box_gpio.c`), so the debounce delays
the report, never the time.

**What its times mean.** The edge is where the light crossed the pot's
threshold, so its latency includes where on the panel's transition the
threshold sits: set at mid-range, it lands on the analog `t50`. Against
the analog signal on a 120 Hz LCD (2026-11-24, patch bottom right): the
edges matched `t50` to sd 0.05 ms while the room's light was steady, but
when both levels shifted by ~6% of the step the edges moved 0.5 ms (rise
and fall in opposite directions) while `t50`, measured against each
flip's own levels, did not move. Shield the detector, and keep the analog
group logged (`extio record`) in a validation session to see it.

A comparator cannot follow a strobed backlight: it would pulse with every
flash. Use an analog diode and `strobe` on a Display++.

### The box's own comparator

The FRDM-MCXN947 has comparators of its own, with a DAC for the threshold.
A pin in `cmp` mode is sampled by the ADC as usual and watched by a
comparator at the same time. The comparator's edges are reported as the
pin's DI events: stamped at the interrupt, through the same FIFO and
debounce as any input. So to dserv it is the same labelled line as a
module's DO, and everything above applies unchanged. The box model
`photo-cmp` (`config/models/photo-cmp.tcl`) sets it up. The LM393
module's AO alone (or any sensor) goes to A3, the one pin that reaches both
the ADC and a comparator (box pin 15, CMP1_IN2).

A **Teensy 4.0/4.1** (firmware 0.4.0+115) does the same over USB: model
`photo-cmp-teensy`. The sensor goes to A0 (pin 14: ain ch 7, CMP1_IN3), VCC to
3.3V. The line is still box pin 15, a pad with no Teensy pin, since that
board's analog pads are not box pins. Its DAC is 6-bit, so the minimum step
is 192 counts rather than 100, and `cmp dac` takes 0-63. `state/cmp` still
reports `dac` on the 8-bit scale, so the threshold formula below holds, and
it always shows ` vref 1 hyst 2`, because vref 1 is that board's default.
Bench, 2026-11-25: dark ~4085, light ~3250, locked at dac 227, mismatch 0.

What it changes:
- **No pot.** The box learns the dark and light levels from the samples
  and locks the threshold at their midpoint, then holds it there like a pot
  you never have to touch. It does not follow room light; a relearn moves it.
- **No low-light failure.** The LM393's inputs stop working within ~1.5 V of
  its supply, which a dim patch on its AO reaches. The MCU's comparator inputs
  work up to the rail.
- **Hysteresis.** 20 mV, set by the comparator's hysteresis control.

**Learn, then lock.** No edges are reported until the threshold locks. The box
looks for still periods (the signal within a band for 3 ms): one level, a
second at least 100 counts (~80 mV) away, then a return to the first. It then
writes the DAC once, at their midpoint, and arms the comparator. A still
period beyond the second level replaces it, so the learned step is the full
one. Any other new level (at least the minimum step from the second) means
the first is not coming back -- a pin read before the sensor was wired, a
hand moving it -- so the last two levels become the pair and a return to the
second locks (firmware +114; before that the box could stay `learning`
until a relearn). So after a boot or a config change, flip the patch a few times
(`photo_test aim`). An aim on a cmp line ends, done or stopped, by relearning
where the detector now is and flipping three more times so it locks.

**No analog on the wire.** The box samples the channel for the comparator
itself (firmware 0.4.0+112; `ain enable 1` is all it needs, no group), so a
cmp line sends only its edges, `state/cmp`, and `state/cmp/level`: each new
settled level of the light in counts, stamped when the light got there. That
is about one value per patch change and nothing while the light stays put,
against 250 frames/s for a 5 kHz group. `photo_test` measures dark, light and
step from it as it does for an analog group (`source` `cmp`), and data files
log it. To look at the analog itself (a transition's shape, an odd edge),
add a group on the channel live from the config page, and remove it after.
On +108/+109 firmware the channel must also be in a group.

Once locked, the threshold stays put for the session. The box keeps watching
the levels and reports how far their midpoint has moved from the threshold
as `drift`, in % of the step: near 0 is a centred edge; past about ±30 the
edge is off-centre enough to relearn. A **relearn** (console `cmp relearn`,
or publish anything to `extio/<box>/cmd/cmp/relearn`) forgets the levels and
learns again; it waits for the end of an obs, so it never moves the threshold
mid-trial.

Box console `cmp` shows the state, the levels, the DAC code, the drift, and a
count of disagreements between the comparator and the ADC. That count should
stay 0; if it doesn't, the DAC's reference is not the ADC's 3.3 V (`cmp vref
1` tries the other one). `cmp dac <0-255>` fixes the threshold, `cmp dac auto`
relearns, `cmp hyst <0-3>` sets the hysteresis; none of these is saved.

The edges are `extio/<box>/state/di/15`, like any input on that pin. The
comparator's state is `extio/<box>/state/cmp`, a dict string such as
`state armed lo 812 hi 2960 dac 117 drift 3 mismatch 0` (levels in ADC
counts; the threshold is (dac + 1) × 4096 / 256 counts; ` vref V hyst H` is
appended only when changed from 0 / 2). `state` is `off`, `unsupported`,
`no_samples` (learning, but the channel is not being sampled: ain is off), `learning` or
`armed`. It is published at once when the state, the DAC, the mismatch count
or the relearn count changes; a drift move of 5 points or more at most every
10 s; and again at each announce. Until it reads `armed`, the value of
`state/di/15` means nothing. With `photo din` on that line, `state/cmp` and
`state/cmp/level` go into every data file with the edges.

In the data file, the line's edges are logged with the box's clock records
and `photo/din` (where the label resolved). The trials get them as
`photo_din_t` / `photo_din` (ms from obs onset, 1 = light; left out when
`extio record` already adds the line under its own label), and the
pairing's `N_light` and `display_ok` as with an analog diode.

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

## Placing the diode

The gear's photo section has **◐ Test placement…**. It is
`::ess::photo_test` in the ess interp (the `photo_test` block of
`lib/ess_photo-1.0.tm`), which can also be run by hand:

```bash
dservctl -c 'send ess {::ess::photo_test}'
```

```bash
dservctl get photo/test
```

It flips the patch between dark and light (10 times, 250 ms apart) and
after each flip reads two things: the edges the detector reported, and its
idle `level`. The level follows the light whether or not the change was an
edge, so the step (light minus dark, in ADC counts) is measured even when
it is too small to detect. It runs only between sessions: it refuses
while ESS runs or a data file is open, and stops if either starts. With
`patch` off it draws the patch for the test and removes it afterwards.
The patch is in manual mode for the test and goes back to its own mode
afterwards.

| verdict | means | do |
|---|---|---|
| `aligned` | every change answered, step at least 2 × `min_step` | nothing |
| `small` | every change answered, step under 2 × `min_step` | centre the diode or raise the brightness: little margin |
| `partial` | some changes answered | centre, brighten, or lower `min_step` |
| `no_edges` | the step is above `min_step`, but no edge came | brighten, or lower `min_step` to about a third of the step (below) |
| `below_min_step` | the diode sees the patch, step under `min_step` | the same |
| `reversed` | the reading falls when the patch lights (or rises, with `invert` on) | flip `invert` in the gear |
| `none` | no change under the diode | it is not over the patch (the summary names the corner and size) |

`photo_test aim ?s?` keeps flipping (every 300 ms, at most `s` seconds,
60 from the gear) and reports the step from the last flips: slide the diode
until the step peaks. `photo_test stop` ends either one, and closing the
gear's window stops a test it started. `photo/test` (JSON) carries the
counts, `dark`, `light`, `step`, `best`, `noise`, `min_step`, rise and fall
latencies when stim2 has flip times, and the last 50 flips.

**A step only a little above `min_step` can give no edges.** The detector
fires when a sample is `min_step` away from its idle level, and that level
is a 20 ms running average (`level_tau`). A change that takes ~10 ms
drags the level along with it, so the gap may never reach `min_step`. On
the Pi 5 rig (2026-11-22) the step was 146 counts against `min_step` 100,
the rise took about 10 ms to get most of the way, and none of the flips gave
an edge. Hence the suggestion of a third of the step, kept above 5 ×
`noise`.

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
   a refresh. Rising and falling edges each get their own median, since
   a panel can rise and fall at different speeds. A direction with fewer
   than 3 first-pass edges uses the overall median instead.
2. Each mark then takes the unused edge nearest its time plus that
   edge direction's latency, within `-window` ms (default 4). This is
   what lets `cmd:` and `evt:` marks, which don't say their direction,
   pair both ways: on the .40 LCD (rise 8.1 ms, fall 3.4) one latency
   between the two left the `!target_on` falls outside the window, 21 of
   36 paired; now 36 of 36.

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
from a stim2 without the module), every state of the light, and the
comparator line (`din`: resolving, a chord group refused, its edges paired
in place of the processor's, its light and its data-file matches).
`tests/test_ess_photo_test.tcl` covers the placement test for both
sources, and `tests/test_df_recorded_streams.tcl` the `photo_din` columns.
`tests/test_ess_photo_verify.tcl` (ctest `ess_photo_verify`) feeds the
pairing records on a clock it sets: a clean session (every change lit,
both directions' latencies, each command's seq), a change judged only
after the hold, an edge that arrives late or before its change's record,
missed frames, a frame an LCD never showed between two others (the first
change is not given the third's light), a light 20 ms off its latency
(late), a latency that steps up 20 ms mid-session (three late each way,
then lit, none dark or extra), an extra edge, the patch
appearing, `!` commands with no change in marked and manual mode, two on
one swap, and a new data file's counts.
`tests/test_ess_obs_hold.tcl` (ctest `ess_obs_hold`) checks when ENDOBS
is held, that the timer writes the trial's `photo/pair` before it, that an
event meanwhile writes it first and cancels the timer, and that a stale
timer does nothing.
`tests/test_ess_photo_test.tcl` (ctest `ess_photo_test`) runs the
placement test against a pretend patch and diode: each verdict, the patch
drawn for the test and taken away, its mode put back, refusal while a data
file is open, stopping when ESS starts, `stop`, `aim`, and a patch that
disappears mid-test.
`tests/test_ess_evt_display.tcl` (ctest `ess_evt_display`) checks which
events are linked to which command, and `tests/test_df_display_times.tcl`
(ctest `df_display_times`) the columns conversion makes from them.

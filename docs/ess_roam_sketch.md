# `::ess::roam` — the sketch, and what got built

A response mode for **free locomotion**: the subject drives an agent around a
bounded arena, for as long as the trial lasts. Foraging, patch-leaving,
navigation, open-field search.

**Status: implemented.** `lib/ess_roam-1.0.tm`, unit test
`tests/test_ess_roam.tcl`, first user `systems/ess/joystick/forage`. The
argument below is the design that was built; where the implementation
diverged from this sketch, it says so inline and the module's own header
carries the reasoning. The three divergences worth knowing:

- **A `sectors` source was added, and is the default.** This sketch assumed
  an analog stick. The first user is a *d-pad* task, so the module reads
  `ess/joystick/dir` too: eight headings at one speed, free position. Both
  sources feed one integrator, so the arena, the edge rule, the region tests
  and the publishing exist once.
- **Circular arenas as well as rectangular**, because a boundary that
  encloses a ring of targets is a circle, and clamping a circle radially is
  what makes the agent *slide* along the wall instead of sticking.
- **The region test is in Tcl, inside the roam's own tick**, not the C
  `windows` processor — see the caveat added to that section below. The pose
  format is still the processor's, so the swap stays available.

## Why not the dial

`::ess::dial` is a **report** mechanism, and its shape says so: arm a response
window, move a cursor, take **one** commit, disarm. Every variable in it is
per-response state.

Roaming has neither a commit nor a response window. The agent moves for the
whole obs period, and what you measure is a trajectory — dwell time per patch,
visit sequence, path efficiency, time-to-first-patch — not an angle and an RT.
Forcing it into `dial_arm`/`dial_response` would mean fighting the wrapper at
every step and reinterpreting "the response" on every sample.

What *is* reusable is the input handling, and after the `stick_gain` /
`stick_velocity` extraction that is already shared framework code rather than
anything the dial owns.

## The layering, unchanged

    roam owns       integration, arena geometry, edge behavior, publishing
                    the agent's pose
    the protocol    what a patch IS, what being in one earns, when the trial
    owns            ends

Same split as the dial (which owns geometry and commit detection, while the
protocol owns what the angle means). `roam` must not learn what a patch is.

## The one decision to make first

**World-relative** (twin-stick / platformer): stick direction *is* world
direction. No heading state; `stick_velocity` already returns exactly this.

**Body-relative** (tank): stick x is turn rate, stick y is throttle. Heading
persists and becomes a measured variable — required for path integration or
egocentric navigation, and materially harder to learn.

Start world-relative. Add heading only when orientation is something being
studied rather than something the subject has to fight.

## The pose contract

Publish position as a **DSERV_FLOAT pair in degrees**, exactly like
`eyetracking/position` and `slider/position`:

    ess/roam/pos      "x y" as binary ff, degrees, origin at screen center

This is not a cosmetic choice — see the next section. If body-relative is
added later, heading goes in a *separate* datapoint rather than widening this
one, so the region processor keeps working unchanged.

## Patches come free from the C windows processor

dserv already ships C processors that test a point against N regions and
publish **only on state change**:

    processors/windows.dylib        used today for eye windows
    processors/touch_windows.dylib  used today for touch regions

and `processAttach` binds one to *any* datapoint (`config/triggers.tcl`):

    processLoad   .../windows[info sharedlibextension] roam_windows
    processAttach roam_windows ess/roam/pos roam_windows
    triggerAdd    proc/roam_windows/status 1 update_roam_region_status

That yields, with no new C:

- inside/outside tests for N patches at C speed, on every sample
- `proc/roam_windows/status` published only when a region is entered or left
- a trigger that wakes the state machine on exactly those events
- the whole `em_region_set` / `em_eye_in_region` API shape to copy

**This is the reason to match `eyetracking/position`'s format.** Inventing a
pose format would mean writing the region test in Tcl, on every sample, for
the entire trial.

> **What was actually built, and why.** The region test *is* in Tcl, inside
> `roam_step`. Two reasons, neither of which weakens the argument for the
> pose format:
>
> - Attaching a second `windows` instance costs a `config/triggers.tcl` edit
>   on **every rig**, and the `ain` param API selects a processor *globally*
>   (`ainSetProcessor`) while `em_region_on` assumes its selection survives.
>   A second instance is a live hazard to eye windows, not merely extra
>   wiring.
> - The first user has ≤8 regions on a 125 Hz tick — 1000 distance
>   comparisons a second, against a state machine that evaluates every
>   transition in the system on each wake. The tick, not the test, is the
>   cost.
>
> `roam_region_set` / `roam_region_on` / `roam_in_region` are deliberately
> `em_region_set`'s shape, and the pose is published in the format the
> processor wants, so when the region count or the rate makes it worth it the
> swap changes no protocol.

It also answers the recurring "would a C coprocessor help here" question. For
the dial: no — the arithmetic is smaller than the boundary crossing. For
roaming: yes, and it is already written.

## Why the SM discipline stops being an optimization

Every cursor source in `ess_dial` refuses to call `do_update` on motion; only
a commit wakes the state machine. For a dial that is a nicety, since a reach
lasts under a second.

For roaming the agent moves for the *entire* obs period. At 200 Hz a 30 s
trial is 6000 samples. Waking the SM on each one is 6000 evaluations of every
transition in the system. The region processor is what lets the SM sleep
through all of it and wake only on the handful of events that matter — patch
entered, patch left, trial over.

So: `roam_sample` publishes and integrates, and **never** calls `do_update`.

## The API, as built

    ::ess::roam_init   -sources sectors \      ;# sectors (d-pad) | rate (analog)
                       -rate 8.0 \             ;# deg/s at full push, either source
                       -accel 0.0 \            ;# sectors only: deg/s per second held
                       -expo 2.0 \             ;# rate only: the deflection curve
                       -scale 4.93 \           ;# rate only: rig slider/full_scale
                       -arena {circle 8.75} \  ;# or {rect x0 x1 y0 y1}, degrees
                       -edge clamp
    ::ess::roam_set_arena circle 8.75       ;# per-trial, usually from stimdg
    ::ess::roam_place  0 0                  ;# put the agent somewhere (clamped)
    ::ess::roam_start                       ;# begin moving; resets path/wall/latch
    ::ess::roam_stop                        ;# freeze, e.g. at trial end
    ::ess::roam_pos                         ;# {x y}
    ::ess::roam_path_length                 ;# integrated distance travelled
    ::ess::roam_wall_contacts               ;# 0->1 transitions, not per-tick
    ::ess::roam_tune -rate 12.0             ;# live, for an add_live_param
    ::ess::roam_simulate $vx $vy $dt        ;# drive it headlessly

    ::ess::roam_patch_set  $win $cx $cy $r  ;# circular region (roam_region_set
                                            ;# takes a type for rects)
    ::ess::roam_region_on / _off $win
    ::ess::roam_regions_clear
    ::ess::roam_in_region  $win
    ::ess::roam_entered                     ;# first region entered since start, or -1
    ::ess::roam_entered_time                ;# ... and when, in dserv-clock us

`roam_sample` (the `rate` source) is `dial_astick_sample` minus the ring, the
commit, the arc and the re-approach latch. `roam_dir` + `roam_tick` (the
`sectors` source) is `dial_dpad_dir` + `dial_dpad_tick` minus the spoke: a
new direction is adopted *immediately* rather than by retracting through the
center, which is the whole difference between reporting a bearing and moving
in a plane. Both end in `roam_step`, which owns the clamp, the wall count,
the path integral, the publishing and the region test.

`-edge`: **clamp** is the honest default (a wall is a wall). **wrap** makes an
infinite plane, which breaks path-length interpretation. **bounce** teaches
the subject a physics they did not ask to learn. Only clamp is implemented,
and the other two raise rather than being silently accepted.

## Trajectory logging

`slider/position` is already logged per-trial with a decimating logger
(`ess-2.0.tm`: `dservLoggerAddMatch $filename slider/position 1 80 1`).
`ess/roam/pos` wants the same treatment — the trajectory *is* the data here,
so this is not optional the way a cursor track is for a dial.

Built: `ess-2.0.tm`'s `file_open` records `ess/roam/pos 1 80 1` (obs-limited,
80-byte buffer) plus `ess/roam/geometry` and `ess/roam/regions` whenever
`roam_active`, mirroring the `slider_active` block above it. Geometry is
`dservTouch`ed so the arena the path must be read against lands in *this*
file. Path length and wall contacts are queryable live (`roam_path_length`,
`roam_wall_contacts`) and recomputable exactly offline — so a protocol does
not have to choose its summary statistics in advance.

## An `astick` version of `search`

`systems/ess/search` is a touch-screen search task: targets appear, the
subject touches one. It becomes a foraging task by replacing the finger with
a roamed agent — same stimulus generation, same scoring, different effector.

Concretely, the pieces that already exist:

- targets are drawn from `stimdg` as circles at known positions
- a patch is a `windows` region at each target's center
- "found it" becomes region-entered rather than touch-in-window
- dwell-to-collect is a timer started on entry, which the region status
  transition gives for free

What is genuinely new is the scoring: a touch task scores one discrete choice,
where a foraging task scores a *path*. Time-to-first-target, revisit rate and
search-path efficiency have no counterpart in the touch version, and they are
the reason to build it.

The reduced-rung ladder from `joystick/targets` does **not** transfer. Its
mechanism is withholding a spoke, and a freely roaming agent has no spokes.
Shaping here is arena size, target count and target radius instead.

## The first user: `joystick/forage`

Built before `search`, because `joystick/targets` was already sitting one
protocol away from it. `systems/ess/joystick/forage` shows the same ring of
targets `targets` does — same loader math, same class pools, same variants —
and changes only how the cursor moves: free in two dimensions, with the ring
of targets enclosed by a (by default invisible) boundary circle that stops
the cursor rather than accepting a commit.

It is the concrete answer to "what does a protocol have to do", and it is
short: set the arena and one patch per target in `nexttrial`, `roam_start` in
`targets_on`, and read `roam_entered` in `responded`. Everything above it —
trial structure, timing, the response window, reward — is the unchanged
`joystick` system.

Two things the paragraph above predicted and the build confirmed: the ladder
rungs still exist but no longer *teach* the same way (fewer targets makes the
goal easier to hit, it does not make the wrong move free), and the response
timestamp has to become the arrival rather than the deflection onset — which
is why `joystick.tcl` grew a `response_onset` method instead of hard-coding
the joystick latch.

# `::ess::roam` — a sketch

A response mode for **free locomotion**: the subject drives an agent around a
bounded arena with an analog stick, for as long as the trial lasts. Foraging,
patch-leaving, navigation, open-field search.

Status: design only. Nothing below is implemented.

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

## Sketch

    ::ess::roam_init   -sources astick \
                       -scale 4.93          ;# rig, from slider/full_scale
                       -rate 12.0           ;# deg/s at full push
                       -expo 2.0
                       -arena {-24 24 -13 13}   ;# xmin xmax ymin ymax, deg
                       -edge clamp          ;# clamp | wrap | bounce
    ::ess::roam_start                       ;# begin moving (obs onset)
    ::ess::roam_stop                        ;# freeze, e.g. at trial end
    ::ess::roam_place  0 0                  ;# put the agent somewhere
    ::ess::roam_pos                         ;# {x y}
    ::ess::roam_path_length                 ;# integrated distance travelled

`roam_sample` is `dial_astick_sample` minus the ring, the commit, the arc and
the re-approach latch, plus arena clamping — call it 40 lines, most of it the
edge rule.

`-edge`: **clamp** is the honest default (a wall is a wall). **wrap** makes an
infinite plane, which breaks path-length interpretation. **bounce** teaches
the subject a physics they did not ask to learn. Start with clamp.

## Trajectory logging

`slider/position` is already logged per-trial with a decimating logger
(`ess-2.0.tm`: `dservLoggerAddMatch $filename slider/position 1 80 1`).
`ess/roam/pos` wants the same treatment — the trajectory *is* the data here,
so this is not optional the way a cursor track is for a dial.

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

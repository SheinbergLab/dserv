# Input Layer — Design

Reorganization proposal for dserv's handling of kernel input devices
(touchscreens, trackpads, and, eventually, additional contact / HID devices).
Scope here is **reframing and reorganization without breaking existing
consumers**. New initialization paths are acceptable; existing datapoint
contracts, processors, and state-system APIs are preserved byte-for-byte.

## Goal

Replace the touchscreen-specific `dserv_touch` module + ad-hoc device
discovery in `essconf.tcl` with a single **`input`** module/subprocess that:

- owns device discovery and event reading for all evdev-class input devices,
- classifies devices by capability rather than by USB path,
- publishes per-device-class datapoints so consumers subscribe to what they
  care about,
- is portable (name not tied to `libevdev`; macOS backend can be stubbed
  or added later via IOKit without changing consumers),
- lets us add trackpad as the first new device class, and future classes
  (foot pedal, button box, gamepad) without further architectural moves.

## Non-goals (explicitly)

- **No libinput.** Raw events only. Gesture recognition, hotplug, tap-to-click
  are deferred. If a future device class needs them, libinput slots in
  behind the module without changing consumers.
- **No changes to state-system APIs.** `::ess::touch_win_set`,
  `::ess::slider_init`, slider calibration semantics remain untouched.
- **No changes to `touch_windows.c`** or any existing `mtouch/event` consumer.
- **No changes to `ess/slider_pos` indirection** used by the remote stim2 host.
- **No trackpad delta mode in v1.** Absolute surface coordinates only.
- **No multi-touch beyond primary slot.** Additional contact slots are
  ignored for now.
- **No native macOS backend in v1.** On Darwin, `autodiscover` returns an
  empty device list; rigs rely on `virtual_slider` as today.

## Architecture

```
modules/input/input.c               # C module: libevdev on Linux, stub on macOS
                                    # device enumeration + per-device reader threads
config/inputconf.tcl                # new subprocess: loads module, runs autodiscover
local/input.tcl                     # per-rig overrides (optional)
```

The `input` subprocess is spawned from `config/dsconf.tcl` alongside `em`,
`ain`, `slider`, `juicer`, `sound`, etc. It owns:

- enumeration of `/dev/input/event*`,
- classification by libevdev capability bits,
- per-device reader threads,
- publishing raw events to per-device-class datapoint namespaces.

It does **not** own calibration, coordinate remapping beyond the existing
touchscreen rotation/scale logic, gesture recognition, or any
experiment-level semantics.

## Datapoint contracts

### Touchscreen — unchanged

- **`mtouch/event`** — `uint16_t[3] = (x, y, event_type)`
  where `event_type ∈ {0 = PRESS, 1 = DRAG, 2 = RELEASE}`.
  Coordinates are in display-pixel space, after the existing rotation /
  raw-to-pixel mapping from `modules/touch/touch.c`.

This is bit-for-bit identical to today. `touch_windows.c` and every state
system that calls `::ess::touch_win_set` continue to work without change.

### Trackpad — new

- **`mtouch/trackpad`** — `uint16_t[3] = (x, y, event_type)`
  Same shape and event semantics as `mtouch/event`. Coordinates are raw
  absolute `ABS_X` / `ABS_Y` from the active contact slot. No rotation, no
  remapping — consumers normalize using the companion range datapoint.

- **`mtouch/trackpad/range`** — `int32_t[4] = (min_x, max_x, min_y, max_y)`
  Published once at device discovery from libevdev's axis info. Consumers
  (the slider subprocess) use this to map trackpad surface → stimulus space.

### Slider — reframed

The slider subprocess already publishes `slider/position`. It gains:

- **`slider/active`** — `uint8_t` 0/1. **Universal engagement signal**,
  not trackpad-specific. See "Slider layer" below.

`slider/position` remains a continuous stream (consumers subscribe as today).
The `ess/slider_pos` republish used by the remote stim2 host is preserved
unchanged.

## Device classification and auto-configuration

Discovery is a two-layer mechanism.

**Layer 1 — capability-bit classification** (replaces path-glob matching
in `essconf.tcl:300-316`):

| Caps | Class | Output |
|------|-------|--------|
| `INPUT_PROP_DIRECT` + `ABS_X`/`ABS_Y` + `BTN_TOUCH` | touchscreen | `mtouch/event` |
| `INPUT_PROP_POINTER` + `ABS_MT_POSITION_X`/`Y`, not `INPUT_PROP_DIRECT` | trackpad | `mtouch/trackpad` + `mtouch/trackpad/range` |
| (future) `BTN_TRIGGER` / `BTN_JOYSTICK` | gamepad | TBD |
| (future) single-button HID | foot pedal / button box | TBD |
| other | ignored | — |

**Layer 2 — known-device table** (restores the "known hardware just works"
property the old `connect_touchscreen` had). Each entry is `{class,
pattern, defaults...}`. At autodiscover, after a device is classified,
its `/dev/input/by-id/<name>` basename and libevdev-reported device name
are matched (Tcl glob) against the table; the last matching entry's
defaults are applied.

Seeded in `config/inputconf.tcl` from the hardware list the old
`connect_touchscreen` carried:

```tcl
inputKnownDevice touchscreen *wch.cn_USB2IIC_CTP_CONTROL* \
    -screen_w 1024 -screen_h 600
inputKnownDevice touchscreen *ILITEK_ILITEK-TP* \
    -screen_w 1280 -screen_h 800
inputKnownDevice touchscreen *eGalax* \
    -screen_w 1280 -screen_h 800
```

`local/input.tcl` is sourced *after* the defaults, so re-declaring a
pattern with different values (last-match-wins) overrides cleanly. Rigs
with new hardware add their own `inputKnownDevice` entry.

**Touchscreens without a known-device or class-level dimensions are
skipped at autodiscover with a warning**; if the class was required via
`inputExpect`, `inputValidateExpectations` then fails startup loudly.
Trackpads auto-enable purely from capability-bit classification — no
dimensions needed (the device's own `ABS_MT_POSITION_*` range is
published as `mtouch/trackpad/range`).

## Rig init

### `config/inputconf.tcl` (new subprocess entry point)

```tcl
load ${dspath}/modules/dserv_input[info sharedlibextension]

# Register device classes and their output datapoints
input::register_class touchscreen -namespace mtouch/event
input::register_class trackpad    -namespace mtouch/trackpad

# Auto-discover: scan /dev/input/event*, classify by caps, spawn readers
input::autodiscover

# Per-rig overrides (rotation, exclusions, forced classification)
if { [file exists [file join $local_config input.tcl]] } {
    source [file join $local_config input.tcl]
}
```

### `local/input.tcl` — dual-input rig example

```tcl
# 10" ELO touchscreen mounted rotated 90°
input::configure touchscreen -rotation 90 -screen_w 1280 -screen_h 800

# USB trackpad; no knobs needed for absolute mode
input::configure trackpad
```

### `config/dsconf.tcl` — spawn the subprocess

```tcl
subprocess input "source [file join $dspath config/inputconf.tcl]"
```

### `config/essconf.tcl` — removals

- Drop `load dserv_touch.so` (touch reading moves to input subprocess).
- Drop `connect_touchscreen` (replaced by `input::autodiscover`).
- Ess continues to listen to `mtouch/event` via whatever mechanism it uses
  today for protocol-level wiring (no change).

## Slider layer

The slider subprocess is where *semantics* live. Input transport stays
honest about what devices report; experimentally-meaningful behavior is
synthesized here.

### `continuity_mode` — declared per-system at `::ess::slider_init`

Applies only to contact-based sources (trackpad, virtual). Pot sources
ignore the setting.

- **`absolute`** — new PRESS sets cursor to the finger's absolute position
  mapped into stimulus space. Models "where my hand is, is where the
  stimulus is." Appropriate for hand-in-space / absolute-pointing paradigms.
- **`continuous`** — new PRESS holds cursor at last position; subsequent
  DRAG moves cursor by delta from the PRESS point. No teleports across
  contacts. Appropriate for steering / tracking paradigms.

**No default.** Systems must declare, e.g. `::ess::slider_init -mode continuous`.
Forgetting to declare is an error at protocol_init, not a silent guess.

### `release_behavior` — per-source

What `slider/position` does after RELEASE (contact-based sources only):

- **`hold`** — keep publishing last value. Default for both modes;
  matches "hand left, last known position is meaningful."
- **`stop`** — cease updating; consumer keys off `slider/active`.
- **`recenter`** — return to a defined neutral.

### `slider/active` — universal engagement signal

Source-dependent population:

| Source | `slider/active` |
|--------|-----------------|
| Plain pot on rail (`ain/vals`) | always 1 — mechanically enforced |
| Touch-sensing pot (future hardware) | capacitive touch bit from wiper |
| Trackpad | 1 between PRESS and RELEASE, 0 otherwise |
| Virtual (`press_drag_release`) | 1 during simulated contact |
| Virtual (`always_active`) | always 1 |

Adding a touch-sensing Bourns-style pot in the future **requires no
architectural change** — the signal already has a home.

### Source selection

Slider subprocess retains its existing source-gating logic. Adding trackpad
as a source means:

```tcl
# in sliderconf.tcl
dservAddExactMatch mtouch/trackpad
dpointSetScript    mtouch/trackpad slider::process_trackpad

dservAddExactMatch mtouch/trackpad/range
dpointSetScript    mtouch/trackpad/range slider::set_trackpad_range
```

`process_trackpad` applies `continuity_mode` + `release_behavior`, maps
surface coords to stimulus space using `mtouch/trackpad/range`, publishes
to `slider/position` and `slider/active`.

## Systems / experiments

### `steer` — continuous paradigm

In `steer.tcl`:

```tcl
::ess::slider_init -mode continuous -release hold
```

The stim-side velocity-clamp in `steer_stim.tcl:167-180` stays — it's
experiment-level robustness independent of trackpad vs pot.

Disengagement policy: when `slider/active` transitions 1 → 0 mid-trial, the
system decides (abort, pause-and-recover, grey the ball, etc.). This is
state-graph logic, not a transport question.

### Hand-in-space absolute-pointing systems

```tcl
::ess::slider_init -mode absolute -release hold
```

### `virtual_slider` alignment

Add a mode knob, default `press_drag_release`:

- **`press_drag_release`** (new default) — simulates contact lifecycle;
  publishes `slider/active` matching the contact state. Makes virtual and
  trackpad behaviorally equivalent for cross-platform development.
- **`always_active`** (legacy) — continuous stream, `slider/active` always 1.
  Opt-in for existing experiments that rely on it.

## What is explicitly preserved

- `mtouch/event` payload and semantics.
- `touch_windows.c`.
- `::ess::touch_win_set` and every state-system touch API.
- `ess/slider_pos` republish for remote stim2.
- Existing `slider/position` stream (just gains a companion `slider/active`).
- The velocity-clamp in `steer_stim.tcl`.
- Pot slider (`ain/vals`) behavior.

## Migration phases

Each phase is a self-contained PR; later phases depend on earlier ones.

### Phase 1 — Introduce `input` module (no behavior change)

1. Create `modules/input/input.c` by lifting `modules/touch/touch.c`
   verbatim into a **touchscreen device class** inside a
   multi-class skeleton. Keep platform guards (`__linux__` vs stub).
2. Create `config/inputconf.tcl` with `input::register_class` +
   `autodiscover`, initially supporting only the touchscreen class.
3. Add `subprocess input ...` to `config/dsconf.tcl`.
4. Remove `load dserv_touch.so` and `connect_touchscreen` from
   `config/essconf.tcl`.
5. Validate on a rig with a touchscreen: `mtouch/event` payload must be
   byte-identical; `touch_windows` consumer must behave identically;
   `planko/bounce` and any touchscreen-using system must run unchanged.
6. Delete `modules/touch/touch.c` once validated.

### Phase 2 — Add trackpad device class

1. Extend `input.c` with trackpad classification + reader.
2. Publish `mtouch/trackpad` + `mtouch/trackpad/range`.
3. No slider-side changes in this phase — verify via `dservGet` /
   inspection only.

### Phase 3 — Slider subprocess reframe

1. Add `slider/active` publication; pot path emits `always 1`.
2. Add `continuity_mode` + `release_behavior` settings to `slider::`.
3. Change `::ess::slider_init` to require `-mode`.
4. Add `process_trackpad` callback subscribing to `mtouch/trackpad`.
5. Validate `ain/vals` path unchanged (steer on a pot rig).

### Phase 4 — Virtual slider alignment

1. Add `press_drag_release` mode to `virtualsliderconf.tcl`.
2. Flip default to `press_drag_release`.
3. Existing experiments that break: set `always_active` explicitly
   in their settings, or migrate their state graph.

### Phase 5 — System declarations

1. Update `planko/steer/steer.tcl` to declare
   `::ess::slider_init -mode continuous -release hold`.
2. Update any hand-in-space system to declare
   `-mode absolute`.
3. Decide disengagement policy per system (state-graph work).

### Not in this plan (future)

- Additional device classes (gamepad, foot pedal, button box).
- Touch-sensing pot hardware integration (software contract already supports).
- Native macOS input backend (IOKit).
- Multi-slot multi-touch.
- Experiment-level `input_semantics` validation against rig capabilities.
- Extracting GPIO init out of essconf.tcl (tracked separately).
- Sampler subprocess / registry-to-configs moves (tracked separately).

## Resolved decisions

1. **Autodiscover is one-shot** at subprocess startup. `systemctl restart dserv`
   is the supported way to re-enumerate after plugging/unplugging hardware.
2. **Pot source populates `slider/active = 1`** whenever the pot path is the
   active source, and publishes nothing when it isn't. No disconnect /
   out-of-range detection in v1.
3. **Missing devices fail startup loudly, by default.** Rigs declare
   expectations in `local/input.tcl`; each declaration is required by
   default and may be marked optional:
   ```tcl
   input::expect touchscreen             ;# required (default)
   input::expect trackpad  -optional     ;# OK if missing
   ```
   A rig with no declarations gets permissive behavior (dev / laptop use).
4. **Device-class C-API shape** is drafted during Phase 1 from how
   `touch.c` currently structures its reader thread and publish calls;
   kept minimal and opened for critique before the module is finalized.

## Still open for Phase 1 drafting

- Exact signatures of the class-registration C-API (`dserv_input` internal).
- Whether `input::configure` knobs (rotation, screen_w, screen_h) should
  live as Tcl commands that update module state, or as arguments threaded
  through `autodiscover`. Leaning toward Tcl commands for symmetry with
  `ain::`/`slider::` patterns.

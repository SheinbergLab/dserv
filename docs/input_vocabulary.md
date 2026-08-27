# Input vocabulary — device, contract, strategy

One word meant three things across this codebase, and the confusion was not
"joystick is overloaded". It was that **three different axes were being named
out of one vocabulary**, with nothing saying which axis a word was on.

| axis | the question it answers | the words |
|---|---|---|
| **device** | what is physically wired | `dpad` (four switches), `stick` (2-axis analog), `trackpad`, `mouse`, `touchscreen` |
| **contract** | what an experiment consumes | `joystick` (eight sectors), `slider` (continuous position), `dial` (a bearing), `button` (a channel) |
| **strategy** | *how* a device answers a contract | `ring`, `sectors`, `rate` (three readings of one stick) |

**The rule: say which axis you are on, and never reuse a word across two.**

That is what went wrong before. `dial sources` offered `stick`, `dpad` and
`astick`, which look like devices and are not: all three read the SAME analog
stick, differing only in how they read it. `dpad` there never meant a d-pad,
and the validator had to explain that in prose — a name needing a disclaimer
is the wrong name. Meanwhile extio calls a four-switch group `joystick`,
which is a device, while ESS's `joystick` is a contract that a stick can
drive just as well.

## What each layer says now

**Device.** Exactly two words for the two things a rig wires for direction:
`dpad` = four switches, `stick` = a 2-axis analog stick. extio already
labels its analog groups `stick`; its digital groups are still labelled
`joystick` on fielded boxes, and label matching is token-based so they keep
resolving. New boxes should label them `dpad`.

**Contract.** `joystick` is the eight-sector direction contract — NOT a
device, and deliberately not renamed. It is what experimenters say, it is in
`ess/joystick/*`, in ~200 procs, and in every recorded `.ess` file. Renaming
it would break past data to improve present prose. A `stick` and a `dpad`
both answer it; that transport-independence is the whole point.

**Strategy.** A strategy word says how a device answers a contract, so the
same word means the same reading wherever it appears:

| strategy | what it does |
|---|---|
| `ring` | deflection rotates a bearing; cursor pinned to the ring (1-D) |
| `sectors` | deflection quantized to eight; a cursor moves along that heading |
| `rate` | deflection IS the cursor's velocity; continuous, speed-graded |

`dial` (`ess_dial-1.0.tm`) offers all three; `roam` (`ess_roam-1.0.tm`)
offers `sectors` and `rate`. What a strategy *produces* differs by response
mode — under the dial, `sectors` walks the cursor out one spoke and reports
one of eight bearings; under a roam it sets the heading of a free 2-D
position — but in both it is "the deflection is read as eight directions",
which is what the word claims.

`roam_init` takes **only** the strategy words. It is new, so it has no old
spellings to honour; `-sources dpad` raises an error naming `sectors` rather
than reporting "unknown source" to someone carrying a habit over from the
dial.

## Old spellings, and where they are accepted

Every old word still works, at every door, and is normalized on the way in.
Nothing on a rig or in a protocol has to change.

| old | new | where |
|---|---|---|
| `analog` | `stick` | `setting joystick transport`, `joystick_bind` |
| `box_group`, `box` | `dpad` | `setting joystick transport`, `joystick_bind` |
| `stick` | `ring` | `setting dial sources`, `dial_init -sources` |
| `dpad` | `sectors` | `setting dial sources`, `dial_init -sources` |
| `astick` | `rate` | `setting dial sources`, `dial_init -sources` |

Normalizers: `::ess::joystick_transport_norm` (ess_transports-1.0.tm) and
`::ess::dial_source_norm` / `dial_sources_norm` (ess_dial-1.0.tm). The
settings layer runs `-validate` before its `-values` check, so a rig file
saying `setting joystick transport analog` validates as `stick` and reads
back as `stick` in the settings gear. `dial_init` normalizes too, so a
protocol's `-sources {swipe dpad}` keeps meaning what it meant.

Pinned by `tests/test_ess_transports.tcl` and `tests/test_ess_dial_dpad.tcl`
— both mappings are asserted, so a later edit cannot quietly drop them.
`tests/test_ess_roam.tcl` asserts the opposite for `roam_init`: that the
device words are *refused*, with the strategy word named in the message.

## What was deliberately NOT renamed

- **`ess/joystick/*` datapoints and the `joystick_*` procs.** See above:
  recorded data and the domain word.
- **Internal dial state variables** — `dial_dpad_angle`, `dial_astick_rate`,
  `dial_stick_angle` — 400+ references, and some are tunables a rig may
  already set. `dial_dpad_*` serves `sectors`, `dial_astick_*` serves
  `rate`, `dial_stick_*` serves `ring`. The module header says so.
- **`joystick box_group` (the settings KEY).** It names the chord group's
  label on the box, which is accurate whatever the transport is called.
- **The resolver registry keys** (`analog`, `box`). `joystick_bind` accepts
  the device words and speaks the registry's language downstream.

## Adding a word

Ask which axis it is on. A new device gets a device word; a new way of
reading an existing device gets a strategy word, and must not borrow the
device's name — that is exactly how `dpad` came to mean two things.

# Migrating a rig off hand-edited `local/*.tcl`

A runbook. Written after doing it on three machines (a dev Mac, rpi500,
officepi pending) and getting it wrong twice in ways worth recording.

## What the migration is

Three kinds of value, three homes. The whole point is that none of them is a
number a person types into a file:

| kind | example | home | written by |
|---|---|---|---|
| **measured** | stick centre, orientation, throw | `db/calibration.db` | `slider::cal_*` |
| **declared** | which transport answers a button | `local/rig.tcl` | `settings::put -persist` |
| **derived** | the threshold in output units | nowhere — computed | at bind time |

What legitimately **stays** in `local/*.tcl`: things a human genuinely
declares that have no schema yet — GPIO line setup, `set_obs_pin`, board-type
switches, subsystem sources (`slider::set_source`), scales and limits.

## 0. Survey — and grep EVERY local file

    ssh <rig> 'grep -rn "button_bind\|joystick_bind\|dial_bind" \
                    /usr/local/dserv/local/*.tcl | grep -v EXAMPLE'
    ssh <rig> 'grep -nE "set_center|set_chan|set_invert|full_scale" \
                    /usr/local/dserv/local/slider.tcl'

**Do not check one file and conclude.** Bindings live in BOTH
`post-input.tcl` and `post-pins.tcl`, and different rigs put them in different
ones. On the dev Mac they were in both — and since `post-pins` sorts after
`post-input`, it silently won, so an analog joystick binding in
`post-input.tcl` had never taken effect from disk at all. Nothing reported
this; the two files simply disagreed and the later one was the answer.

That is the single best argument for the migration, and it is invisible until
you grep the whole directory.

## 1. Measured values first (if the rig has an analog input)

The stick must be **streaming** — a `continuous` ain group, not `onchange`.
An on-change group publishes NOTHING at rest, so a rest mark would capture
whatever the last movement left behind. `cal_mark` refuses with a message
saying so.

    dservctl -H <rig> slider 'slider::cal_begin'
    # at rest
    dservctl -H <rig> slider 'slider::cal_mark rest'
    # holding UP
    dservctl -H <rig> slider 'slider::cal_mark up'
    # holding RIGHT
    dservctl -H <rig> slider 'slider::cal_mark right'
    # after sweeping every mechanical stop
    dservctl -H <rig> slider 'slider::cal_mark sweep'
    dservctl -H <rig> slider 'slider::cal_apply'

Notes that cost time to learn:

- **Two perpendicular pushes, not one.** "Up reads as right" is equally
  consistent with a 90-degree rotation and with a reflection, and they need
  different fixes. A PSP stick on officepi turned out to be a rotation, which
  the obvious column swap would NOT have fixed.
- **Hold still and mark promptly.** A position mark reads the last 0.5 s. It
  refuses if the stick is still moving.
- **Calibrate in the FINAL mounted orientation.** For a hand-held PCB, "up" is
  a property of your grip. A rotation held consistently through the whole
  calibration is undetectable by definition — the 8-direction sweep afterwards
  is what checks the result against the frame the task uses.
- **Two analog inputs need two profiles**, or one device's mapping is forced
  onto the other:

      set slider::cal_profile stick     ;# beside the branch that selects it

If the hardware is absent and you only want to relocate values already in the
file, `slider::save_calibration` writes the current settings' learned keys to
the db as a one-time carryover.

Then delete the measured lines from `local/slider.tcl` (`set_chan_*`,
`set_center_*`, `set_invert_*`, `dservSet slider/full_scale`) and leave a
pointer comment. They are DEAD once the db has them — loaded after the file,
they override it, and the boot log says so.

## 2. Declared bindings

Run these in the **ess** interp, which is where the declarations live:

    dservctl -H <rig> ess 'settings::put joystick transport analog -persist'
    dservctl -H <rig> ess 'settings::put button 0 box:*/response/left -persist'
    dservctl -H <rig> ess 'settings::put button 1 box:*/response/right -persist'
    dservctl -H <rig> ess 'settings::put dial sources stick -persist'

Route forms for buttons: `box:<dev>/<group>/<label>` (preferred —
board-independent), `box:<dev>/<pin>`, `gpio:<pin>`, `joystick:<bit>`, `none`.

**Do not carry an absolute joystick threshold across.** It is a measured
quantity in disguise: the right value was 2.0, 3.9 and 3.8 on three sticks in
one afternoon. Omit it and the default is 40% of `slider/full_scale`;
`setting joystick threshold_frac` overrides that.

`put -persist` validates first, so a typo is refused at the door rather than
becoming a channel that silently never fires.

## 3. Remove the imperative lines

Delete the `*_bind` calls from `post-pins.tcl` / `post-input.tcl` and append a
comment saying where the routing went. Keep a `.bak-declared` copy.

Migrate a binding even if it currently resolves to nothing — it records
intent, and `ess/inputs/<key>` is what reports whether hardware answers.

## 4. Verify by WIPE and cold reload

This is the step that makes the migration real. Anything short of it can pass
on leftover runtime state:

    dservctl -H <rig> ess '
      ::ess::joystick_bind {}; ::ess::dial_bind {}
      ::ess::button_bind 0 ""; ::ess::button_bind 1 ""
      source $dspath/lib/ess_dial-1.0.tm
      source $dspath/lib/ess-2.0.tm
      foreach f {post-pins post-input} {
        if { [file exists $dspath/local/$f.tcl] } { source $dspath/local/$f.tcl }
      }
      ::ess::joystick_init
      list joy [::ess::joystick_bind] btn0 [::ess::button_bind 0] \
           dial [::ess::dial_bind]'

Everything must come back from the declarations alone.

**A status read immediately after a reload can lie.** Resolution consults the
box's live manifest datapoints, so `ess/inputs/button/0` may report
`unresolved` for a moment and then be `ok`. Re-read before believing it — this
cost a wrong conclusion about rpi500's buttons, which work fine.

## 5. What a page can then bind to

    settings/<sub>/<key>          effective value
    settings/<sub>/<key>/source   default | file | runtime
    settings/<sub>/<key>/schema   type, allowed values, doc

`/schema` is the binding surface: a page renders the right control from it
without hardcoding anything, and `/source` shows whether a value is a default,
declared in the file, or a live override.

## Status by rig (2026-08-21)

| rig | joystick | buttons | dial | slider calibration |
|---|---|---|---|---|
| dev Mac | declared | declared | n/a | pending |
| rpi500 | declared | declared | declared | in db |
| raspberrypi | nothing to migrate — never had imperative bindings or a `local/slider.tcl` | | | |
| officepi | imperative | imperative | imperative | not run |

A rig with nothing to migrate is a normal outcome. Check before assuming
there is work.

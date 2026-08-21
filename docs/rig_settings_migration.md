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

## 3a. Swapping a box: the LABEL form is what survives

Prefer `box:<dev>/<group>/<label>` over `box:<dev>/<pin>`. Measured on
psychophysics 2026-08-21, swapping an rp2350 box for an mcxn947: `btn_left`
moved from `response` bit 1 to bit 0, on different pins, and **the host config
needed no edit at all** — the label resolves through the box's own manifest.
A pin-addressed binding would have silently pointed at the wrong line.

So the new box only has to reproduce the NAMES: the group label, and the
member labels. Pin numbers are free.

**JOYSTICK LABELS MUST BE BARE DIRECTION WORDS. Buttons token-match; the
joystick does not.** `button_group_map` splits on `_`/`-`, so `btn_left`
resolves to `left`. `joystick_dir_canon` is an exact switch on
`up|down|left|right` (or `u|d|l|r`), so `joy_up`, `resp_up`, `hat_up` do NOT
canonicalize. Labelling the joystick consistently with the buttons is the
natural thing to do and it is exactly what this punishes.

With no canonical label, `joystick_map_for` warns and falls back to
POSITIONAL bits 0-3 = up, down, left, right in ascending pin order. On
psychobox (`joy_down`/`joy_up`/`joy_right`/`joy_left` on pins 2,3,4,6) that
fallback would have given a clean up<->down AND left<->right flip — the same
symmetric flip that cost a rig session once before, where the ess decode was
blameless.

Check the map BEFORE binding, and confirm it is not the fallback:

    dservctl -H <rig> ess '
      array unset ::ess::joystick_maps; array set ::ess::joystick_maps {}
      ::ess::joystick_map_for extio/<box>/state/group/joystick'

The fallback is exactly `{up 0 down 1 left 2 right 3}`. Anything else is
label-derived and therefore honest about the wiring.

## 3b. A relabel does NOT invalidate the resolver's cache

After relabelling a box, `ess/inputs/*` keeps reporting the OLD labels until
the cache is flushed or dserv restarts — and it lies in the alarming
direction, saying `unresolved` for a rig that is actually fine:

    status unresolved ... detail {no direction labels in {joy_down joy_up ...}}

while `joystick_map_for` on the same box already derives the correct map.
Two independent caches: `joystick_maps` (which `joystick_init` clears) and
`button_group_maps` (which nothing clears on a `state/label/*` change).

    dservctl -H <rig> ess '
      array unset ::ess::button_group_maps;   array set ::ess::button_group_maps {}
      array unset ::ess::button_group_warned; array set ::ess::button_group_warned {}
      ::ess::joystick_publish_status
      ::ess::button_publish_status 0
      ::ess::button_publish_status 1'

Worth fixing at source: `button_group_map` should invalidate on a label
update, the way `extioconf` already purges its decoded map.

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

## 6. The env-var files — and this one migrates ITSELF

`local/pre-remote.tcl` and `local/pre-registry.tcl` are the other hand-edited
settings, and they are a different shape from bindings: one value that three
or four interps each read, distributed by the process environment
(`ESS_RMT_HOST`, `ESS_REGISTRY_URL`, `ESS_WORKGROUP`). MAIN declares those —
`config/rig_settings.tcl`, sourced by dsconf before the first subprocess —
and exports them to the environment, so every reader downstream is unchanged.
See §3 of `docs/settings_panel_plan.md` for why they cannot be declared in the
subsystem that uses them.

**There is no manual step.** On the first boot after the upgrade the existing
exports are ADOPTED into `local/rig.tcl` and the rig behaves identically:

    rig_settings: adopted ESS_RMT_HOST into local/rig.tcl (setting stim host 192.168.88.50)
    rig_settings: adopted ESS_WORKGROUP into local/rig.tcl (setting registry workgroup brown-sheinberg)
    rig_settings: local/pre-registry.tcl is superseded by 'registry url/workgroup' in local/rig.tcl -- edits to it no longer take effect; safe to delete

Then delete the legacy file. Leaving it is not dangerous — the declaration
wins from the second boot on — but it is a file that looks like configuration
and no longer is, which is the trap this whole migration exists to remove.

`local/mesh.tcl` usually goes too: mesh falls back to the same declared pair
when that file does not call `mesh_configure`, and on every rig here it was
the same URL and workgroup written a second time.

Afterwards:

    essctrl -c 'settings::dump'                    # sub key value source, in main
    essctrl -c 'dservGet settings/stim/host'
    essctrl -c 'settings::put stim host 192.168.88.50 -persist'

**What does NOT move:** `ESS_IPADDR` stays a file override — it is derived at
boot from the route toward the stim host, and the derivation beats what a
person types. And **dserv-agent keeps its own flags**: it is a separate
process that must work while dserv is down, so a put cannot move it. Match
its unit file and restart it.

## Getting to a rig at all

The lab rigs sit on their own LAN behind a MikroTik; the office reaches a
CONSOLE by a forwarded ssh port, and the dserv host is another hop in. So:

    Host rig1                    # the console, via the MikroTik's forwarded port
        HostName 10.2.145.116
        Port 322
        User lab
        IdentityFile ~/.ssh/id_qpcsnet
        IdentitiesOnly yes

    Host rig1-dserv              # the dserv host, one hop further
        HostName 192.168.88.40
        HostKeyAlias rig1-dserv
        User lab
        ProxyJump rig1
        IdentityFile ~/.ssh/id_qpcsnet
        IdentitiesOnly yes

**EVERY RIG LAN REUSES 192.168.88.x, SO EVERY HOST NEEDS A `HostKeyAlias`.**
Without it, rig1's `.40` and psychophysics's `.40` are the same key in
`known_hosts`, and the second one you touch fails with
`REMOTE HOST IDENTIFICATION HAS CHANGED`. This is not hypothetical — it bit on
`.50` (the stim partners) on 2026-08-21. `HostKeyAlias` keys `known_hosts` by
the alias instead of the address, so each host is tracked separately.

`IdentitiesOnly yes` matters too when several keys exist: ssh otherwise offers
them all in order and a server with the default `MaxAuthTries 6` can refuse
before reaching the right one — which reads as "permission denied", not "I ran
out of attempts".

The key must be on BOTH hops. `ssh-copy-id -i <key>.pub <alias>` handles the
second one through `ProxyJump` once the first is in.

`dservctl` has no port option (it always dials 2560), so a tunnel would have to
land on the local 2560 and collide with a dev machine's own dserv. Running
`dservctl` ON the rig over ssh avoids that entirely, and is what the commands
in this runbook assume.

## Status by rig (2026-08-21)

| rig | joystick | buttons | dial | slider calibration |
|---|---|---|---|---|
| dev Mac | declared | declared | n/a | in db (`stick` profile) |
| rpi500 | declared | declared | declared | in db |
| raspberrypi | nothing to migrate — never had imperative bindings or a `local/slider.tcl` | | | |
| officepi | declared | declared | declared | in db |
| rig1 | unbound (hw present, labelled) | declared **by pin** | n/a | n/a |
| psychophysics | declared | declared | n/a | n/a |

A rig with nothing to migrate is a normal outcome. Check before assuming
there is work.

**rig1 is the one with work left.** Its buttons are pin-addressed
(`box:*/14`), which will not survive the mcxn947 swap — put pins 13/14 into a
labelled group on the box first, as pins 6-9 already are, and the binding
becomes `box:*/<group>/btn_left` and stops caring about pins. Its joystick
group is wired and canonically labelled but bound to nothing;
`setting joystick transport box_group` is the one line, after checking the map
is label-derived per 3a.

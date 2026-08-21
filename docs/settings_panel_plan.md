# Panels for settings and calibration — a work order

Two ess_control panels that the settings/calibration backends now make
possible: a **schema-driven settings gear**, and a **calibration wizard**.
They share one prerequisite (§0) and one transport, but they are different
UI shapes and should not be merged.

Facts below were verified on 2026-08-21, not recalled.

## 0. PREREQUISITE — the ownership gap

A page reads every knob from one datapoint tree:

    settings/<sub>/<key>          effective value
    settings/<sub>/<key>/source   default | file | runtime
    settings/<sub>/<key>/schema   default, values, type, doc, validate, apply

but it cannot WRITE one, because `settings::put` must run in the interp that
DECLARED the knob — `joystick transport` in `ess`, `juicer destination` in
`juicer`, `extio obs_autobind` in `extio` — and nothing in the schema says
which. `apply ::juicer_bind` hints; it is not an address.

The transport itself is already generic and already shipping
(`ESSControl.js:3867`):

    connection.evalAsync(`send ${interp} {settings::put ${sub} ${key} {${v}} -persist}`)

So the whole gap is the missing word `${interp}`.

**AN INTERP CANNOT LOOK UP ITS OWN NAME.** Verified: no `::dservInterpName`,
no global matching `*name*`, no `argv0`. So it has to be told.

**Fix, one line where the name is already known.** `dsconf.tcl`'s `subprocess`
wrapper has `$who`; inject it into the child before its config is sourced:

    set ::dserv_interp $who      ;# in the subprocess wrapper

Then `settings::declare` records `[expr {[info exists ::dserv_interp] ?
$::dserv_interp : "main"}]` in the declaration, and `_publish_declared` emits
it in the schema. Every current declarer (`ess`, `juicer`, `extio`, and
`slider` once calibration is exposed) is a subprocess, so the wrapper covers
them all; `main` is the honest fallback.

Do NOT pass the name to `declare` explicitly. It would be boilerplate at every
call site and the one place someone typo'd it would route writes into the
wrong interp — silently, since `send` to a live interp succeeds and the
setting simply would not exist there.

## 1. The settings gear

The pattern already ships: `ESSControl.js:191` has an
`ess-btn-juice-settings` ⚙ opening an `ess-modal-overlay`. What it does NOT do
is read the schema — it hardcodes its controls and calls named setters
(`juicer_set_destination`). Generalising means the juicer's dialog eventually
collapses into the same component.

**Render from `/schema`, in this order:**

| condition | control |
|---|---|
| `values` present and contains no `<...>` | `<select>` of those values |
| `type bool` | checkbox |
| `type int` / `type double` | number input |
| otherwise | text input |

**`values` is not always a list of choices.** `juicer destination` declares
`extio:<box>/<pin>` — documentation for a shape, not a literal option. A naive
dropdown would offer a nonsense value. Rule: if any entry contains `<`, treat
the whole list as HINTS beside a free-text field, not as options. `type` is
empty on several knobs (`values` carries the constraint), hence the fallback
order above rather than switching on `type` alone.

**Show `/source`.** `default` / `file` / `runtime` is the difference between
"nobody has decided this" and "this rig declares it", and it is the first
thing to look at when a rig behaves unexpectedly. A quiet badge beside the
control.

**Write:** `send <interp> {settings::put <sub> <key> {<v>} -persist}`.

**Errors are the feature.** `put` validates first and throws a message written
to teach ("button route 'box:*/response': the two-part form is <dev>/<pin>…").
Show it verbatim in the modal rather than "invalid value". Note `send` returns
Tcl errors as `!TCL_ERROR ` strings that look like success — `evalAsync`
already normalises that, which is why the juicer path uses it.

**Nothing to re-read after a write.** `-persist` fires the `-apply` chain, and
the effective value and `/source` re-publish themselves; the panel updates
from the datapoint like everything else.

## 2. The calibration wizard

**Do not fold this into the gear.** A settings modal is a form: read, render,
write. Calibration is a sequence with the operator's hands in it, and the
backend is already shaped for it — `slider/cal/status` publishes

    active <0|1>  stage <rest|up|right|sweep|done>  samples <n>  msg <text>

**Flow**, one button per step, each `send slider {…}`:

    slider::cal_begin
    slider::cal_mark rest     # "hold at rest"
    slider::cal_mark up       # "push UP and hold"
    slider::cal_mark right    # "push RIGHT and hold"
    slider::cal_mark sweep    # "sweep to every stop"
    slider::cal_apply         # writes db/calibration.db

`slider::cal_cancel` restores whatever was in force, so Cancel is safe at any
point and the modal should offer it throughout.

**Show a live readout** of `slider/raw` and `slider/position` during the whole
wizard. It is what tells an operator the stick is actually streaming, and it
turns the commonest failure (an on-change group that publishes nothing at
rest) into something visible rather than a refusal they have to interpret.

**The refusals are already user-facing.** They name the cause and the fix
("the stick was still MOVING (sd 12.4/3.1 counts) — hold it steady, then
mark"; "the 'right' push was not clean — its two columns moved by a ratio of
only 2.1"). Render them as guidance, not as errors, and let the operator
re-mark: a bad mark costs a re-mark, never a restart.

**Then confirm with the 8-direction sweep, in the same modal.** Deriving the
transform is not the same as checking it against the frame the TASK uses —
that check is what caught a 90° rotation on officepi that a swap alone would
not have fixed. The dpad panel already renders `ess/joystick/dir`; reuse it,
prompt for the eight directions in a known order, and show pushed-vs-reported.

**Profiles.** A rig with two analog inputs needs `slider::set_cal_profile`
per input or one device's mapping is forced onto the other. The wizard should
show which profile it is about to write and refuse to guess when
`local/slider.tcl` names none.

## Sequencing

1. **§0 first, alone.** It is a one-line wrapper change plus two small edits
   in `settings-1.0.tm`, and both panels are blocked on it. Ship and verify
   `settings/<sub>/<key>/schema` carries `interp` on a real rig before writing
   any UI.
2. **The gear**, against `joystick`/`button`/`dial` — knobs with real `values`
   lists, so the select path gets exercised first.
3. **The wizard**, which is the larger piece and needs a rig with a stick.
4. **Fold the juicer dialog into the gear** last, if at all: it works, and
   replacing working UI is the least valuable step.

## Risks

- **Writing to the wrong interp fails silently-ish.** `send ess {settings::put
  juicer destination …}` reaches a live interp where that knob is not
  declared; `put` errors, but only because `_effective` checks the schema. Do
  not add a fallback that tries other interps — a wrong route should fail.
- **The schema is per-interp.** A page sees `settings/*` from every interp,
  but each interp only knows its own declarations. Anything that enumerates
  knobs must do it from the DATAPOINT TREE, never by asking one interp.
- **`values` wildcards** (§1) — the one place a generic renderer produces a
  wrong control rather than an ugly one.
- **Do not let the wizard write `local/slider.tcl`.** Measured values belong
  in the db; that separation is the whole point of the migration and a panel
  that "helpfully" edits the file would undo it.

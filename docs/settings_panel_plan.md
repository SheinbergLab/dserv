# Panels for settings and calibration — a work order

Two ess_control panels that the settings/calibration backends now make
possible: a **schema-driven settings gear**, and a **calibration wizard**.
They share one prerequisite (§0) and one transport, but they are different
UI shapes and should not be merged. §3 is not a panel — it is the class of
knob that would break the gear if it were declared the obvious way, and the
tenants worth feeding it.

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

Do NOT pass the name to `declare` explicitly. It would be boilerplate at every
call site and the one place someone typo'd it would route writes into the
wrong interp — silently, since `send` to a live interp succeeds and the
setting simply would not exist there.

### DONE (2026-08-21) — and not where this plan first put it

The plan above said to inject it from `dsconf.tcl`'s `subprocess` wrapper,
which has `$who`. That wrapper is the wrong place: it early-returns once
`::dsconf_booting` is gone, so every interp spawned after boot
(`virtual_subject`, `virtual_extio`, `-link` sandbox children) would get no
name, and main would have none either.

Shipped instead as one `Tcl_SetVar2Ex` in `Tcl_DservAppInit`
(`TclServer.cpp`), where `tserv->name` is already in hand and every interp
without exception passes through:

    set ::dserv_interp <registry name>

`settings::declare` stamps `[_interp_name]` into the declaration after the
option loop — so `-interp` stays an unknown option by construction — and the
`settings/<sub>/<key>/schema` datapoint carries it. `settings::interp_of`
returns it for anyone driving settings from `essctrl`.

**The value is the REGISTRY name, and two of them are not send targets:**

| value | what a page does |
|---|---|
| `ess`, `juicer`, `extio`, … | `send <name> {settings::put …}` |
| `dserv` | the main interp — `send` REFUSES it ("cannot send directly to dserv"); evaluate directly |
| empty | not addressable: a nameless one-off interp, or the module under plain tclsh. Render the knob read-only |

`main` was the plan's fallback and would have been a lie — no interp answers
to it. Verified running (`--cscript`): main reports `dserv`, a child reports
its own name, and its schema datapoint reads
`… apply {} interp probe`. Covered by `tests/test_settings.tcl` (`ctest -R
settings`), which also pins the `-interp` rejection.

**§1 gate PASSED** on the Mac rig, 2026-08-21: all 13 declared knobs carry a
name and none is empty — `button/*`, `dial/sources`, `joystick/*` → `ess`;
`juicer/*` → `juicer`; `extio/obs_autobind` → `extio`. The live route answers
too (`send ess {settings::interp_of joystick transport}` → `ess`).

Note for every other rig: the binary and `lib/settings-1.0.tm` must be
installed TOGETHER. Half-installed (new binary, Aug-15 module) the field is
simply absent from every schema — `::dserv_interp` is set but nothing copies
it — so a panel must treat a missing `interp` key as read-only rather than
assume a route.

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

### DONE (2026-08-21) — `www/js/SettingsModal.js`

A ⚙ Settings button in the ess_control status bar, beside Sync Tasks. One
`evalAsync` walks `dservKeys settings/*/schema` and returns value + source +
schema per knob; the modal groups by subsystem and renders each control by
the table above. `settings/*` is subscribed, so a write's effect arrives the
same way every other value does.

Browser-verified against the live rig on the dev Mac (`python3 -m http.server`
in `www/` + `?host=localhost:2565&ssl=1` — no install needed):

- 16 knobs, every one carrying its owner: `ess`, `extio`, `juicer`, `dserv`.
- `joystick transport` renders the select (`none analog box_group`);
  `stim host` renders free text with `accepts: localhost <host> <ip>`.
- **The error path is the one that proves the design.** Typing
  `http://192.168.88.50` into `stim host` returned "…is a URL — give the host
  alone (stim2 listens on port 4612)" verbatim, the control snapped back to
  the truth, and nothing was written. That message exists in
  `config/rig_settings.tcl` and the panel never had to know about it.
- Both routes exercised: `interp dserv` evaluated directly, `interp ess`
  through `send`, both idempotent (`local/rig.tcl` byte-identical after).

**Two backend defects the gear surfaced**, both fixed in `settings-1.0.tm`:

1. `declare` published only the SCHEMA. A knob declared after the first
   `load` had no value and no `/source` in the tree — present, unreadable,
   unwritable. That is exactly how `joystick box_device` and `box_group`
   looked in the first render. `declare` now publishes the effective value
   too.
2. Worse, and found by the fix: a file line for a not-yet-declared SUB is
   stored raw, so whichever knob triggered the first `load` left every later
   declaration's file value **unvalidated**. `declare` now judges a held line
   when the knob becomes known. Order-dependent validation across modules is
   not something anyone would ever have seen.

Left for later: `juicer ms_per_ml`/`hand_ml` gained `-type int`/`-double` so
they render as number inputs; the juicer's own dialog is untouched (step 5);
and a rig needs the new module installed before `box_device`/`box_group` show
a value.

**Layout, second pass.** Seven groups of one to four knobs is a long scroll
for a question that is always about ONE subsystem, so the modal is now a
subsystem list beside a single section's knobs. Two things fell out of it
that are worth keeping in mind for any later panel:

- **The sidebar cannot answer "where is the thing called X"**, because the
  asker does not know which subsystem owns it — that is why they are asking.
  So there is a filter over key, value and doc, and a query that matches
  nothing in the current section but something elsewhere widens to All
  rather than showing an empty pane beside a sidebar that says the match
  exists.
- **The nav counts declared/total** (`joystick 1/4`), which turns out to be
  the fastest answer to "what has this rig actually decided?" — the question
  `/source` exists for, asked at rig scale instead of per knob.
- **No focus gates.** The first cut skipped updating a control while it held
  focus; `www/CLAUDE.md` names that as a bug class (Chrome focuses every
  control it is clicked on, Safari/macOS almost none, so the gate is
  invisible on the dev Mac and freezes the panel on Windows). Replaced with
  a dirty flag set on `input` — half-typed text survives a background
  update, the `/source` badge updates regardless, and a put's own outcome
  forces through.

**Taking a value BACK, and showing the rejected lines.** Two things the
panel made obviously missing, both landed 2026-08-21:

- **`settings::clear <sub> <key> ?-all?`.** Every other verb only added:
  `put` wrote an override or a file line and nothing removed either, so a
  rig could not return a knob to stock without hand-editing the file this
  module exists to stop people hand-editing. `clear` removes the layer
  `/source` is REPORTING — the runtime override if there is one, else the
  file line — so the badge and the button agree, and a knob carrying both
  takes two clicks with the badge changing in between. `-all` strips both.
  `-apply` fires only when the effective value actually changed (clearing an
  override that matched the file value must not re-bind hardware for
  nothing). The generated `# persisted …` comment is removed with its line;
  a human's comment on the line above is not.
- **`settings/parse_errors` is now `settings/parse_errors/<interp>`.** A
  rejected line costs a breadcrumb and a fallback — but nothing ever showed
  the breadcrumb, which made "I set it in the file and the rig ignores me"
  unanswerable without an essctrl session. Publishing per interp is not
  cosmetic: every interp parses the whole file but judges only its own
  declarations, so `ess` sees the bad joystick line and `juicer` does not,
  and one flat datapoint meant last-writer-wins — the errors a page showed
  depended on boot order. The gear renders them as a strip above everything.

Verified on the live rig by hot-patching the new procs into main (the
`www/CLAUDE.md` pattern) and driving a throwaway declared knob: ↺ removed
the line, the badge went `file` → `default`, the button removed itself, and
`local/rig.tcl` came back byte-identical by md5. A deliberately bad line
then produced the strip — "1 line in local/rig.tcl rejected — the default is
in force for this", attributed to `dserv` — and clearing it emptied the
strip again.

**Open-at-section, and "declared only".**

- `openSettingsModal('juicer')` opens straight at one subsystem, applied
  after the load and only if that subsystem declares anything (a rig that
  never started the juicer would otherwise open on an empty pane). Its first
  caller is the juicer's own dialog, which now carries a **⚙ All juicer
  settings** hand-off in its footer — the useful half of step 5 without
  replacing UI that works: that dialog knows about pumps and pins, the gear
  knows every juicer knob and where each value came from.
- **"declared only"** filters to `file`/`runtime`, i.e. everything this rig
  has DECIDED — 5 of 16 here, on one screen, which is the form the question
  takes when comparing two rigs. It shares the filter's widen-to-All rule:
  narrowing a section to nothing while matches exist elsewhere shows those
  instead of an empty pane.
- Clearing a knob under "declared only" has to make its row LEAVE, so the
  pane rebuilds when the visible SET changes and updates in place otherwise
  — extio-config's structure/values split, for the same reason (a rebuild
  loses half-typed text). Verified: ↺ on the throwaway knob dropped its row
  and took its nav count 1/1 → 0, leaving the other five alone.

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

### DONE (2026-08-21) — `www/js/SliderCalModal.js`

Reached from the gear's `joystick` section (**⟲ Calibrate stick…**), through
a small `SECTION_ACTIONS` table so the gear stays schema-driven and the next
wizard is one line. A subsystem has knobs AND sometimes a procedure; the
section someone opens to set `joystick transport` is where they look for
"calibrate the stick".

**One backend addition: `slider/cal/live`** — the raw COLUMN VECTOR plus a
sample count, published from `cal_feed`, throttled 1-in-10 (200 Hz → 20 Hz)
and only while calibrating. `slider/raw` cannot serve this, and the plan was
wrong to suggest it: raw carries `chan_x`/`chan_y` AFTER selection, and which
columns those are is exactly what calibration decides — during it, the column
that matters may be one `slider/raw` is not publishing.

Verified end-to-end on the dev Mac by feeding synthetic samples into
`cal_feed` (the same entry point the hardware path uses) against a throwaway
`_wizardtest` profile:

- **The dead-stick case first.** With nothing streaming: "NO SAMPLES" and the
  on-change explanation, before any mark is attempted. Feeding samples flips
  it to "streaming" with the columns and a climbing count.
- **A refusal is guidance.** A deliberately wobbly `right` push returned "the
  stick was still MOVING (sd 120.0/0.0 counts over the last 40 samples). Hold
  it steady, then mark." — rendered verbatim, `rest`/`up` still marked, and
  `right` still the next step. A bad mark cost a re-mark.
- **Apply** derived `chan_x 1 / chan_y 0`, centers 2000/2000, no inversion,
  throw 500 counts — exactly what the synthetic pushes implied.
- **The 8-direction check** correctly refused to fake it with no system
  loaded: it needs `joystick_init` for `ess/joystick/dir`, and says so rather
  than showing an inert grid.
- **Closing the window cancels.** Abandoning a half-finished calibration is
  the likeliest exit, so it is the safe one: `slider/cal/status` came back
  "cancelled; previous settings restored" and `slider/settings` was identical.

**Two findings from doing it on a real rig:**

- **`local/slider.tcl` on the dev Mac declares MEASURED values by hand** —
  centers, channel swap, full_scale — and the `default` db profile was
  EMPTY. So `cal_cancel`/`load_calibration` cannot restore them: there is
  nothing stored to restore, and only a restart re-reads the file. Not a bug
  (apply is meant to replace them) but worth stating in the panel, so the
  result now says the db wins from here and those file lines have stopped
  being in force.
- **This rig is exactly the two-input case the profile warning exists for**
  (trackpad + extio stick, switched by `set INPUT` in the file) and its
  profile is still `default`. The wizard names the profile it will write and
  flags `default` in amber rather than refusing: it cannot detect a second
  input, but it can refuse to be silent about where the measurement lands.

## 3. Rig facts SEVERAL interps read — and why main must declare them

`joystick transport` and `juicer destination` are the easy shape: one
subsystem owns the knob, declares it, validates it, applies it. The rig has
another shape that looks identical and is not — a fact about the rig that
three or four interps each need a copy of, distributed today by the process
ENVIRONMENT, set in a `local/pre-*.tcl` before any subprocess starts.

Three of them, verified 2026-08-21:

| env var | set in | read by |
|---|---|---|
| `ESS_RMT_HOST` | `local/pre-remote.tcl` | `stimconf.tcl:38` (`stim_host`); `ess-2.0.tm:4736` (`::ess::rmt_host` → `ess/rmt_host`); `dsconf.tcl:211` in MAIN, to derive `ess/ipaddr` |
| `ESS_REGISTRY_URL` | `local/pre-registry.tcl` | `essconf.tcl:62`; `ess_sync-1.0.tm:382`; `scriptsconf.tcl` (via the same) |
| `ESS_WORKGROUP` | `local/pre-registry.tcl` | `essconf.tcl:65`; `ess_sync-1.0.tm:385`; `trialsyncconf.tcl:884` |

**Why the environment, and why `declare` in the owning subsystem cannot
replace it.** The environment is inherited, so it is ORDERING-FREE. `ess`
starts at `dsconf.tcl:138` and `stim` at `dsconf.tcl:282` — ess boots FIRST,
so if `stim` declared `stim host`, ess could never read it. Reaching across
at boot is the exact hazard `dsconf.tcl:203` documents from experience: a
datapoint read of another subprocess's value truncated the whole config.

**So MAIN declares this class.** Main parses `local/rig.tcl` itself, like
every interp does, before any child exists; it exports the effective value to
`env(...)`; every downstream reader stays byte-identical to what it is today.
`dsconf.tcl` already has `tcl::tm::add $dspath/lib` (line 15) and would gain
`package require settings`. The `interp` field on those knobs then reads
`dserv` — the "evaluate directly, `send` refuses it" case §0 made explicit,
and the first real exercise of that path in the gear.

**The apply hook has to FAN OUT, and must not block.** Setting the env var
only affects the next boot; the live copies are in ess, stim, scripts,
trialsync. So `-apply` pushes: `stimOpen`, `::ess::rmt_host`,
`ess::registry::configure -url/-workgroup`. Use **`sendNoReply`**, not
`send`: a `send` from main blocks on the child's reply queue, so one wedged
subprocess would wedge MAIN — the request loop every page and every rig
tool rides on — from a settings write. That is a much worse failure than a
setting that did not reach one interp.

**Some values need a RECONNECT, not a reassign.** `stimSend` opens a socket
per message (`stimconf.tcl:5`), so stim switches hosts on the very next
message for free. Ess does not: `configure_stim`/`rmtOpen`
(`ess-2.0.tm:313`) holds a connection to the OLD host and has already
fetched screen geometry from it. Either the apply hook reconnects, or the
gear's "nothing to re-read after a write" promise is a lie for this knob.
Decide per knob and say which in `-doc`; the panel should show it.

**Registry/workgroup is worse than stim, and the reason to do it.** The same
two values are written in THREE independent places on this box, with nothing
keeping them in agreement:

    local/pre-registry.tcl   ESS_REGISTRY_URL / ESS_WORKGROUP   (ess, scripts, sync, trialsync)
    local/mesh.tcl           mesh_configure "https://dserv.net" "brown-sheinberg"
    dserv-agent's flags      --registry https://dserv.net --workgroup brown-sheinberg

One declaration with an apply that reaches mesh (`mesh_configure`) and the
registry namespace collapses the first two. **It does not reach the third**:
dserv-agent is a separate OS process with its own command line, started
before dserv and deliberately kept from polling dserv (that separation is
load-bearing — the agent must stay useful when dserv is down). A put cannot
move a running agent. The setting can be the source of truth an
updater/installer writes into the unit file, and the panel should say
"agent restart required" rather than pretend. Do not wire the agent to read
dserv's settings tree to close this gap.

**What stays OUT of this section.** `ESS_IPADDR` is DERIVED, not declared —
`dsconf.tcl:186-230` picks the route-toward address for a remote stim2 and
loopback for a local one, which beats what a human will type. If an override
is wanted it is a separate knob, `-default ""` meaning "derive", declared in
main beside the derivation. Box identity likewise stays in `box.conf`: a
rig's name is not a runtime setting.

**One hard rule the panel depends on.** Never declare the same key in two
interps. The module permits it, but `settings/<sub>/<key>/schema` would carry
whichever declared LAST, and a put would update only that interp's runtime
dict — a knob that looks written and half is. When several interps need the
value, ONE declares and the apply hook pushes to the rest.

### DONE (2026-08-21) — all three, in `config/rig_settings.tcl`

Sourced by `dsconf.tcl` after the `local/pre-*.tcl` glob and before the first
subprocess, which is the only window where both halves are true: the legacy
exports exist to be adopted, and no child has inherited an environment yet.

- **`stim host`** (default `localhost`), **`registry url`**,
  **`registry workgroup`** (both default EMPTY — `ess::registry`'s own
  defaults are empty, and a default of `https://dserv.net` would quietly
  enroll every unconfigured rig).
- **The migration runs itself.** If the knob is still `source default` and
  the legacy env var is set, the value is adopted into `local/rig.tcl` with
  `put -persist` and the boot prints what it did — same one-time carryover
  `obs_autobind` used. From the second boot the declaration wins even with
  the legacy file still in place, and a declared EMPTY value *unsets* a stale
  export, so "no registry" stays sayable.
- **`local/mesh.tcl` folds in too**: mesh falls back to the declared pair
  when that file does not `mesh_configure`. Same two values, previously
  written twice.
- **No `-values` on the registry knobs.** `_validate`'s wildcard branch
  matches only a NON-empty value, so any values list at all makes `""`
  unwritable — the shape goes in `-doc` instead. Worth remembering when the
  gear renders: a knob whose empty state is meaningful cannot carry hints
  this way.
- Covered by `tests/test_rig_settings.tcl` (`ctest -R rig_settings`), which
  boots each scenario in its own child interp — first boot, second boot, bare
  rig, declared-empty, validation, fan-out, and a bad line staying non-fatal.

Not yet done on a rig: the adopt-and-delete pass. Every rig prints its own
instructions at boot; `docs/rig_settings_migration.md` §6 is the runbook.

## Sequencing

1. ~~**§0 first, alone.**~~ **DONE and rig-verified** — one line in
   `TclServer.cpp`, two small edits in `settings-1.0.tm`, a unit test. Both
   panels were blocked on it. Every other rig needs the binary and
   `lib/settings-1.0.tm` installed together before it trusts the field.
2. **The gear**, against `joystick`/`button`/`dial` — knobs with real `values`
   lists, so the select path gets exercised first.
3. **§3's `stim host`**, once the gear renders — the first `interp dserv`
   knob and the first free-text-with-hints knob, so it exercises both paths
   the ess/juicer tenants do not. Registry/workgroup after it.
4. **The wizard**, which is the larger piece and needs a rig with a stick.
5. **Fold the juicer dialog into the gear** last, if at all: it works, and
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
- **A §3 apply hook can wedge main.** It runs in the MAIN interp and fans out
  to children; `send` blocks on the child's reply queue, so one stuck
  subprocess would take the request loop down from a settings write. Use
  `sendNoReply` in those hooks, always.
- **A missing `interp` key means an old `lib/settings-1.0.tm`**, not a knob
  without an owner: `::dserv_interp` comes from the binary and the stamp from
  the module, so a half-installed rig has the field absent EVERYWHERE. Treat
  missing as read-only; never `dict get` it bare.

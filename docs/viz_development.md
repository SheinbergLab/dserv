# Writing and testing ESS viz configs

A protocol's **viz config** draws the experimenter's view of a trial in the
*Stimulus Display* panel of the ESS control GUI (`ess_control.html`). This
is the working reference for writing one, the traps the existing configs
have fallen into, and how to test one offline with `tools/viztest`.

The ESS skill (`ESS_SKILL.md`) has the short version of the API. This
document is about getting a viz to show what the subject actually sees and
to stay correct while a trial is running.

## Where a viz config runs

```
protocol.tcl:  $s set_viz_config { ... }
      │  published as ess/viz_config (a string) on system load
      ▼
config/vizconf.tcl  (its own subprocess — NOT the ess interpreter)
      │  cleanup_namespace, then  namespace eval ::viz::<system> $config
      │  stimdg rebuilt locally from the stimdg datapoint (dg_fromString)
      ▼
flushwin → dumpwin json → graphics/stimulus → www/js/GraphicsRenderer.js
```

- The config has **dlsh** (`dl_*`, `dg_*`, `dlg_*`, `clearwin`,
  `setwindow`, …) and the `::viz` helpers. It does **not** have the `ess`
  package, protocol variables, or params. Anything per-trial comes from
  `stimdg`. Anything live comes from a datapoint (`dservGet`, `vizSubscribe`).
- Handlers are registered with `evtSetScriptByName TYPE SUBTYPE proc`
  (preferred) or `evtSetScript type_id subtype_id proc`, and are called
  as `proc { type subtype data }`. A subtype of `*` or `-1` matches all
  subtypes.
- Event names and ids come from `evt_info` in `lib/ess-2.0.tm`. ess
  publishes them as `ess/evt_type_ids` and `ess/evt_subtype_ids`.
- Params are readable as a dict: `dict get [dservGet ess/params] cursor_scale`.

## Framing: use `::viz::setup_window`

Call `::viz::setup_window` in `setup` and again at the start of every
draw. Don't hand-pick `setwindow` bounds. It frames the window on the
**display**, at the display's own extents (`ess/screen_halfx|halfy`),
padded to the canvas aspect and divided by the operator's zoom
(`ess/viz/zoom`, default 2.0). One screen position then maps to the same
panel position in every protocol, and x and y are scaled the same.

- `-zoom 1.0` shows the whole screen. Use it when content reaches the edges.
- It returns `{x0 y0 hx hy}`, for placing text at the corners.
- `::viz::text_size small|normal|large|huge|tiny` gives font sizes that
  follow the operator's font control (`ess/viz/fontsize`).
- **Anything outside the window is clipped out of the frame entirely**,
  not drawn at the edge. If a target at 8° vanishes, check the window first.

## Sizes: `-size` is a diameter

For `dlg_markers` `fcircle` **and** `circle` (and `fsquare` etc.), `-size`
is a **diameter**. Add `-scaletype x` or an `x` suffix (`-size 1.5x`) to
give it in world units (degrees). `GraphicsRenderer.cmdCircle` halves it
for canvas `arc()`.

This used to be wrong: the renderer drew every circle at twice its size,
and marker sizes across the tree were tuned to compensate. Code comments
from that era survive. **"`circle` takes a RADIUS" is stale wherever you
see it.** The usual bug:

```tcl
# stimdg holds a RADIUS (the stim does scaleObj ... [expr {2*$r}])
dlg_markers $x $y circle -size ${r}x        ;# WRONG: draws at half size
dlg_markers $x $y circle -size [expr {2.0*$r}]x
```

Draw stimuli at their real size, taken from the same stimdg column the
stim file reads. Check the stim's `scaleObj` to learn whether a column is
a radius or a diameter. Save fixed sizes for markers that have no
counterpart on the display (path endpoints, a fixation icon on a
full-screen schematic). If the stim draws a highlight larger than its
target (for example 1.2× for pointer and 1.35× for feedback), draw it at
the same scale.

## Drawing: one path, from cached state

A viz receives two unordered streams: **events** (state-machine
transitions) and **datapoints** (a cursor, an eye position). A handler that
draws its own version of the scene will be overwritten by the next draw
from the other stream. A typical failure is a late cursor update repainting
the scene without the green/red outcome that ENDTRIAL had just drawn.

The pattern that holds up:

```tcl
variable showing 0     ;# between TARGET ON and TARGET OFF
variable response -1
variable outcome -1    ;# -1 pending, 1 correct, 0 wrong/abort

proc redraw_scene {} {               ;# the ONLY proc that draws
    variable showing; variable trial; variable outcome
    clearwin
    lassign [::viz::setup_window] x0 y0 hx hy
    if { $showing } { ...everything, from the variables above... }
    flushwin
}
proc redraw {} { redraw_scene }      ;# operator zoom/font hook

proc targets_on { t s d } { clear_trial; variable showing 1; redraw_scene }
proc response   { t s d } { variable response $d;  redraw_scene }
proc endtrial   { t s d } { variable outcome [expr {$s == 1}]; redraw_scene }
proc pointer    { dp data } { ...update cursor vars...; redraw_scene }
```

- Handlers **update state, then call the one draw proc**. Each draw
  re-derives everything, so the drawing order (targets, then highlight,
  then cursor) can't drift between handlers.
- **`proc redraw`** (no arguments) is what `::viz::on_display_control`
  calls when the operator changes zoom or font size. Without it the
  change waits for the next event. It must not have side effects beyond
  drawing, and it must call `setup_window` so the new zoom actually
  applies.
- Guard the draw against no or stale stimdg
  (`dl_exists stimdg:col`, `$trial < [dl_length ...]`). It can be called
  before a variant has loaded.
- Look highlight positions up from the stimdg target columns
  (`target_x:$trial $i`) rather than recomputing them from a sector or
  angle, so the highlight lands on the disc that was drawn.

## Live datapoints

```tcl
vizSubscribe ess/dial/pointer [namespace current]::pointer
```

- Use **`vizSubscribe`, never a bare `dpointSetScript`**. `cleanup_namespace`
  deletes the config's namespace on the next load but can't know about a
  raw subscription, which then throws on every update under the next
  system. `vizSubscribe` records it so it can be removed.
- **Throttle redraws by time, about 16 ms (60 Hz).** Every redraw sends the
  whole scene to the browser. 16 ms matches the browser's paint rate.
  30 Hz looks juddery because it lands 2 and 3 animation frames apart.
  See `forage.tcl`'s `redraw_ms` comment.
- **Never throttle a state change.** When the cursor is shown or hidden, or
  enters or leaves a target band, redraw immediately. Otherwise the last
  frame can leave a hidden cursor on screen.
- Accumulate trails by distance, not per update, and cap the point count.
  Frame size is the cost (`dumpwin json` is about 63 bytes per polyline
  point).

## dlg idioms

- `dlg_lines` takes an **x-list and a y-list**. Four scalars are misparsed
  as style arguments and quietly draw the wrong shape.
- Filled polygon: `dlg_lines $xs $ys -fillcolor $c -linecolor $c -closed 1`
  (renders as `fpoly`). Handy for arrowheads.
- Colors: `[dlg_rgbcolor r g b]` or names (`white`, `yellow`, `gray`, …).

## Testing offline: `tools/viztest`

`tools/viztest/viztest.tcl` runs a viz config in a plain `tclsh9.0`,
with no rig and no GUI, and doesn't touch the running dserv. It evaluates
the config the way vizconf does, using the **real** `setup_window`,
`text_size` and `set_base_font` from `config/vizconf.tcl` and the real
event tables from `lib/ess-2.0.tm`. Only the plumbing is stubbed. It
captures every `flushwin` as the exact JSON GraphicsRenderer draws.

```bash
cd tools/viztest

# smoke test: every handler once, against the stimdg a variant's REAL
# loader builds (run headless by dlsh's ess_test) -- no rig at all
tclsh9.0 viztest.tcl -variant left_right ~/systems/ess/joystick/motiondir/motiondir.tcl

# ... or against the stimdg ess has loaded right now
tclsh9.0 viztest.tcl -stimdg live ~/systems/ess/joystick/motiondir/motiondir.tcl

# the config and stimdg actually running, rendered to a page
tclsh9.0 viztest.tcl -html /tmp/viz.html live

# a scripted trial with assertions
tclsh9.0 viztest.tcl -v -scenario examples/motiondir_trial.tcl \
    -html /tmp/motiondir.html ~/systems/ess/joystick/motiondir/motiondir.tcl
```

- **`-variant <name>`** builds the stimdg with `ess_test::run_variant`, the
  protocol's own loader with that variant's defaults. This is the best
  offline stimdg: it's the real columns and values, so a viz that reads a
  column the loader doesn't write fails here and not on the rig.
- **The smoke test** (no `-scenario`) gives every event `data 0`. A handler
  that unpacks a real payload (`{resp correct}`, `"x y r"`) will reject
  that. Those errors are reported as `WARN`, not failures. Write a
  scenario with the real payload to check those handlers.
- **`-stimdg live`** / **`-live`** read from the running ess with `essctrl`:
  the stimdg, event tables, `ess/params` and screen extents. These are
  read-only. Without them you get the source event tables and a
  `-screen "24.8 14.2"` default display. A scenario can build its own
  stimdg with `dg_create stimdg`.
- **`-html`** writes a page of labeled snapshots, one after each event,
  rendered by `www/js/GraphicsRenderer.js` itself. Open it in a browser,
  or capture it headlessly:
  `chromium --headless=new --no-sandbox --screenshot=out.png --window-size=1010,2900 file:///tmp/viz.html`
- **Scenario helpers:** `fire TYPE SUBTYPE ?data?`, `dpoint name data`,
  `setdp`, `zoom z`, `snap label`, `summary`, `check expr msg`, and `vns`
  (the config's namespace, for reading its state). See the header of
  `viztest.tcl` and `examples/`.
- The exit status is non-zero if setup or a scenario step threw, a `check`
  failed, or the config logged an error (for example an unknown event name).
- **Braces in comments count.** The config body is a braced word, so an
  unbalanced `{` in a comment inside `set_viz_config` breaks it on the rig
  too. viztest counts them the same way.

## Reaching the running ess

- `essctrl -s ess -c '<script>'` does **not** evaluate inside the ess
  interpreter. Use `getVar ess <var>` for its variables (for example
  `getVar ess ::ess::current(system)`), as vizconf does.
- Dyngroup datapoints are binary. Fetch them as
  `binary encode base64 [dservGet stimdg]` and rebuild with
  `dg_fromString [binary decode base64 ...]`. Printed raw, they get
  mangled by the text transport.

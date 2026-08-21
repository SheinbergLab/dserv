# Extracting `ess_transports-1.0.tm` from `ess-2.0.tm`

A work order. The coupling below was measured on 2026-08-21, at
`ess-2.0.tm` 9193 lines. **Re-measure the line ranges before cutting** — they
drift with every edit; the structure will not.

## What moves

Four contiguous blocks, ~2250 lines, 86 procs:

| block | lines (2026-08-21) |
|---|---|
| joystick | 3946–4705 |
| buttons (incl. the `input_transport` registry + resolvers) | 4705–5690 |
| analog stick response shaping (`stick_gain`, `stick_velocity`) | 5875–5932 |
| slider support | 5932–6201 |

## What does NOT move

`em_windows`, `sound`, `juicer`, `touch_windows` — subsystems, not
experiment-facing input routing. `touch_windows` is the arguable one; it wraps
a C processor rather than being a bound transport, so leave it.

**`dial_bind` stays in `ess_dial`.** An earlier note here suggested moving its
`setting dial sources` declaration so "all routing lives together" — that was
wrong. `dial_bind` is six lines belonging to the dial, which is a response
MODE, not a transport. Moving a knob away from the thing it configures to
satisfy a filing preference is not an improvement.

## Why it is mechanical

**Both files are `namespace eval ess`**, so every cross-reference resolves at
call time with no qualifying, renaming, importing or exporting. `ess_dial`
already proves the pattern: it lives in `::ess`, calls `do_update` and
`respwin_active`, and has done for months.

**Outbound** — everything the moved code needs from what stays, after
stripping comments so prose stops counting as calls:

    do_update        10
    ess_warning       5
    respwin_active    1

That is the complete list. (`reset`, `start`, `stop`, `init`, `em_init` all
appeared in a naive grep and are prose; the one surviving `start` is
`send virtual_eye start`, a string for another interp.)

**Inbound** — 18 procs called from what stays, all the protocol-facing API:
`slider_x/y`, `slider_pos`, `slider_process`, `slider_deinit`,
`slider_virtual_set`, `joystick_dir`, `joystick_dir_name`, `joystick_active`,
`joystick_centered`, `joystick_reset`, `joystick_init`, `joystick_deinit`,
`joystick_simulate`, `button_pressed`, `button_none_pressed`,
`button_simulate`, `button_deinit`. Exactly what a transport layer should
expose.

**Variables** — 12 of 13 transport-owned variables are declared inside the
moving blocks (`joystick`, `joystick_dirmap`, `joystick_dir_names`,
`joystick_sector_nibbles`, `joystick_analog`, `joystick_binding`,
`joystick_maps`, `buttons`, `button_bindings`, `button_nchan_declared`,
`input_transports`, `slider_swipe_angle`).

The one straggler is `slider_active`, declared at ~1012 beside `em_active` and
`touch_active`. It does NOT need to move: same namespace, so `variable
slider_active` inside a moved proc still finds it. Moving it is tidier;
leaving it is correct.

**No circularity.** `ess-2.0.tm` would `package require ess_transports` at
load; the transports' LOAD-TIME code touches only `settings::declare` and a
catch-guarded `settings::get`. `do_update` is called at runtime, long after
both files are loaded.

## Risks, in order

1. **The `input_transport` registrations execute at load** (~5375, inside the
   buttons block) and must travel with their resolvers. A split that leaves a
   registration behind gives a resolver name pointing at a proc that no longer
   exists — and it fails only when someone binds that transport.
2. `package require settings` must appear in the new module: the three
   declarations (`joystick transport`, `joystick threshold_frac`, `button N`)
   go with their procs.
3. `package require ess_dial` ordering — `ess-2.0.tm` requires both; make sure
   `ess_transports` does not require `ess` back.

## Procedure

Its own session, nothing else in flight. **Strictly mechanical** — no
behaviour changes smuggled in, because a green suite proves much less if the
diff also changes something.

1. Run all eleven suites, record green.
2. Cut the four blocks into `lib/ess_transports-1.0.tm`, `package provide
   ess_transports 1.0`, `package require settings`, wrapped in
   `namespace eval ess { ... }`.
3. `package require ess_transports` in `ess-2.0.tm`, beside `ess_dial`.
4. Run all eleven suites again. They must be green with NO harness edits — a
   harness that needs changing means the cut moved something it should not
   have.
5. Cold-reload on a rig and check `ess/inputs/*` still resolve.

## One test worth adding first

Assert that every registered `input_transport` resolver names a proc that
actually exists. That is precisely the failure a botched split produces, and
the current suite catches it only indirectly:

    foreach kind {button joystick} {
        foreach name [::ess::input_transports $kind] { ... resolver exists ... }
    }

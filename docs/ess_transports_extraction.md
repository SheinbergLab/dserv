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

Not all four are lift-out blocks. joystick and buttons are each a complete
`namespace eval ess { }` block, banner to brace. Stick shaping and slider are
the TAIL of the em_windows namespace block (opens ~5694, closes ~6199), and
em_windows stays: cut the procs, LEAVE the closing brace on the em_windows
side, and wrap the cut fresh in the new file. Cutting the slider range
literally by the numbers above takes that shared brace with it and leaves
em_windows unclosed — a loud syntax error at load, but no reason to meet it.

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

**Inbound** — the protocol-facing API, called from what stays:
`slider_init`, `slider_x/y`, `slider_pos`, `slider_deinit`,
`slider_virtual_set`, `joystick_init`, `joystick_deinit`, `joystick_dir`,
`joystick_dir_name`, `joystick_centered`, `joystick_reset`,
`joystick_response`, `joystick_response_time`, `joystick_simulate`,
`button_init`, `button_deinit`, `button_pressed`, `button_active`,
`button_any_pressed`, `button_none_pressed`, `button_simulate`. Exactly what
a transport layer should expose. (An earlier count here listed
`slider_process` and `joystick_active` — those appear in comments only.)
Many of these are also NAMES in the `ess_functions` list (~3167) the script
analyzer consults; it stays behind, it is strings rather than calls, and it
is where a future rename goes stale silently.

**Variables** — the transport-owned variables are declared inside the moving
blocks and travel with them (`joystick`, `joystick_dirmap`,
`joystick_dir_names`, `joystick_sector_nibbles`, `joystick_analog`,
`joystick_binding`, `joystick_maps`, `buttons`, `button_bindings`,
`button_nchan_declared`, `button_group_chans`, `button_group_maps`,
`button_group_warned`, `input_transports`, `io_class`, `slider_mode`,
`slider_swipe_angle`, `slider_swipe_mag`, `slider_swipe_time`,
`slider_swipe_engaged`).

The stragglers are `slider_active` (~1012, beside `em_active` and
`touch_active`) and the `slider_x`/`slider_y` VARIABLES (~1017) — their
same-named accessor procs move, the variables they read do not. None of the
three needs to move: same namespace, so `variable slider_active` inside a
moved proc still finds it. Moving them is tidier; leaving them is correct.

**No circularity.** `ess-2.0.tm` would `package require ess_transports` at
load. The transports' LOAD-TIME code is: the variable declarations, five
`settings::declare` sites, and two full bind attempts —
`catch { joystick_bind_from_settings }` (~4145) and
`catch { button_bind_from_settings }` (~4909). Traced: those reach a
catch-guarded `settings::get`, their own namespace variables, and
catch-guarded status publishes to dserv builtins. Resolution is deferred by
design, so nothing from the staying file is needed at load. `do_update`,
`ess_warning` and `respwin_active` are called at runtime, long after both
files are loaded.

## Risks, in order

1. **The `input_transport` registrations execute at load** (~5375, inside the
   buttons block) and must travel with their resolvers. A split that leaves a
   registration behind gives a resolver name pointing at a proc that no longer
   exists — and it fails only when someone binds that transport.
2. `package require settings` must appear in the new module: five
   declaration sites (`joystick transport`, `threshold_frac`, `box_device`,
   `box_group`, and the `button 0..N` loop) go with their procs.
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
4. Run all suites again. Two of them extract procs from `lib/ess-2.0.tm` by
   hardcoded path, and only MOVED procs, so a CORRECT cut fails them loudly
   ("could not extract proc ...") until their open line points at
   `lib/ess_transports-1.0.tm`: `test_ess_binds` (`joystick_bind`,
   `button_bind`) and `test_ess_resolve` (the registry and resolvers, twelve
   procs). That one-line repoint each is PART of the move. Everything else
   must be green with no other harness edits — any other harness change
   means the cut moved something it should not have. (`test_ess_transports`
   scans all of `lib/ess*.tm` and needs nothing.)
5. Cold-reload on a rig and check `ess/inputs/*` still resolve.

## The test that guards the cut

`tests/test_ess_transports.tcl` (`ctest -R ess_transports`), added
2026-08-21. It extracts the real registry procs, executes the real
registration lines, and asserts (a) the six known transports are still
registered — a cut that drops a registration line fails here — and (b) every
registered resolver names a proc that exists — a cut that strands a resolver
fails here. It scans ALL of `lib/ess*.tm`, so the move itself needs no edit
to it. Both failure modes and the completed move were rehearsed against
doctored copies before the test was trusted.

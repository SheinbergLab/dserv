# Consolidating local GPIO behind one routing layer

Status: **proposal, not implemented.** Written 2026-08-05.

Goal: get every `gpioLine*` call into one place, so local Pi GPIO can be
*routed* to an extio box or *retired* per-rig without touching framework code
or hand-editing `local/post-pins.tcl`.

## Where things actually stand

Most of the routing already exists. Taking inventory before designing anything:

| concern | routed today? | mechanism |
|---|---|---|
| buttons | **yes** | `button_init chan pin` / `... box {dev pin}` / `... box {dev grp label}` / `... joystick N`, with rig-level `button_bind` overriding whatever the protocol asks for (`lib/ess-2.0.tm` `button_init`, `button_bind`) |
| joystick | **yes** | `joystick_init` + `joystick_bind`; legacy GPIO (`::joystick_init` in `config/essconf.tcl`) vs extio chord GROUP, one protocol-facing API (`joystick_dir`, `joystick_reset`, …) |
| juicer | **n/a** | not ESS GPIO. ESS calls `send juicer "reward $ml"`; `juicerSetPin` configures the juicer module's own pin |
| **obs sync out** | **no** | the gap — see below |

So this is not a green-field abstraction. It is one missing route plus a
custody change for the request calls.

## The two real problems

### 1. The obs sync output is dual-driven

`rpioPinOn/Off $obs_pin` fires **unconditionally** in `lib/ess-2.0.tm` — at
lines 2029, 2064, 2113 (onset paths) and 2308 (offset) — *alongside* the extio
scheduled path (`$io_class/$box/cmd/do/$pin/at_abs`, set by
`obs_schedule_bind`).

Consequences:

- A rig whose obs line is owned by a box **still drives local pin 26 as well**.
  Two lines assert per observation; only one is the timing authority.
- Because the local pin is always driven, `post-pins.tcl` must still request it
  (`gpioLineRequestOutput 26` + `::ess::set_obs_pin 26`) even on a box-led rig.
  That request is exactly the call that throws.
- There is no way to express "this rig has no local obs line."

### 2. Nothing owns the request calls

The actual `gpioLineRequest*` calls are scattered across three custody domains:

- `config/essconf.tcl` — `gpioInputInit` / `gpioOutputInit` (both already
  `catch`-wrapped), and three raw `gpioLineRequestInput` calls inside
  `joystick_init` (joystick4 interrupt, `joystick/lines`, `joystick/button_line`)
- `lib/ess-2.0.tm` — `gpioLineRequestInput` in `button_init`'s local-pin branch
- `local/post-pins.tcl` — **per-rig, hand-written, unguarded**: the button
  lines, the obs output, `juicerSetPin`

The third is the damaging one. `post-pins.tcl` is sourced bare from
`essconf.tcl`, so a line that is missing, already claimed, or mis-numbered
throws straight out of essconf and leaves `ess` half-initialized — no system
loaded, `::ess::init` refusing to run. Before the `dsconf.tcl` guard landed
(commit 75882b4c) that also truncated the whole boot.

## Proposed shape

One framework-owned module — `::ess::io` — that is the **only** caller of
`gpioLineRequest*` / `gpioLineSetValue` / `gpioInputInit` / `gpioOutputInit`,
driven by a declarative routing table. Follows the `button_bind` precedent
exactly: the rig declares, the framework resolves.

`local/post-pins.tcl` becomes declarative, with no `gpio*` calls in it:

```tcl
# obs sync line: pick ONE owner
io_route obs    box   {auto auto}    ;# extio leader announces its own pin
#io_route obs   local 26
#io_route obs   none                 ;# rig has no obs line

# discrete inputs (these already work via button_bind; io_route is the
# single spelling so a rig declares all of its I/O in one idiom)
io_route button 0 local 24
io_route button 1 box {* response left}

io_route juice  local 27             ;# -> juicerSetPin under the hood
```

### Rules

1. **`::ess::io` is the sole caller.** `button_init`'s local branch and
   `joystick_init`'s three requests move behind `io_request_input`; the obs
   pin moves behind `io_set obs 0|1`. Nothing else in the tree calls
   `gpioLine*`.
2. **Every request is caught.** A failure publishes `ess/io_error` (dict:
   route, target, message) and marks that route dead; it never propagates out
   of `post-pins.tcl`. A rig with one bad line still boots, still loads a
   system, and *says* which line is bad. This is the single biggest robustness
   win and it is worth doing even if nothing else here is.
3. **`obs` becomes single-path.** The four `rpioPinOn/Off $obs_pin` sites
   become `::ess::io_set obs 1|0`, which dispatches on the route:
   `local` → `gpioLineSetValue`; `box` → the existing scheduled
   `cmd/do/<pin>/at_abs` path (already implemented in `obs_schedule_bind`);
   `none` → no-op. Dual-drive stops.
4. **Absent target is a declared state, not an error.** If a route names a box
   that is not present, the route is *pending*, not failed — the extio binding
   layer is already hot-swap-transparent for glob devices, and the same
   semantics should apply here. `ess/io_status` reports each route as
   `ok | pending | failed | none`.
5. **Retirement is a one-line edit.** `io_route obs none` on a rig with no
   local GPIO. No framework change, no protocol change.

## Migration

Backwards compatibility matters here — every deployed rig has a hand-written
`post-pins.tcl`, and they are not in git.

- Keep `gpioLineRequestInput/Output`, `set_obs_pin`, `rpioPinOn/Off` working as
  today, as thin shims over `::ess::io` that log a deprecation to
  `ess/io_deprecated`. Existing `post-pins.tcl` files keep working untouched
  and *self-report* that they need migrating.
- Ship a converted `local/post-pins.tcl.EXAMPLE` showing the `io_route` form
  alongside the old form.
- Migrate rigs opportunistically; drop the shims only once
  `ess/io_deprecated` is quiet fleet-wide.

### `board_type` must stay a global

Step 1 moves chip custody into `::ess::io` — `detect_board_type`
(`config/essconf.tcl:171`), `gpio_chip_map` (233-251) and the
`gpioOutputInit`/`gpioInputInit` calls (255-256). Two globals those lines
currently leave behind in the ess interp, `board_type` and `gpiochip`, are
**part of the per-rig contract whether or not that was intended**:

    # local/post-pins.tcl.EXAMPLE:5 -- and every deployed post-pins.tcl
    switch $board_type {
        "rpi4" - "rpi5" - "rpi" - "beagley-ai" { ... }

`post-pins.tcl` is sourced *after* those lines, reads `$board_type` at its very
first statement, and picks its pin numbers from it. Encapsulating the globals
into the new namespace therefore breaks every rig on line 5 of its own pins
file — with a `can't read "board_type": no such variable` thrown out of
essconf.tcl, i.e. exactly the half-initialized-ess failure this work exists to
remove, inflicted fleet-wide instead of intermittently.

So: `::ess::io` may *own* detection, but it must keep publishing `board_type`
and `gpiochip` as ess-interp globals for as long as the deprecation shims live.
Retire them on the same schedule as the shims, once `io_route` has made the
`switch $board_type` blocks unnecessary (per-board pin choices become per-rig
route declarations, which is the point).

Worth a regression test rather than a read-through: source a representative
`post-pins.tcl` against the new module and assert it still resolves its pins.
The same goes for the `ess_validation-1.0.tm` stub set
(`lib/ess_validation-1.0.tm:155` currently stubs only `gpioLineSetValue`) —
if the io layer becomes the caller and the harness does not stub or `none`-route
it, the headless sim breaks silently.

## Order of work

1. `::ess::io` with the routing table, `io_route`, and caught requests
   publishing `ess/io_error` / `ess/io_status`. Shims for the old verbs.
   *(This alone fixes the half-initialized-ess failure mode.)*
2. Move `button_init`'s local branch and `joystick_init`'s three requests
   behind it. No behavior change.
3. Route `obs`, collapsing the four `rpioPinOn/Off` sites. **Behavior change**
   — a box-led rig stops driving the local pin. Wants a scope check on a rig
   with both wired before it ships.
4. Convert `post-pins.tcl.EXAMPLE` and the docs; migrate rigs.

Steps 1–2 are mechanical and independently shippable. Step 3 is the one that
needs hardware verification.

### Files affected by steps 1-2

Step 1 — new `lib/ess_io-1.0.tm`, plus:

| file | change |
|---|---|
| `config/essconf.tcl` | `package require ess_io`; `rpioPinOn`/`rpioPinOff` (70-71) become shims; chip custody moves in (171, 233-251, 255-256) |
| `lib/ess-2.0.tm` | `set_obs_pin` (1888) becomes a shim; `variable obs_pin` (987) stays as its backing store |
| `lib/ess_validation-1.0.tm` | 155 — stub set (see above) |
| `local/post-pins.tcl.EXAMPLE` | `io_route` form alongside the old |
| `local/README` | "writing these files" section |

Step 2 — 4 call sites in 2 files:

| site | what |
|---|---|
| `lib/ess-2.0.tm:4627` | the one `gpioLineRequestInput` in `button_init`'s local-pin branch |
| `config/essconf.tcl:314` | `joystick_init` — joystick4 interrupt (`RISING`) |
| `config/essconf.tcl:323` | `joystick_init` — `joystick/lines` |
| `config/essconf.tcl:334` | `joystick_init` — `joystick/button_line` |

Deliberately untouched: `modules/gpio_input/`, `modules/gpio_output/`,
`modules/juicer/` (C unchanged — this is a Tcl-layer consolidation);
`extio-zephyr/host/*.sh` (bench tools, keep the raw verbs);
`lib/examples/em_window_sampling.tcl`.

## Open questions

- Should `io_route` live in `post-pins.tcl` (sourced inside essconf, as now) or
  move to a `pre-*` file so routing is known before subprocesses start? Pins
  are an `ess` concern, so `post-` is probably right, but the juicer pin argues
  the other way.
- Does anything outside ESS drive local GPIO on a live rig? The
  `extio-zephyr/host/*.sh` harnesses do (`dservctl ess "gpioLineRequestOutput
  ..."`), but those are bench tools and can keep using the raw verbs.
- Is there a rig that genuinely needs *both* a local obs line and a box-led
  one (e.g. driving a second recording system)? If so `obs` needs a list of
  targets rather than one, and rule 3 changes shape.

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

# extio ↔ ESS integration: friction log → `::ess::io_*` API design

The `extio_test` system (systems/ess/extio_test) deliberately drives the box
with **bare** `dservSet` / `dpointSetScript` calls against the wire contract.
This file records every point of friction met while writing and running it.
The Stage-7 `::ess::io_*` layer is designed FROM this list, then proven by
migrating extio_test onto it and re-running the frozen certification — the
verdicts and timing distributions must match the bare-command runs.

Design principles (agreed up front):
- wire-format knowledge stays in `lib/extio-1.0.tm` (already the decoder home)
- lifecycle-aware wrappers join the existing hardware families in
  `lib/ess-2.0.tm` (`touch_*` / `em_*` / `button_*` / `joystick_*`;
  `io_class` and `box_schedule_pulse/timer` are already there)
- every helper is sugar over a single `dservSet`/`dpointSetScript` —
  no polling, no extra interp hops, hot paths byte-identical

## Friction log

1. **Box resolution is hand-rolled.** Every protocol re-invents
   `auto → dservGet extio/primary → warn if absent`.
   → `io_init ?box?`: resolve (name | glob | auto), liveness-check
   (`state/watchdog` advancing), remember for the session.

2. **No absolute-time wrapper.** `::ess::box_schedule_pulse` covers `at`,
   but `at_abs` means hand-building `extio/<box>/cmd/do/<n>/at_abs` with
   `[now]+lead` math, and the reply (`state/sched/abs_err` =
   armed|late|unsynced, `abs_lead_us`) must be fished out manually — and it
   is one datapoint per box, so two in-flight `at_abs` on different pins
   alias it.
   → `io_pulse_at_abs <pin> <T_us>` returning/surfacing the reply status.

3. **`at_abs` sets HIGH only** — the clear is the caller's cleanup burden
   (extio_test drops the line in `trial_cleanup`). Wrapper should pair
   set+clear; longer term firmware could add an `at_abs` pulse variant.

4. **No blessed home for protocol-level hardware state.** The edge
   callbacks (dpointSetScript) and the state-system methods share counters
   only via absolute namespace paths
   (`::ess::extio_test::loopback::lb_*` everywhere).
   → `io_line_bind <pin> <script>` (timestamped edge subscription) plus an
   optional built-in latch/counter (`io_line_count`, `io_line_last_us`,
   `io_line_reset`) modeled on the joystick first-crossing latch.

5. **Config is push-and-pray.** Box config is RAM-resident; dserv retains
   stale `config/*` datapoints after a box reboot, and there is no
   ess-level "push + `cmd/announce` + readback-verify".
   → `io_config_verify {key val ...}`.

6. **Analog group setup is six raw config keys** (`channels/label/mode/
   decimate/batch/rate` + `enable`), with the 200 blocks/s silent-throttle
   trap left to the caller's arithmetic.
   → `io_ain_config <label> <channels> <rate> ?opts?` that REFUSES a config
   whose blocks/s exceeds the budget unless told otherwise;
   `io_ain_start/stop`; `io_ain_bind <label> <script>` (decode via
   `::extio::ain_decode`).

7. **The "log the whole conversation" match set is a hand-maintained glob
   list** in the file_open callback (and `config/*` was missed on the first
   pass — exactly the class of bug a helper kills).
   → `io_log_matches $filename` adding the standard set: `cmd/*`,
   `config/*`, `state/di|do|ain|sched|sync|dbg/*`, `state/watchdog`,
   `state/cmds_rx`, `ess/in_obs`.

8. **Rig facts don't reach the datafile on their own.** extio_test invents
   a side-channel datapoint (`extio_test/session_config`) so the extract
   knows box + pin map + analog label.
   → the io layer should publish its binding config on a standard
   datapoint (e.g. `ess/io_config`) that `io_log_matches` includes and
   extract helpers parse.

9. **µs analysis requires raw-log mining.** Event times in the obs dg are
   ms (dslog stores `evtime/1000`); logged datapoints become `<ds>` columns
   with values but NO timestamps; `dslog::read` timestamps are float32
   seconds (µs precision gone after ~2 min). Only `dslog::open/next`
   preserves 64-bit µs.
   → ship extraction helpers next to the decoder (extio-1.0.tm or a small
   `extio_extract` module): obs-windowed scan, per-pin edge series, block
   iterator — extio_test_extract.tcl's `ex::` procs are the prototype.

10. **No DAC command in the wire contract** — RESOLVED in v0.4.0+18:
    `cmd/dac/<ch>` (immediate 12-bit set, ch 0, echo `state/dac/<ch>`,
    capability flag `dac_en` in the manifest). The io layer should still
    wrap it (`io_dac <counts>`) and know the ~code-700 floor.

11. **Scheduler capacity is a silent contract** (8 `at` slots per box,
    one in-flight `at_abs` per pin). Loaders must "just know" (≤ 6 rule).
    → wrappers should error loudly when a schedule would exceed capacity.
    OBSERVED FOR REAL 2026-08-02: an np=10 analog train had 2 pulses per
    trial refused with a console-only "sched: table full" — nothing on
    the wire — and the loss was invisible until the +21 transport fix
    unmasked it (before that, the burst cliff ate the excess commands
    first). extio_test's loader now hard-errors past 6; the firmware
    follow-on is an `at` refusal reply datapoint (`state/sched/err`,
    mirroring how `at_abs` answers armed|late|unsynced) so hosts can see
    a refused schedule without a console cable.

12. **In-obs STRING event params surface as CHAR lists** in the obs dg
    (`STIM_DATA` summaries came back as 71-element char rows; the pre-obs
    metadata path decodes to real strings, in-obs does not), and `df::File`
    has no string-param accessor — every consumer re-invents
    `binary format c* + encoding convertfrom`.
    → a `event_string_sparse` helper in df-2.0.tm (or the extio extract
    helpers of item 9).

13. **`dservLoggerAddMatch` registration races a one-shot `dservSet` issued
    immediately after it** — the set is lost from the file. Continuous
    publishers don't care; single-shot session records do (extio_test's
    session_config vanished until it was republished at session START).
    → any io-layer "publish rig facts for the datafile" datapoint must be
    (re)published on session start, not only at file open; document the
    idiom, or give the logger a synchronous match-registration path.

14. **The analog batch/window contract is easy to violate silently**: a
    trial whose streaming window closes before one full batch accumulates
    yields ZERO blocks (partial batches are held, on the virtual box and
    real firmware alike), and a sub-second overload never trips the
    per-whole-second 200 blocks/s throttle. Both bit extio_test's first
    drafts. → `io_ain_config` should warn when `batch/rate` is large
    relative to the intended window, and the docs should state that
    overload detection needs >1 s of sustained overrun.

## The scheduled-obs design (David, 2026-08-02): BEGINOBS as a future instant

Today `::ess::begin_obs` asserts the obs line NOW and stamps the event
[now] -- so "when did the obs begin" has three answers smeared over ~a
millisecond with a host-tail: the event's host stamp, the extio box's
frame-arrival mirror, and the physical line every other instrument syncs
on. The campaign's numbers now justify the fix the platform was built
for: **schedule the obs onset in the future on the disciplined clock,
verify it armed, and record BEGINOBS at the scheduled time.**

    T = now + lead (lead >> the measured ~1 ms delivery tail; 20 ms default)
    cmd/do/<obs_pin>/at_abs T          ; box asserts the line at T +/-<=120 us (gated)
    verify state/sched/abs_err == armed   ; the TRUST condition, plus PTP-held
    evt_put BEGINOBS ... T             ; the event carries the true epoch
    fallback: not armed / not PTP-held -> current behavior (assert now,
    stamp now), and RECORD which mode produced the epoch

Done fully, the box's own at-epoch should be the SAME T -- which wants a
firmware verb (+27 candidate): `cmd/obs/begin_at <T>` = schedule the
mirror line via the at_abs machinery AND set obs_begin_us = T(box), so
host event stream, box scheduler, and physical TTL agree on ONE instant.
(Host-only interim: at_abs the line + stamp the event at T; the box's
internal at-epoch stays the in_obs mapping -- fine internally, but the
full verb is the clean contract.) ENDOBS can take the same treatment
later if paradigms want symmetric offsets.

This generalizes to a doctrine the certification now backs with gates:
**any host-initiated marker that other systems observe should be
scheduled on the disciplined clock, verified armed, and recorded at its
scheduled time** -- observation of a software moment is always smeared;
a scheduled instant is exact.

## Candidate surface (to validate by use, not committed a priori)

```tcl
::ess::io_init ?box?                      ;# resolve + liveness + remember
::ess::io_out <pin> <0|1>
::ess::io_pulse <pin> <width_us>
::ess::io_pulse_at <pin> <offset_ms>      ;# = box_schedule_pulse
::ess::io_pulse_at_abs <pin> <T_us>       ;# surfaces armed|late|unsynced
::ess::io_line_bind <pin> <script>        ;# timestamped edge callback
::ess::io_line_reset/count/last_us <pin>  ;# built-in latch/counter
::ess::io_ain_config <label> <chans> <rate> ?-batch -decimate -mode?
::ess::io_ain_start / io_ain_stop
::ess::io_ain_bind <label> <script>
::ess::io_dac <counts> ?ch?               ;# fw >= +18; knows the ~700 floor
::ess::io_sync_status                     ;# source/offset/rate dict
::ess::io_config_verify {k v ...}         ;# push + announce + readback
::ess::io_log_matches <filename>          ;# the standard conversation set
::ess::io_obs_begin_at ?lead_ms?          ;# scheduled-obs onset (see above)
```

# Designing New Experiments: system, protocol, or variant?

A decision guide for turning a paradigm idea into ESS code — written for new
lab members and for LLM-assisted development, where generating a whole system
costs an afternoon and the real questions are *where the new idea should live*
and *how we know it works*.

The worked example threaded through this doc is `remap` (saccadic remapping,
`~/systems/ess/remap`), built as the first deliberate test of these
principles. The inventory appendix at the bottom is the "what already exists"
table you match a new idea against.

---

## 1. The contract the architecture already enforces

```
System   → the trial GRAMMAR: states, epochs, timers, transition gates,
           outcome taxonomy, event stamps
Protocol → the CONTENT and SENSING: method overrides (what is drawn, how a
           response is detected, what counts as correct), loaders, stim file
Variant  → the DATA: loader arguments → stimdg columns and parameter values
```

Two facts make this a contract rather than a convention:

1. **Protocols cannot add states.** Every `add_state` / `add_action` /
   `add_transition` call in the entire systems tree lives in a system file —
   14 systems out of 14, no exceptions (verified 2026-08). A protocol that
   needs an epoch the system lacks has no legal move; that pressure is the
   signal that you are looking at a new system.

2. **Systems sense through overridable predicate methods.** A state's
   transition never reads hardware directly; it asks `[my acquired_fixspot]`,
   `[my responded]`, `[my out_of_start_win]`. The system owns *when the
   question is asked and what happens to the answer*; the protocol owns *how
   the question is answered*. This is what lets one grammar serve touch,
   button, joystick, and eye protocols (see `match_to_sample`,
   `sequence`) — and what keeps a grammar testable headless.

Corollary, from the standing architecture principle: **state machines own
structure, timing, and scoring only; content lives in the loader and stim
file.** If a fact varies per trial, it is a stimdg column, not a state-machine
branch. An experimental *condition* is almost never a reason for a second
protocol or a `if {$condition}` fork in a transition — it is a column the
methods read.

---

## 2. The three-question test

Take the new idea and describe one trial aloud: the epochs in order, what
*gates* each transition (a timer? an acquisition? a release? a landing?), and
the outcome taxonomy (the distinct ways a trial can end). Then ask, in order:

**Q1 — Can the difference be expressed as trial data?**
Different geometry, timings drawn per trial, proportions of trial types,
stimulus sets, reward rule selected from existing choices → **variant**. This
covers more than people expect: a condition that feels structural ("on half
the trials the fixation point jumps") is a column if the state sequence can be
identical on every trial (see §6 — remap runs the jump epoch on *every*
trial; no-remap trials carry a zero-length jump).

**Q2 — Same epoch sequence and same gate types, but different content, rules,
or response modality?**
New stimulus family, new correctness rule, touch instead of buttons, a
different way of computing the same decision → **protocol**. You override
methods; you never touch the machine.

**Q3 — Different epochs, different gate types, or a different outcome
taxonomy?**
A wait-for-acquisition where there was a timer; an enforced-fixation epoch
where nothing was enforced; a response that is a saccade landing instead of a
touch; a new abort class that must be scored and requeued differently →
**system**. Do not thread it through an existing machine with mode flags.

The diagnostic for Q3 is mechanical: if your one-paragraph trial description
names an epoch or a gate that the candidate system does not have, it is a
system. A second, softer diagnostic: *would the guard you are about to add be
exercised by the system's existing protocols, or defaulted off everywhere
except yours?*

The tree already contains the calibration cases:

- `match_to_sample` absorbed **button gating** (letgo / early-abort) — a
  cross-cutting contingency threaded through existing states with
  `[my button_gating_active]` guards. Cost: 5 states, 4 params, 3 variables.
  It was the right call because the grammar (sample → delay → choices) was
  untouched and *every* mts protocol uses buttons. That is the honest upper
  bound on what "absorb it" costs when it is appropriate.
- `launch` **forked from planko** when the `prestim_lead` epoch didn't fit —
  its header says so. Grammar change → fork. That was the right call too, and
  it documents its parent, which is the convention when forking: say where
  the machine came from.

---

## 3. Do protocols still have a place? Yes — they are the scientific axis

With generation this cheap there is a real temptation to fork a whole
mono-protocol system for every idea. Resist it from both directions:

- **A system is the unit of *procedural* variation** — what the subject's
  body must do, epoch by epoch, and how failures are classified.
- **A protocol is the unit of *scientific* variation** — what is shown and
  what the rule is, inside a procedure that already works.

Protocols keep paying for themselves in ways a forked system does not:

1. **Shared debugging is inherited, not re-earned.** `prf`'s four protocols
   (motionpatch, gratings, opponent-motion, plankobits) override an identical
   8-method contract; every timing fix and event-ordering lesson in prf.tcl
   (e.g. the stamp-STIMTYPE-after-BEGINOBS fix) lands in all four at once.
   Same for `pursuit`'s three trajectory protocols.
2. **Analysis stays aligned.** Same grammar → same event sequence → one
   extractor and one analysis harness serve the family.
3. **Configs can mix them.** The config/queue system composes
   system+protocol+variant rows; families of protocols under one system make
   coherent session menus.

Signs the protocol layer is being used well: the system file documents a small
required-method contract; all its protocols override the same set; new
protocols are mostly loader + stim file. Signs an idea is outgrowing protocol
(fork time): you are adding states, or adding a `uses_*` method that only one
protocol will ever turn on, or redefining what an existing event means.

A **single-protocol system is not a smell by itself** (`launch/occlusion` is
one, justified by grammar); it is a smell only when its grammar duplicates a
neighbor's — then it should have been a protocol there.

---

## 4. What the LLM era actually changes

The historical argument for the ever-growing mega-system was: *a
well-debugged machine with many gates beats a special-purpose machine that is
broken.* That was correct — when the scarce resource was the debugging.
Every hand-written state machine carried weeks of latent
edge-case discovery, so concentrating trials-by-fire into one shared machine
was rational risk management.

What changed is not that models write Tcl. It is that **a special-purpose
system no longer has to be broken**, because verification moved from rig time
to desk time:

- `ess_test` runs the real loader → stimdg chain and the real stim file
  (against capturing stubs) headless, so trial-generation math, event
  ordering, and per-frame writes are assertable before hardware exists.
- The variant sweep (`test_variants.tcl`) dry-runs every variant's loader in
  the collection and has caught real arg-count and context bugs.
- The virtual-subject recipe drives a live dserv/ESS with injected responses,
  so full state-machine traversal — aborts included — can be exercised
  without a subject.

So the cost structure inverted. Generation is cheap; what is scarce now is
**verification effort** and **coherence of the collection**. The updated
rules:

1. **Never flag-ify grammar.** The mega-system's marginal flag multiplies its
   state space for every existing protocol, and the flag interactions are
   exactly the paths nobody's session exercises. Fork instead — a fork's
   states are all load-bearing, which is what makes it fully testable.
2. **Mint small systems freely, but make the tests part of the deliverable.**
   A new system is not "done" when it loads; it is done when its
   `tests/test_<system>.tcl` passes headless. When the code is
   LLM-generated, the tests are the review; ask for them in the same breath.
3. **Fight duplication in semantics, never in text.** Duplicated state-machine
   *text* between forks is an acceptable, visible cost (launch/planko live
   with it). Duplicated *meaning* is not:
   - **Event IDs are the ABI.** Reuse canonical types/subtypes
     (`ess-2.0.tm`); never renumber; add types rather than overloading
     (RESPWIN exists precisely so CUE doesn't get overloaded).
   - **Content generation goes in `lib/*.tm`** (blobgen, fractal, planko,
     traj…), never copy-pasted into loaders.
   - **Sensing goes in `::ess` services** (em/touch/button/joystick APIs),
     never re-implemented per system.
   - **Extraction conventions** stay shared so datafiles stay uniform.
4. **Promote on the third copy.** When the same epoch cluster has been
   hand-copied into a third or fourth system — fixon/fixhold(+fixjump) is now
   in emcalib, prf, pursuit, launch/planko, and remap — that is the signal to
   promote a state-fragment installer into ess-2.0.tm (the way button/touch
   became services), so the *next* system calls it instead of copying.
   Not before: premature shared fragments are mega-systems by another name.
5. **When forking, cite the parent** in the header (launch does), so a fix
   found in one machine can be checked against its siblings by grep.

---

## 5. The workflow for a new experiment

1. **Write the trial as one paragraph**: epochs in order, the gate on each
   transition, the outcome taxonomy. This paragraph is the design; everything
   else is transcription.
2. **Match it against the inventory** (appendix). Same grammar somewhere? →
   protocol there (Q2), or just a variant (Q1). Otherwise pick the nearest
   grammar as the copy-seed and fork (Q3).
3. **Choose the event vocabulary first** — canonical types/subtypes from
   ess-2.0.tm, stamped at the moments analysis will need. If a needed marker
   has no honest type, add one to the table (new ID, never a reuse).
4. **Design the stimdg schema next.** Every condition is a column. Include
   the columns *scoring and analysis* will want, not just what the stim needs
   (e.g. remap stores the retinotopic-error location per trial so that error
   class is scoreable online and extractable offline).
5. **Generate the five files** (system, protocol, loaders, variants, stim) —
   with the nearest relatives open as context if an LLM is doing the typing.
   Loaders vectorized (`dl_*` column math, no row loops).
6. **Write and run the headless tests in the same sitting**: tier-1 stimdg
   assertions per variant, tier-2 stim-file captures for each display proc,
   plus the invariants a shape check misses (per-row uniqueness, clearances,
   condition equalities).
7. **Virtual-subject / headless dserv run** for full state-machine traversal
   (all abort paths at least once), then the rig for what only eyes can
   judge: appearance.

---

## 6. Worked example: `remap`

**The paragraph (step 1):** Subject acquires a central fixation spot
(gate: eye enters window; timeout aborts). Holds (gate: timer; break aborts).
An array of 1–4 shapes appears at peripheral locations; fixation is enforced
(timer + break-abort). Array off. The fixation spot is re-drawn at a per-trial
location — displaced on remap trials, identical on no-remap trials — and the
subject's eye must be in the (possibly moved) fixation window within a
reacquire timeout (gate: acquisition). Enforced delay (timer + break-abort).
The fixation spot is replaced by one of the array shapes (the cue): this is
the go signal; a response window opens. The subject saccades to the location
where that shape had been **in world coordinates** (gate: landing in one of
the armed location windows, held through a settle time; timeout → no
response). Outcomes: correct (target's former location), incorrect (another
item's former location, or — on remap trials — the retinotopically-equivalent
location, scored as its own slot), abort-eye (no acquisition / break /
failed reacquire), abort-noresponse.

**The decision trace (steps 2–3):** Q1 fails (no existing machine can run
this as data). Q2 fails (no system has this epoch sequence — the closest
touch machine, `match_to_sample`, has the sample→delay→probe skeleton but
timer gates, no fixation states, no jump; the eye machines have fixation and
even a jump epoch but no memory response). Q3 → new system. Organs cribbed
rather than invented:

| Piece | Source |
|---|---|
| fixon/fixhold + acquire/timeout/break grammar | `emcalib`, `pursuit` |
| fixjump epoch + `acquired_fixjump` + `FIXSPOT SET` | `emcalib` (its jump is visually guided — so is remap's; only the *response* is memory-guided) |
| stamp STIMTYPE after BEGINOBS lands | `emcalib` fixon comment (2026-08-06 lesson) |
| sample→delay→cue skeleton | `match_to_sample` |
| N-window response scoring | `shapematch responded`, transposed `touch_in_win` → `em_eye_in_region` |
| shapes + rendering | `blobgen` lib + `shapematch` shader/impro idiom |
| events | FIXSPOT ON/SET/OFF, FIXATE IN/REFIXATE, SAMPLE, CUE, RESPWIN, RESP, FEEDBACK, EMPARAMS CIRC, ENDTRIAL, ABORT EYE/NORESPONSE — all pre-existing |

**Condition-as-data (step 4):** every trial traverses the *same* states,
including `fixjump`; a no-remap trial's `fix2_x/fix2_y` equal `fix_x/fix_y`,
so `acquired_fixjump` is already true and the epoch passes through in one
update. The delay timer starts at reacquisition in both conditions, so the
memory interval is equated by construction. The loader also emits, per trial:
each item's former location (the response windows), the target slot, and the
retinotopic-equivalent location (`target + jump vector`) armed as an extra
scored window on remap trials — the theoretically diagnostic error is a
first-class response, not a post-hoc reconstruction.

**What the tests assert (step 6):** `remap/tests/test_remap.tcl` — trial
counts and column presence per variant; remap fraction; `fix2 == fix` exactly
on no-remap rows and `|fix2−fix| == jump_dist` on remap rows;
`retino == target + (fix2−fix)` row-wise; per-row shape-id uniqueness;
clearance of every response window from the jumped fixation point; and
tier-2: the stim's fixspot actually translates to `fix2` on a remap trial and
does not move on a no-remap trial, visibility toggles per display proc.

**What live testing added (step 7, and why it exists):** the first
virtual-eye session caught two bugs headless tests structurally cannot see,
and each became a permanent check:

- *Scalar-ness*: `dl_choose` over a `dl_pack`ed per-row index returns nested
  1-element rows, and every arithmetic assertion digests the nested form
  happily — but `dl_get` hands consumers a `%list%` handle, so
  `em_fixwin_set` got garbage. Fix: `dl_collapse`; new tier-1 assert:
  `string is double` on every scalar column.
- *Every state needs an action.* `do_action` invokes `my <state>_a`
  unconditionally, so a transition-only state (`add_transition` without
  `add_action`) throws `TCL LOOKUP METHOD <state>_a` mid-update. The throw
  is swallowed at the dpoint-script layer (latched in the `error/ess`
  datapoint), and the machine parks in that state until the next unrelated
  wake — on the rig this presented as "the response only registers once the
  eye moves again." Fix: explicit empty actions; new static check: every
  `add_transition` state appears in `add_action`/`add_state`.

Debugging recipe that found them, worth keeping: check `error/ess` first
(state-machine throws latch there silently); when event ordering is in
question, wrap `::ess::do_update` with a tracer that logs
`timestamp, state-before → state-after, em mask, timer flags, wake source`
to a file — one trial's trace localizes a wake-ordering bug in seconds. Two
environment facts for virtual-eye work: every timer id (0..7) wakes the
machine via its `timer/<id>` datapoint (named timers are safe as a state's
sole wake source), and essgui's virtual eye keeps republishing its parked
position (~5 Hz), so a second injected eye source fights it — filter or
park one.

---

## Appendix: system inventory (2026-08)

Match new ideas against this table; regenerate it when it drifts.

| System | Trial grammar (one clause) | Modality |
|---|---|---|
| blinky | blink loop; boots a bare rig end-to-end | none |
| extio_test | command pulses → collect → compare (hardware self-test) | none |
| search | array on → touch the target | touch |
| video | offer → play-or-skip | touch |
| detection | stimulus stream → respond in window at target | touch |
| match_to_sample | sample → delay → choices → select | touch + buttons |
| hapticvis | sample (seen/felt) → choices → select | touch + joystick |
| sequence | N timed elements → probe at cue time | touch + buttons + swipe |
| launch | fixate(opt) → occluded trajectory → 2AFC → feedback | buttons + touch (+eye) |
| planko | fixate(opt) → physics fall → 2AFC → feedback | buttons + touch (+eye) |
| joystick | targets ring → settled deflection / roam-and-touch | joystick |
| emcalib | fixate → jump → refixate → sample eye | eye |
| prf | fixate → stim stream under fixation → jump probe | eye |
| pursuit | fixate → target moves → pursue (± probe) | eye + buttons |
| remap | fixate → array → (jump) → delay → cue → memory saccade | eye |

Shared content libraries live in `~/systems/ess/lib` (blobgen, blob, fractal,
haptic, planko 1–3, ricochet, tilemap, thread_pool, computebroker); shared
sensing lives in `::ess` (em, touch, button, joystick, sound, juicer);
event types/subtypes live in one table in `ess-2.0.tm` and their numeric IDs
are the ABI.

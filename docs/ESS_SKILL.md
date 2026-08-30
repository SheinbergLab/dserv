---
name: ess-development
description: |
  Reference for developing and modifying ESS (Experiment State System) experiments
  built on the dserv/dlsh/stim2 platform. Use this skill whenever working with:
  ESS state system scripts (.tcl), match_to_sample or planko experiment frameworks,
  dserv datapoints and pub/sub patterns, experiment protocols/loaders/variants,
  stimdg (stimulus data group) columns, vizconf visualization configs,
  dlg_* graphics commands, button/touch/eye-movement APIs, ESS Workbench web GUI,
  or any Tcl code that uses `package require ess`. Also use when the user mentions
  experiment control, trial structure, behavioral paradigms, or stim files.
---

# ESS Development Reference

## Overview

ESS (Experiment State System) is a Tcl-based framework for running behavioral
neuroscience experiments. It runs inside `dserv`, a distributed data acquisition
server. Experiments are defined as hierarchical state machines with this structure:

```
System → Protocol → Variant (via Loader)
```

Each level adds parameters, methods, and behavior. Protocols override system
defaults; variants configure trial sets via loaders.

---

## File Naming and Directory Layout

For a system `foo` with protocol `bar`, files live under the systems tree:

```
<project>/foo/foo.tcl                    # System definition
<project>/foo/bar/bar.tcl                # Protocol
<project>/foo/bar/bar_loaders.tcl        # Loader methods
<project>/foo/bar/bar_variants.tcl       # Variant definitions
<project>/foo/bar/bar_stim.tcl           # Stimulus program (runs on stim2)
```

The `ess_paths` module constructs these paths from `(system, protocol, type)`.
Script types: `system`, `protocol`, `loaders`, `variants`, `stim`,
`sys_extract`, `sys_analyze`, `proto_extract`.

### Overlay System

User-specific edits go in overlays: `<systems>/overlays/<username>/...`
mirroring the same directory structure. `ess::paths::resolve` checks
overlay first, falls back to base. Saves go to overlay when active.
Promote copies overlay → base and removes the overlay file.

---

## System Definition Pattern

A system defines the state machine, parameters, variables, states, and
default methods. Protocols override methods to implement specific behavior.

```tcl
package require ess

namespace eval my_system {
    proc create {} {
        set sys [::ess::create_system [namespace tail [namespace current]]]

        # Parameters (name, default, category, type)
        $sys add_param response_timeout 10000 time int
        $sys add_param juice_ml 0.6 variable float

        # Variables (name, default)
        $sys add_variable obs_count 0
        $sys add_variable correct -1

        # States
        $sys set_start start
        $sys add_state start {} { return next_state }

        $sys add_action some_state { ... }      ;# runs on entry
        $sys add_transition some_state { ... }   ;# runs each update cycle

        $sys set_end {}

        # Default methods (protocols override these)
        $sys add_method responded {} { return -1 }
        $sys add_method reward {} {}

        # Callbacks
        $sys set_init_callback { ::ess::init }
        $sys set_reset_callback { ... }
        $sys set_start_callback { ... }
        $sys set_quit_callback { ::ess::end_obs QUIT }

        return $sys
    }
}
```

### State Machine Conventions

- **Actions** run once on state entry
- **Transitions** run on each update cycle; return a state name to transition
- Return nothing from a transition to stay in current state (yield to event loop)
- `timerTick $duration` starts the default timer; `timerExpired` checks it
- Named timers: `timerTick $timer_id $duration` / `timerExpired $timer_id`
- `[now]` returns current timestamp in microseconds
- `my method_name` calls an overridable method

### Standard System Methods

These are defined with defaults in the base system and overridden by protocols:

| Method | Returns | Purpose |
|--------|---------|---------|
| `n_obs` | int | Total number of trials |
| `nexttrial` | void | Set up next trial (read stimdg, configure touch/stim) |
| `finished` | bool | True when all trials complete |
| `responded` | int | -1 = no response, 0+ = response identifier |
| `response_correct` | bool | Whether current response is correct |
| `reward` | void | Deliver reward |
| `noreward` | void | Handle incorrect/no-reward |
| `endobs` | void | End-of-observation bookkeeping |
| `presample` | void | Pre-sample period actions |
| `sample_on` / `sample_off` | void | Show/hide sample stimulus |
| `choices_on` / `choices_off` | void | Show/hide choice stimuli |
| `finale` | void | End-of-block actions |
| `button_gating_active` | bool | Whether button checks are active (default: 0) |

---

## Protocol Definition Pattern

Protocols live in `namespace eval system::protocol` and define `protocol_init`:

```tcl
namespace eval my_system::my_protocol {
    variable params_defaults { sample_time 2000 delay_time 0 }

    proc protocol_init { s } {
        $s set_protocol [namespace tail [namespace current]]

        # Protocol-specific params
        $s add_param rmt_host $::ess::rmt_host stim ipaddr
        $s add_param use_buttons 0 variable int

        # Protocol-specific variables
        $s add_variable reward_rule match

        # Init callback — set up hardware
        $s set_protocol_init_callback {
            ::ess::init
            my configure_stim $rmt_host
            ::ess::touch_init
            ::ess::juicer_init
            ::ess::sound_init
        }

        # Final init — runs after variant is loaded (params are available)
        $s set_final_init_callback {
            if { $use_buttons } {
                ::ess::button_init 0 $left_button
                ::ess::button_init 1 $right_button
            }
        }

        $s set_protocol_deinit_callback {
            ::ess::touch_deinit
            ::ess::button_deinit
            rmtClose
        }

        # Override methods
        $s add_method responded {} { ... }
        $s add_method reward {} { ... }

        return
    }
}
```

---

## Button API

The abstract button API in `::ess` manages GPIO pins and joystick-mapped buttons:

```tcl
# Initialization (in set_final_init_callback)
::ess::button_init 0 $left_pin          ;# GPIO pin → channel 0
::ess::button_init 1 $right_pin         ;# GPIO pin → channel 1
::ess::button_init 0 {} joystick 4      ;# joystick value → channel 0

# Queries (in transitions)
::ess::button_any_pressed               ;# 1 if any channel pressed
::ess::button_none_pressed              ;# 1 if no channels pressed
::ess::button_pressed $chan             ;# 1 if specific channel pressed
::ess::button_active                    ;# first active channel, or -1

# Cleanup (in deinit callback)
::ess::button_deinit
```

### Button Gating Pattern

When `button_gating_active` returns true, the system checks for unwanted
button presses during pre-stimulus periods and enforces a "let go" requirement
before trials begin:

- **letgo_wait** → **letgo_sound** → **letgo_released**: subject must release
  buttons before trial proceeds
- **letgo_abort**: held too long, trial aborted
- **button_abort**: premature press during pre-stim/sample periods

The protocol enables this by overriding:
```tcl
$s add_method button_gating_active {} {
    return [expr {$use_buttons || $use_joystick}]
}
```

### Unified Response Pattern

Check buttons first, then fall through to touch:
```tcl
$s add_method responded {} {
    # Check buttons (returns 1=left, 2=right, 0=none)
    set b [::ess::button_active]
    if { $b >= 0 } {
        set resp $b
        # Determine correctness based on screen position
        set chose_match [expr {
            ($resp == 0 && $targ_x < $dist_x) ||
            ($resp == 1 && $targ_x > $dist_x)
        }]
        set correct $chose_match
        return $resp
    }

    # Check touch regions
    if { [::ess::touch_in_win 0] } {
        set correct 1
        return 0
    } elseif { [::ess::touch_in_win 1] } {
        set correct 0
        return 1
    }
    return -1
}
```

---

## Stimulus Data Group (stimdg) Conventions

The `stimdg` is a column-oriented data structure (using `dl_*` commands from dlsh)
that defines all trial parameters. It flows from loader → protocol → stim file.

### Standard Columns

| Column | Type | Purpose |
|--------|------|---------|
| `stimtype` | int | Trial index (usually 0..n-1) |
| `remaining` | int | 1 = not yet shown, 0 = complete |
| `side` | int | 0 = left, 1 = right (for match position) |
| `match_x`, `match_y`, `match_r` | float | Match stimulus position/size |
| `nonmatch_x`, `nonmatch_y`, `nonmatch_r` | float | Nonmatch position/size |
| `sample_x`, `sample_y`, `sample_r` | float | Sample position/size |

### Adding Custom Columns

New per-trial parameters flow through the full chain:

1. **Variant** — add to `loader_options` with choices:
   ```tcl
   loader_options {
       reward_rule { match nonmatch }
       sample_decoration { none {slow {blink 1.0 0.5}} {medium {blink 2.0 0.5}} }
   }
   ```

2. **Loader** — add as parameter, write to stimdg:
   ```tcl
   $s add_loader setup_trials { n_rep targ_scale reward_rule sample_decoration } {
       ...
       dl_set $g:reward_rule [dl_repeat [dl_slist $reward_rule] $n_obs]
       dl_set $g:sample_decoration [dl_repeat [dl_slist $sample_decoration] $n_obs]
   }
   ```

3. **Protocol nexttrial** — read with backward-compatible fallback:
   ```tcl
   if { [dl_exists stimdg:reward_rule] } {
       set reward_rule [dl_get stimdg:reward_rule $stimtype]
   } else {
       set reward_rule match
   }
   ```

4. **Stim file** — read and interpret:
   ```tcl
   if { [dl_exists stimdg:sample_decoration] } {
       set decoration [dl_get stimdg:sample_decoration $id]
   } else {
       set decoration none
   }
   ```

### Variant loader_options Format

Options can be simple values or `{label value}` pairs:

```tcl
# Simple list — first element is default
n_rep { 50 100 200 }

# Name/value pairs — dropdown shows name, loader receives value
transparency { {off 0.0} {low 0.2} {high 0.8} }
sample_decoration { none {slow {blink 1.0 0.5}} {fast {blink 4.0 0.5}} }
```

The option name **must match** the loader parameter name exactly.
The first element is always the default. For a **list-valued** option (a factor
crossed inside the loader, like `gravities`), each choice must be wrapped
`{label {the list}}` — e.g. `launch_ecc { {three {6 9 12}} {one {9}} }`; a bare
`{6 9 12}` is misread as three separate scalar choices.

**Comment placement (breaks the rig, not just ess_test):** `##` comments must go
**outside** a multi-line braced option value, never inside its braces. The
variants loader (`normalize_variants` → `strip_comments`, shared by ESS and
ess_test) only strips *top-level* comment lines *between* options; a `##` line
inside a `coh_params { … }` / `probe_params { … }` block survives as list words,
so the default-extraction (`[lindex [lindex $norm 0] 1]`) grabs `##` and the
loader dies with `missing value to go with key`. Put the comment on the line(s)
*before* the option keyword.

Also: `*_variants.tcl` ends with `set variants [subst $variants]`, so any `$` or
`[...]` in a variant value/comment is evaluated — keep values `$`/`[]`-free
except intended preset refs like `$gravity_presets`.

### Common dl_* Operations

```tcl
dl_set $g:column [dl_repeat value $n]        ;# create column of repeated value
dl_set $g:column [dl_repeat [dl_slist str] $n] ;# string column
dl_get stimdg:column $index                   ;# read single value
dl_exists stimdg:column                       ;# check if column exists
dl_length stimdg:column                       ;# number of rows
dl_sum stimdg:remaining                       ;# sum of column values
dl_select stimdg:col [dl_gt stimdg:remaining 0] ;# filter
dl_put stimdg:remaining $id 0                 ;# set single value
dl_shuffle [dl_repeat "0 1" $n_per_side]      ;# shuffled list
dl_fromto 0 $n                                ;# 0,1,2,...,n-1
dl_ones $n                                    ;# list of n ones
```

### Dynlist lifetimes — plain `set` and `return` work

Current dlsh gives dynlists refcounted handles, so ordinary Tcl works:

```tcl
proc make_col { n } { return [dl_fromto 0 $n] }   ;# plain return is fine
set col [make_col 100]                            ;# plain set keeps it alive
lappend cols [make_col 50]                        ;# so does a list, or a dict
```

Before this, a list died with the frame that made it, so a proc had to hand it
back with `dl_yield` (or `dl_return`) and callers bound it with `dl_local`.
Both still work and existing code needs no migration — `dl_local` and
`dl_yield` are unchanged and still correct. New code does not need them.

**Returning more than one list.** You are no longer limited to one. What
matters is whether the value you return still *holds the objects*:

```tcl
return [list $a $b]       ;# works -- a Tcl list holds both objects
dict set d a $a; return $d ;# works -- so does a dict
return [dl_llist $a $b]   ;# works -- and is a real dynlist column
return "$a $b"            ;# BROKEN -- string interpolation keeps only the
                          ;#   names, and a bare name is not a claim
```

Containers that hold Tcl objects preserve the references; building a string
throws them away. (Previously none of these worked with a plain `return` --
you needed `dl_yield`, which takes exactly one list, so bundling into a
`dl_llist` was the workaround for that limit.)

**The rule.** A list has two independent claims and is freed when both are
gone: the *frame claim* (a hidden variable in whichever frame created it —
this is what `dl_local` re-parents onto your variable and `dl_yield` hands up
a level) and *object references* (your `set`, a `lappend`, a `dict set`, a
returned value). The refcount only ever extends a lifetime, never shortens
one, which is why nothing that worked before changed behavior.

Consequences worth knowing:

- **`unset x` does not free the list.** It drops your reference; the frame
  claim outlives it, until the proc returns (or `dl_clean` at top level).
  `dl_delete $x` frees immediately and overrides outstanding references.
- **Temps still accumulate at the top level.** Inside a proc they are
  reclaimed on return; at the global scope they live until `dl_clean`. Do
  bulk list work inside a proc — which loaders already do.
- **Dynlists are objects, not values.** `set b $a` aliases the same list and
  `dl_append $b ...` shows through `$a`. Use `dl_copy` for a snapshot. This
  is long-standing behavior, not new.
- **Do not apply Tcl list/string commands to a handle.** `llength $dl` on a
  dynlist is meaningless (it returns 1 — the name is one word) and is the one
  case that can still strand a list. Use `dl_length $dl`, or
  `llength [dl_tcllist $dl]` when you genuinely want a Tcl list.

### Vectorized Loaders — build stimdg with column math, not row loops

**This is a goal for every loader.** Construct `stimdg` with whole-column `dl_*`
math, NOT Tcl `for`/`foreach` row loops + `lappend` + `dl_flist {*}$list`. dlsh is
array-oriented, the `dl_*` library is always available in dserv, and vectorized
loaders are shorter, idiomatic, and scale to large trial sets. Before hand-rolling
anything, run `info commands dl_*` — the primitive you want usually already exists.

**Flat columns**
- `dl_repeat L n` — repeat each **element** n times: `{0 2} 3` → `0 0 0 2 2 2`
- `dl_replicate L n` — **tile** the whole list n times: `{0 2} 3` → `0 2 0 2 0 2`
- `dl_repeat [dl_flist $v] $n` — the constant-column idiom (also `dl_ilist`/`dl_slist`)
- `dl_cross A B` — full factorial; returns the two parallel columns (A varies fastest).
  Reach for it when you have **two or more** crossed factors; a single factor crossed
  with repetitions is just `dl_repeat`/`dl_replicate`.
- Scalars broadcast against a list: `dl_mult $col $k`, `dl_add`, `dl_sub`
- `dl_max`/`dl_min` are elementwise with **two** args; single-arg is a reduction

**Nested columns (one row per trial)** — for per-trial sets, e.g. which targets are shown
- `dl_reshape $flat $n_rows $n_cols` — flat → one sublist per trial
- **`dl_randchoose $mlist $n`** — the per-trial draw. Populations and counts may each be
  a list or a scalar (**scalars broadcast**), returning for each row `n_i` **unique**
  indices from `0..m_i-1`; ragged is fine. `dl_randchoose [dl_repeat $n_pool $n_obs] $k`
  is "k unique of the pool, per trial", in one call. When `n == m` it's a full
  permutation. Sampling is uniform in both the chosen set *and* its order (Floyd's), and
  costs O(n) regardless of pool size.
- **`dl_randchooseLists $nested $n`** — same idea, but draws n unique **elements** from
  each row's own candidate set (rows may differ). Compiled into libdlsh.
- `dl_randfill $n` → a random permutation of `0..n-1`; `dl_randfill $lengths` → one
  permutation per element (nested). This is what `dl_shuffleLists` is built on.
- `dl_shuffleLists $nested` — permutes **each row independently**
- `dl_sort $nested` — sorts **within** each row (so does `dl_bsort`)
- `dl_choose` — the **index depth** decides the meaning. **Flat data + nested index** →
  per-row lookup into the flat list (how you turn drawn indices into pool values).
  With **nested data**: a **flat** index selects whole rows; a **nested** index indexes
  *within* each row; a **single index row broadcasts** across every row. A row-count
  mismatch errors rather than misbehaving silently. Per-row indexing does *not* enforce
  uniqueness — `dl_randchoose` (or shuffle-then-take) is what guarantees distinct picks.
- Elementwise math (`dl_mult`, `dl_sin`, `dl_cos`, …) maps over a nested list and
  **preserves its shape** — compute per-row geometry straight off the nested column,
  with no flatten/reshape round-trip.

**Recipes**

```tcl
# k unique items per trial, drawn independently for each trial -- no loops.
# dl_randchoose gives k unique INDICES per row (k broadcasts); dl_choose maps
# them onto the pool.
dl_local idx [dl_randchoose [dl_repeat $n_pool $n_obs] $k]
dl_local sel [dl_sort [dl_choose $pool $idx]]

# balanced 1-per-trial: each item exactly n_rep times, random order
dl_local sel  [dl_reshape [dl_shuffle [dl_repeat $pool $n_rep]] $n_obs 1]
```

**Gotchas**
- `dl_randchoose`'s first arg is the **population** m, the second the **count** n
  (`n <= m`). Lists and scalars mix freely — a scalar broadcasts. An `n > m` in any row
  is a real error, not a usage mistake.
- `dl_sublist` is *not* a per-row slice; it has no help string and returns `0` on bad
  args rather than erroring. Don't reach for it.
- Randomization is seedable: `dl_srand N` makes `dl_shuffle`, `dl_randfill`, `dl_shuffleLists`
  **and** `dl_randchoose` reproducible, so a trial set can be regenerated exactly.
- `dl` lists are float32: trig near-zeros come out ~1e-7 rather than exactly 0
  (harmless at degrees-of-visual-angle scale, but visible in a `dl_tcllist` dump).
  Consequence: never test a *computed* float column for a value with `dl_eq` — it
  misses the ones that rounded (e.g. a "should-be-0" that is `-5e-7`). Use
  `dl_closeto $x $val` (default eps 1e-4, tunable) or `dl_lt [dl_abs [dl_sub $x $val]] $eps`.
  Keep eps above the ~1e-7 float32 floor (1e-9 is too tight).
- Use `dl_local` for intermediates (auto-cleaned when the loader returns).

**Verify** a vectorized rewrite headlessly with `ess_test`: assert trial counts, row
widths, and value ranges — and also the properties a shape check misses, such as *no
duplicate within a row* and *randomized rows actually vary* (a broken vectorized
shuffle silently yields one repeated pattern).

---

## Stim File Pattern

Stim files run on the stim2 renderer (GLFW/OpenGL), not in dserv.
They receive the stimdg and implement visual stimulus presentation:

```tcl
proc nexttrial { id } {
    glistInit 2            ;# 2 display groups
    resetObjList

    # Read stimdg columns
    foreach t "sample match nonmatch" {
        foreach p "${t}_x ${t}_y ${t}_r ${t}_color" {
            set $p [dl_get stimdg:$p $id]
        }
    }

    # Create objects in display groups
    set obj [polygon]
    objName $obj sample_obj
    polycolor $obj {*}[dl_tcllist $sample_color]
    translateObj $obj $sample_x $sample_y
    scaleObj $obj [expr 2*$sample_r]
    glistAddObject $obj 0       ;# group 0 = sample

    # Animation support
    animateBlink sample_obj -rate 2.0 -duty 0.5
    glistSetDynamic 0 1         ;# enable animation for group 0
}

proc sample_on {} {
    glistSetCurGroup 0
    glistSetVisible 1
    redraw
}

proc sample_off {} {
    glistSetCurGroup 0
    glistSetVisible 0
    redraw
}
```

---

## Visualization Config Pattern

Viz configs run in a **separate process** (vizconf.tcl subprocess) with its own
Tcl interpreter. They have `dlsh` and `evtSetScript`/`evtSetScriptByName` but
**not** the `ess` package. The viz config script is published as a string via
`dservSet ess/viz_config` and evaluated inside `namespace eval ::viz::$system`.

### Using Named Events

The `evtSetScriptByName` command (defined in vizconf.tcl) resolves event type
and subtype names to numeric IDs using lookup tables published by ess to
`ess/evt_type_ids` and `ess/evt_subtype_ids`.

```tcl
$s set_viz_config {
    proc setup {} {
        evtSetScriptByName USER RESET       [namespace current]::reset
        evtSetScriptByName SYSTEM_STATE STOPPED [namespace current]::stop
        evtSetScriptByName BEGINOBS *        [namespace current]::beginobs
        evtSetScriptByName ENDOBS *          [namespace current]::endobs
        evtSetScriptByName STIMTYPE *        [namespace current]::stimtype
        evtSetScriptByName SAMPLE ON         [namespace current]::sample_on
        evtSetScriptByName SAMPLE OFF        [namespace current]::sample_off
        evtSetScriptByName CHOICES ON        [namespace current]::choices_on
        evtSetScriptByName CHOICES OFF       [namespace current]::choices_off

        clearwin
        setbackground [dlg_rgbcolor 100 100 100]
        setwindow -8 -8 8 8
        flushwin
    }

    proc stimtype { type subtype data } {
        variable trial
        set trial $data
        # Cache per-trial values from stimdg
        variable sample_x [dl_get stimdg:sample_x $trial]
        ...
    }

    proc sample_on { type subtype data } {
        variable sample_x; variable sample_y; variable sample_r
        clearwin
        dlg_markers $sample_x $sample_y fsquare -size ${sample_r}x -color white
        flushwin    ;# this calls ::viz::update_display
    }

    setup
}
```

### Available in viz context

- `dlg_markers`, `dlg_text`, `dlg_lines` — drawing commands (support named colors)
- `dlg_rgbcolor r g b` — pack RGB into color index
- `clearwin`, `setwindow`, `setbackground` — window management
- `flushwin` — push display to output (aliased to `::viz::update_display`)
- `evtSetScriptByName TYPE SUBTYPE script` — register event handler by name
- `dl_get`, `dl_exists`, `dl_length`, etc. — stimdg access
- Event handler signature: `proc name { type subtype data } { ... }`
- Use `*` or `-1` for subtype to match all subtypes

### Common Event Types for Viz

| Name | ID | Subtypes | Use |
|------|----|----------|-----|
| USER | 3 | RESET=2 | System reset |
| SYSTEM_STATE | 7 | STOPPED=0, RUNNING=1 | State changes |
| BEGINOBS | 19 | * | Observation start |
| ENDOBS | 20 | COMPLETE=1 | Observation end |
| STIMTYPE | 29 | STIMID=1 | Trial type (data = stimtype index) |
| SAMPLE | 30 | OFF=0, ON=1 | Sample display |
| PATTERN | 28 | OFF=0, ON=1 | Pattern/stimulus display |
| RESP | 37 | varies | Response (subtype = response code) |
| ENDTRIAL | 40 | INCORRECT=0, CORRECT=1 | Trial outcome |
| CHOICES | 50 | OFF=0, ON=1 | Choice display |
| FEEDBACK | 49 | OFF=0, ON=1 | Feedback display |

---

## Event System

Events are logged via `::ess::evt_put`:

```tcl
::ess::evt_put TYPE SUBTYPE [now]                    ;# no params
::ess::evt_put TYPE SUBTYPE [now] $param             ;# with param
::ess::evt_put STIMTYPE STIMID [now] $stimtype       ;# trial type
::ess::evt_put ENDTRIAL CORRECT [now]                ;# correct trial
::ess::evt_put REWARD MICROLITERS [now] [expr {int($juice_ml*1000)}]
```

Event lookup tables are published to dserv datapoints for use by vizconf
and web clients:
- `ess/evt_type_ids` — Tcl dict mapping type names to IDs
- `ess/evt_subtype_ids` — nested Tcl dict mapping type→subtype names to IDs

---

## Web GUI / dserv Pub/Sub Pattern

Web GUIs connect to dserv via WebSocket using `DservConnection` and
`DatapointManager`:

```javascript
// Subscribe to datapoint updates
dpManager.subscribe('ess/evt_type_ids', (dpData) => {
    const value = dpData.data !== undefined ? dpData.data : dpData.value;
    // process value...
});

// Touch datapoints to get current values (subscriptions only fire on changes)
const message = {
    cmd: 'eval',
    script: 'foreach v { ess/evt_type_ids ess/evt_subtype_ids } { catch { dservTouch $v } }'
};
conn.ws.send(JSON.stringify(message));
```

Key patterns:
- Subscriptions only fire on value changes, not on initial subscribe
- Use `dservTouch` (via eval command) to trigger republish of current values
- Batch touches in a `foreach` for efficiency
- Always `catch` individual touches in case datapoints don't exist yet
- The `requestInitialData()` pattern in `ess_app.js` shows the canonical approach

---

## Touch and Eye Movement APIs

```tcl
# Touch regions
::ess::touch_init
::ess::touch_win_set $win $cx $cy $radius $type    ;# type: 0=rect, 1=circle
::ess::touch_region_on $win
::ess::touch_region_off $win
::ess::touch_in_win $win                            ;# returns 1 if touched
::ess::touch_reset
::ess::touch_deinit

# Eye movement windows
::ess::em_init
::ess::em_fixwin_set $win $cx $cy $radius $type
::ess::em_region_on $win
::ess::em_region_off $win
::ess::em_eye_in_region $win                        ;# returns 1 if eye in window
```

---

## dlg_* Drawing Commands (viz cheat sheet)

Authoritative option grammar, from `dlsh/src/tcl_dlg.c`. **The color/width flags
differ per command — do not guess.** `dlg_lines` in particular does **not** take
`-color` or `-lcolor`, and its width flag is `-lwidth`, not `-linewidth` (a wrong
flag errors only at draw time on the rig: `dlg_lines: bad option -lcolor`).

| Command | Positional args | Color flag(s) | Width | Other options |
|---|---|---|---|---|
| `dlg_lines` | **`xlist ylist`** (one polyline through the coord lists) | `-linecolor` / `-linecolors`, `-fillcolor` / `-fillcolors` | `-lwidth` | `-lstyle -filled 0\|1 -closed -skip n -boxfilter n -sideways -start -width -interbar` |
| `dlg_markers` | `xlist ylist [marker [size]]` | `-color` / `-colors` | `-lwidth` | `-marker <name> -size n[x\|y\|s\|w] -sizes <dl> -clip n -scaletype x\|y\|s\|u\|w` |
| `dlg_text` | `xlist ylist string` | `-color` / `-colors` | — | `-font <name> -size n[s] -just <n> -spacing <n>` |

- **`dlg_lines` draws a polyline, NOT a segment from 4 scalars.** For a single
  segment pass two 2-element lists: `dlg_lines "$x0 $x1" "$y0 $y1" -linecolor red -lwidth 1`.
  For a curve, sample it into an x-list and a y-list and stroke **once**. Calling
  `dlg_lines $x0 $y0 $x1 $y1` misparses `x1 y1` as `lstyle lwidth` — wrong shape,
  no error. `-closed` joins the last point back to the first (filled polygons).
- **Marker names** (case-insensitive, `f`-prefix = filled): `square`/`fsquare`,
  `circle`/`fcircle`, `triangle`, `diamond`, `plus`, `htick`/`htick_l`/`htick_r`,
  `vtick`/`vtick_u`/`vtick_d`. Given as the 3rd positional or via `-marker`.
- **`-size` suffix** picks the unit: `2x` = 2 in x-data-units (the usual choice
  so markers scale with `setwindow`), `s` = scaled by the x-scale, bare = pixels.
- **Colors**: any color flag takes a `dlg_rgbcolor r g b` index **or** a named
  color: `white black red green blue yellow cyan magenta orange gray`/`grey`
  `darkgray lightgray pink brown purple`.

```tcl
dlg_markers $x $y fcircle -size 0.5x -color yellow          ;# filled dot, data units
dlg_markers $x $y fsquare -size 2x -color [dlg_rgbcolor 255 0 0]
dlg_lines "$x0 $x1" "$y0 $y1" -linecolor [dlg_rgbcolor 80 80 80] -lwidth 1
dlg_text -15 8 "label" -size 12 -color white
```

### Testing viz / dlg_* code headless

**The real `dl_*` and `dlg_*` commands run in bare `dlsh`/`dlsh -e`** — dlsh
carries a default (offscreen) cgraph context, so `dlg_lines`/`dlg_markers`/
`dlg_text`/`dlg_rgbcolor` and `clearwin`/`setwindow`/`setbackground` all execute
and the **real option parser** validates flag names, arity, color names, and
marker names (it rejects `dlg_lines -lcolor` for real). So don't stub `dlg_*`
with no-ops that swallow anything — that is exactly what lets a bad-flag bug
reach the rig. Instead **run the actual draw procs against the real commands**:

```tcl
# capture the set_viz_config body (stub the system object's set_viz_config),
# then instantiate it and fire representative events -- real dlg validates each call
foreach c { flushwin evtSetScriptByName evtSetScript } { proc ::$c {args} {} }  ;# vizconf-only shims
rename dlg_lines ::__real_dlg_lines
proc ::dlg_lines {args} { incr ::nline; ::__real_dlg_lines {*}$args }           ;# real parser + a tally
namespace eval ::viz::$system $viz_script
namespace eval ::viz::$system "stimtype 29 1 $row"    ;# errors here == errors on the rig
```

Only `flushwin` (the live-display push) and the vizconf event hooks
(`evtSetScriptByName`) aren't in dlsh — stub just those. What still needs the
real rig: whether the geometry actually *looks* right (positions, overlap,
legibility) — dlg validates the calls, not the picture.

---

## Sound API

```tcl
::ess::sound_init
::ess::sound_play $channel $note $duration_ms
# Common: channel 1=beep, 3=reward, 4=error, 6=finale
```

## Juice/Reward API

```tcl
::ess::juicer_init
::ess::reward $ml
```

---

## Testing with ess_test (headless loader/stim harness)

`ess_test` is a dlsh package (`package require ess_test`) that validates the
**deterministic logic and data output** of loaders and stim files **without
dserv, OpenGL, the rig, or any hardware**. Use it for a fast inner loop when
writing or changing a loader/stim: run it from a bare `dlsh -e` or a script,
assert on `stimdg` columns and on the per-frame values the stim hands stim2.

**Honest boundary — what it can and cannot check:**
- **CAN** (numbers, machine-checkable): loader → `stimdg` (columns, counts,
  per-row values), timing math (fraction→ms, coherence-dip placement, probe
  onset), per-frame driver output (position, direction/tangent, speed,
  coherence via the *real* `mp_sim` timeline), event timing (`dserv_send_evt`),
  and that a file **sources** clean (syntax / `package require` / `objName`).
- **CANNOT** (needs real stim2 + eyes): whether a stimulus actually looks
  right — invisibility, masking/sampler appearance, dot rendering, real vsync.

Pair the two: `ess_test` catches math/data/timing regressions headless; real
stim2 confirms appearance.

### Tier 1 — loader → stimdg (pure dlsh)

```tcl
package require ess_test
ess_test::load_loaders <system> <protocol>     ;# sources *_loaders.tcl INSIDE ::ess
ess_test::loader_defaults <loader> { <all params with sane defaults> }
set g [ess_test::run_loader { <keys to override> }]   ;# returns the stimdg handle

ess_test::assert {[dl_length $g:stimtype] == <n>} "trial count"
ess_test::assert {[dl_exists $g:my_new_col]}      "new column present"
puts [ess_test::dg_summary $g]                        ;# columns + lengths + samples
ess_test::summary
```

`load_loaders` sources the file inside `namespace eval ::ess` and `run_loader`
runs the loader body via `apply` from the global namespace — matching the real
oo-method context, so absolute-name helper calls
(`::ess::sys::proto::my_helper`) must resolve exactly as on the rig. This is
deliberate: a namespace-resolution bug surfaces here, not on the rig. (Every
current system nests its namespace under the directory name — hyphens and all,
e.g. `prf/drifting-gratings` → `::ess::prf::drifting-gratings`, matching how the
real ess calls `::ess::<sys>::<proto>::loaders_init`.)

### Tier 2 — per-frame stim driver (capturing stubs)

```tcl
ess_test::stub_stim2                              ;# stub GL/motionpatch/dserv, capture writes
ess_test::stim_source <system> <protocol>         ;# sources *_stim.tcl headless

ess_test::set_time 0
nexttrial 0 [dl_get $g:fix_x 0] [dl_get $g:fix_y 0] [dl_get $g:fix_r 0]
ess_test::clear_captures                          ;# ignore build-time writes
target_on ; pursuit_start                         ;# whatever your protocol drives
ess_test::play -dur [dl_get $g:land_time 0] -dt 0.016

ess_test::values motionpatch_direction dots_target    ;# per-frame value list
ess_test::last   motionpatch_coherence dots_target    ;# most recent record
ess_test::events pursuit/probe_on                     ;# {t name payload} records
```

`stub_stim2` captures **every** `motionpatch_*` setter plus `translateObj`,
`polycolor`, `setVisible`, `objName` (handle↔name), and `dserv_send_evt`, keyed
by the registered object name. `step`/`play` advance a synthetic `StimTimeF`,
run the registered `addPreScript` drivers, then the `addThisFrameScript` (event)
queue. dserv comm (`qpcs::dsSet`/`dsGet`/`dsStimAddMatch`, `$::dservhost`) is
stubbed so nothing connects.

### Key commands

| Command | Purpose |
|---|---|
| `load_loaders sys proto` | source loaders in `::ess`, run `loaders_init`, return loader names |
| `loader_params ?name?` / `loader_defaults name dict` | param list / register defaults |
| `run_loader ?name? <dict\|list>` | run a loader body, return `stimdg` |
| `run_variant sys proto variant` | run the loader as ESS would for a variant's defaults → `stimdg` |
| `dg_summary g` | columns + lengths + sample rows |
| `stub_stim2` / `stim_source sys proto` | install stubs / source the stim file |
| `set_time` / `step ?-dt?` / `play -dur -dt` | drive the synthetic frame clock |
| `captured cmd ?target?` / `last` / `values` / `series` | query per-frame writes |
| `events ?name?` / `event_time name` | query captured `dserv_send_evt` |
| `assert {expr} msg` / `approx` / `in_range` / `test` / `summary` | assertions + tally |

### Running

```sh
dlsh my_tests.tcl                                    # a test script
dlsh -e 'package require ess_test; source my_tests.tcl'
```

`ess_test` is in `dlsh.zip`, so `package require ess_test` works from `dlsh`,
`dlsh -e`, and any `tclsh` after `source /usr/local/dlsh/dlsh_setup.tcl`. Its
default systems root is `~/systems/ess` (override:
`ess_test::config -systems_root <path>`). See the package's own `README.md`,
`test_ess_test.tcl` (a worked example against `pursuit/ballistic`), and
`test_variants.tcl` (runs every variant's loader across the collection).

To run real loaders headless the harness reproduces enough ESS context: it
injects **ambient vars** (`screen_halfx/halfy`) plus the **system/protocol
params+variables** it harvests from `add_param`/`add_variable` (so a loader
reading `$n_choices` works), puts **`<systems_root>/lib`** on the Tcl module
path (so `package require planko`/`haptic`/`blob` resolve), **stubs dserv**
(`dservGet` seeded, default `ess/screen_refresh_rate 60`) and a few ESS globals
(`::ess::system_path`, `::ess::rmt_host`, …), binds **`my`** for delegating
loaders, and best-effort **sources the system + protocol files**. So
`ess_test::run_variant sys proto variant` dry-runs any variant's trial
generation without hand-writing args — and a sweep (`test_variants.tcl`) flags
real problems: it has caught an arg-count bug in a delegating loader and loaders
that depend on runtime-computed state or an external DB.

**Requirement:** a script is testable headless only if the packages it
`package require`s load headless — the pure-dlsh ones (`launch_sim`, `mp_sim`,
`sqlite3`) plus anything under `<systems_root>/lib`. A stim that does real dserv
I/O at load is kept headless by the dserv stubs, but a loader that needs live
datapoint *values* or a real data file will surface a clear error.

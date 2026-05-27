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
The first element is always the default.

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

## dlg_* Color Support

The `-color` option on `dlg_markers`, `dlg_text`, `dlg_lines` (and variants
`-linecolor`, `-fillcolor`, `-lcolor`) accepts both numeric color indices
(from `dlg_rgbcolor`) and named colors:

Named colors: `white`, `black`, `red`, `green`, `blue`, `yellow`, `cyan`,
`magenta`, `orange`, `gray`/`grey`, `darkgray`/`darkgrey`,
`lightgray`/`lightgrey`, `pink`, `brown`, `purple`.

```tcl
dlg_text 0 0 "hello" -size 14 -color white
dlg_markers $x $y fsquare -size 2x -color [dlg_rgbcolor 255 0 0]
dlg_markers $x $y fsquare -size 2x -color red    ;# equivalent
```

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

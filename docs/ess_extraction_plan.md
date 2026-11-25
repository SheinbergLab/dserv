# Slimming `ess-2.0.tm`: the next extractions

A plan, not started. Surveyed on 2026-11-24 at commit `db67231d`, when
`ess-2.0.tm` was 8,906 lines. **Re-measure the line ranges before cutting.**
They drift with every edit; the structure will not.

Done so far: `ess_transports` (input routing, 2026-08; see
`docs/ess_transports_extraction.md`), `ess_photo` (photodiode, `60f16576`)
and `ess_stim` (stim2 command log, bindings, frame log, tlock,
evt_display, script/lib records, late draw; `db67231d`). About a third of
what is left in `ess-2.0.tm` is not on the trial path.

## How to move a block (the pattern used for all three)

- **Copy the blocks unchanged.** Keep any `# BEGIN`/`# END` markers,
  because tests evaluate blocks by text. Diff the moved text against the
  old lines before committing.
- **Wrap them in the same namespace.** The new module is
  `package provide <name> 1.0` followed by `namespace eval ess { ... }`, so
  every cross-reference resolves at call time.
- **Load it from ess, never the reverse.** `ess-2.0.tm` requires the module
  at the top; the module never requires `ess`.
- **Watch for code that runs at load.** For `ess_stim` that was the
  `::rmtSend` rename and the `stim_cmd_hook` registration. They still run
  at `package require ess`, a few lines earlier than before.
- **Leave a short pointer comment where the blocks were.**
- **Fix the tests that read the blocks.** Tests read either a whole block
  (`string first "# BEGIN x"`) or one proc by name
  (`string first "\n    proc name "`), from a file named in the test.
  Point each at the new file. Where a test also checks call sites by
  position, put the module's text FIRST and ess-2.0.tm after it, as
  `test_ess_stim_cmd` does.
  `test_ess_transports` already scans every `lib/ess*.tm`.
- **Check that the loaded package is the same.** Load it from source before
  and after, with a stand-in `rmtSend`, and dump every `::ess` proc (body
  and args) and every variable's value. The only difference should be
  `loading_start_time`. The script:

  ```tcl
  tcl::tm::path add [file normalize lib]
  proc ::rmtSend { args } { return "raw:$args" }
  foreach c {dservSet dservGet dservSetData dservTouch dservExists now} { proc ::$c args {} }
  package require ess
  puts "wrapper=[string trim [info body ::rmtSend]] raw=[info commands ::ess::rmtSend_raw] hook=$::ess::stim_cmd_hook"
  foreach p [lsort [info procs ::ess::*]] { puts "P $p [string length [info body $p]] [info args $p]" }
  foreach v [lsort [info vars ::ess::*]] {
      if {[array exists $v]} { puts "V $v (array) [lsort [array names $v]]" } \
      elseif {[info exists $v]} { puts "V $v [list [set $v]]" } else { puts "V $v (unset)" }
  }
  ```

  Run it with `/usr/local/bin/dlsh` from the repo root (the new tree), and
  from a scratch tree that holds HEAD's `lib/` (the old one). Then diff the
  two outputs.

- **Run the tests:**
  `ctest --test-dir build -R "ess_|photo|df_|extio_record|stim"`.
- **Rigs need the new module installed and dserv restarted.** A system
  reload does not re-require `ess`.

## Candidates, in the order to do them

### 1. Script and lib file editing → `ess_scriptfiles` (~1,190 lines)

| part | lines (db67231d) |
|---|---|
| `analyze_ess_patterns`, `validate_script*` levels, backup info and restore | 4757–5283 |
| ownership helpers, backup dir, `backup_script`, `save_script`, `get_script_backups`, `cleanup_old_backups` | 5285–5617 |
| lib files: `get_lib_files`, `get_lib_file_content`, `save_lib_file`, lib backups and restore | 8269–8602 |

Nothing here runs at load: `init_backup_system` sets the backup directory
up on first use.

**Live callers, which must keep working:**
- `save_script`: config/essctrl.tcl, lib/ess_validation-1.0.tm, and the
  tools/essgui and tools/essqt GUIs.
- `get_lib_files`, `get_lib_file_content` and `save_lib_file`:
  lib/ess_sync-1.0.tm. `commit_lib` depends on them, and the scripts
  subprocess depends on `commit_lib`.

**Delete first rather than move.** None of these had a caller anywhere in
the repo on 2026-11-24; the workbench became a read-only inspector in
2026-08:
- `restore_script_backup`
- `get_backup_info`, `get_backup_info_json`, `get_backup_info_enhanced`
- probably the `minimal`/`strict` validation levels

Check `~/systems/ess` and `git grep` again before deleting.

### 2. Devices → `ess_sound` and `ess_windows` (~690 lines)

| part | lines (db67231d) |
|---|---|
| eye-movement windows and the sampler (`em_*`) | 5623–5802 |
| sound and wav (`sound_*`, `wav_*`: feedback volume, gains, settingsdb persistence) | 5808–6092 |
| juicer stub (`juicer_init`, `reward`) | 6098–6105 |
| touch windows (`touch_*`) | 6110–6325 |

Self-contained, and the same shape as `ess_dial`, `ess_roam` and
`ess_sling`. Systems call these by name (on 2026-11-24, `em_fixwin_set` in
17 files, `touch_win_set` in 14, `sound_play` in 42), and the names do not
change.

`sound_init` is called from config/essconf.tcl and from the dserv-agent
scaffold. `em_region_set` and `touch_region_set` match names in
processors/windows.c and touch_windows.c (those are the processors' own
parameters). The transports work order also kept `em_windows`, `sound`,
`juicer` and `touch_windows` out of `ess_transports`: they are subsystems,
not input routing.

### 3. Stim event timing → into `ess_stim` (~200 lines)

The procs are `stim_evt_set_anchor`, `stim_evt_reset_anchor`,
`stim_evt_wall_ok`, `stim_evt_time`, `stim_evt_put`, `stim_tag_evt` and
`stim_tag_params` (~3700–3895): placing events by stim2's flip wall clock.
This finishes the stim module.

`tests/test_ess_stim_evt_time.tcl` and `test_ess_stim_tag_params.tcl` read
these procs by name from ess-2.0.tm; point each at the new file.

`evt_put` calls into this code, and it runs on every event. Moving the code
does not change that cost: the calls still resolve in the same `ess`
namespace.

### 4. GUI introspection → `ess_introspect` (~650 lines)

| part | lines (db67231d) |
|---|---|
| variant-options editor: `_scan_element` … `set_variant_options`, `resource_variants` | 6920–7290 |
| `get_system_dict`, `system_dict_to_json`, `variant_info_to_json`, `variant_arg_options_to_json` | 7568–7800 |
| `system_snapshot_json`, `publish_snapshot` (top-level `proc ess::…`) | 7805–7914 |
| `get_states`, `find_transitions`, `get_state_transitions`, `find_rmt_cmds`, `get_rmt_cmds` | 6332–6416 |

**Callers outside this group:**
- www/js/VariantOptionsModal.js and tools/essgui: `set_variant_options`,
  `variant_options_json`, `test_loader`.
- ess_statemap_plugin.js: `get_state_transitions`.
- The load path, at call time: `load_system_body` calls `get_rmt_cmds`, and
  `variant_init_body` calls `variant_info_to_json` and `publish_snapshot`.

### 5. Config loading → `ess_config` (~325 lines)

This is `capture_config`, `validate_config`, `load_config` and the
`apply_pending_config_args`/`params` hooks (7920–8244). It is called from
config/essconf.tcl and lib/ess_configs-1.0.tm. That module runs in the
configs subprocess, a different interpreter, so don't merge the two.

It is tied more closely than the others: `load_system` and
`variant_init_body` call the pending-config hooks partway through a load.
Move it last, and test a config load on a rig.

## Dead code to delete, not move

None of these had a caller on 2026-11-24 in the repo (git grep) or, for
the ones checked, in `~/systems/ess`:

- `file_load_ess`, `ess_to_dgz`, `ess_to_json`
- `remote_eval`, `error_untrace`, `get_system_json`
- `variant_arg_options_to_json`
- `load_config_json`, `config_to_json`, `get_config_schema`

`get_system_status` appears only in a commented-out line in `dockerfile`.

JS clients build some commands as strings, so grep `www/js` for each bare
name before deleting.

## What stays in `ess-2.0.tm`

- The `System` class (~1,090 lines; the core object).
- Loading: `load_system`, reload, load reports.
- The session: `start`/`stop`, hold and boundary stop, obs scheduling,
  `begin_obs`/`end_obs`, `evt_put`, params, `file_open`/`file_close`.
- Participant binding: it sits on the `set_subject`/`file_open` path.
- The `evt_info` table at the end, which is data.
- Optional, small: the camera-grab section (~125 lines, 4086–4210), which
  `test_ess_camera` reads by its section banners.

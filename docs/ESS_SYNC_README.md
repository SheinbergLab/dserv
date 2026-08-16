# ESS Registry Sync — Command Reference

Quick reference for the Tcl commands provided by `ess_sync-1.0.tm`.
All live in the `ess::` namespace.

## Setup

```tcl
package require ess_sync
ess::registry_configure -url https://server:port -workgroup mylab
```

Or set via environment: `ESS_REGISTRY_URL`, `ESS_WORKGROUP`.

---

## Syncing

| Command | Description |
|---------|-------------|
| `ess::sync_base` | Sync all systems + libs in workgroup from registry. One call to refresh everything. |
| `ess::sync_system <name>` | Sync a single system. Sends local checksums, pulls only changed scripts. |
| `ess::sync_libs` | Sync shared lib `.tm` files by checksum. |
| `ess::sync_status` | Compare local base checksums vs registry for current system. Returns per-script status: `synced`, `modified`, `local_only`, `registry_only`. |
| `ess::lib_sync_status` | Same as above but for lib files. |

```tcl
ess::sync_base                      ;# full refresh
ess::sync_system match_to_sample    ;# just one system
ess::sync_status                    ;# what's different?
```

## Committing

| Command | Description |
|---------|-------------|
| `ess::commit_script <type> ?comment?` | Push one base script to registry. Types: `system`, `protocol`, `stim`, `loaders`, `variants`, `extract`, `sys_extract`, `sys_analyze`, etc. |
| `ess::commit_system ?comment?` | Push all base scripts for current system+protocol. |
| `ess::commit_lib <filename> ?comment?` | Push a lib `.tm` from base to registry. |
| `ess::seed_libs` | Push all local libs to registry (skips unchanged). |

```tcl
ess::commit_script protocol "fix timing bug"
ess::commit_system "release v2"
ess::commit_lib "utils-1.0.tm"
```

## Editor identity

Edits go directly to the local script tree (the per-user overlay layer
was retired 2026-08; the manifest-based sync made a second local layer
redundant). `ess::set_editor_user` (defined in `ess-2.0.tm`, wrapped in
`essctrl.tcl`) records who is editing — it tags saves and is the default
identity for commits.

| Command | Description |
|---------|-------------|
| `ess::set_editor_user <name>` | Set the editor identity (attribution only; empty string clears it). |
| `ess::get_editor_user` | Return the current editor identity. |

The typical workflow is: **set user → edit locally → commit to registry**.

The user identity also controls registry permissions — `commit_script`,
`commit_system`, and `commit_lib` check the user's role via the registry
and block commits if the role is `viewer`. Roles are `admin`, `editor`,
or `viewer`.

```tcl
ess::set_editor_user alice        ;# now editing as alice
# ... edit scripts ...
ess::commit_script protocol       ;# push local → registry
```

## Scaffolding

| Command | Description |
|---------|-------------|
| `ess::scaffold_system <name> ?opts?` | Create new system from clone, template, or skeleton. |
| `ess::scaffold_protocol <name> ?opts?` | Create new protocol by cloning or from skeleton. |
| `ess::scaffold_info ?-system name?` | List available protocols/templates for scaffolding. |

```tcl
ess::scaffold_system prf_v2 -from prf
ess::scaffold_protocol newmatch -from testmatch
ess::scaffold_info -system match_to_sample
```

## Deleting

| Command | Description |
|---------|-------------|
| `ess::delete_script <type> ?opts?` | Delete one script from registry + local. Accepts `-system` and `-protocol` overrides. |
| `ess::delete_protocol <name> ?opts?` | Delete protocol and all its scripts. Accepts `-system`. |
| `ess::delete_system <name>` | Delete system and all its scripts. |

```tcl
ess::delete_script sys_analyze -system prf
ess::delete_script stim -protocol testmatch
ess::delete_protocol oldproto -system match_to_sample
ess::delete_system obsolete_sys
```

## Libs

| Command | Description |
|---------|-------------|
| `ess::list_libs` | List local lib `.tm` files (name, version, filename). |
| `ess::read_lib <filename>` | Read lib content (checks overlay first). |
| `ess::save_lib <filename> <content>` | Write lib to overlay or base. |

---

## Script Type Names

These are the `type` values used with `commit_script`, `delete_script`, `sync_status`, etc.:

| Type | Maps to | Scope |
|------|---------|-------|
| `system` | `system` / `_` | System-level |
| `sys_extract` | `extract` / `_` | System-level |
| `sys_analyze` | `analyze` / `_` | System-level |
| `protocol` | `protocol` / current proto | Protocol-level |
| `proto_extract` | `extract` / current proto | Protocol-level |
| `stim` | `stim` / current proto | Protocol-level |
| `loaders` | `loaders` / current proto | Protocol-level |
| `variants` | `variants` / current proto | Protocol-level |

The second column shows how the type maps to the registry API path:
`/api/v1/ess/script/{workgroup}/{system}/{protocol}/{type}`

Protocol `_` means system-level (no protocol).

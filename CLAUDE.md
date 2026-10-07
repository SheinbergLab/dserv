# dserv

Real-time datapoint server for experimental control and data acquisition:
a shared datapoint store (the "bus") with Tcl subprocesses attached to it.
It hosts **ESS**, the experiment state system. Sibling repos:
[dlsh](https://github.com/SheinbergLab/dlsh) (`dl_*`/`dg_*`/`dlg_*`),
[stim2](https://github.com/SheinbergLab/stim2) (the subject display), and
[ess](https://github.com/SheinbergLab/ess) (the experiment systems
themselves).

## Read first, by task

| Task | Doc |
|------|-----|
| Writing or changing an ESS system/protocol/loader/variant/stim | `docs/ESS_SKILL.md` |
| Deciding system vs protocol vs variant | `docs/DESIGN.md` |
| dynlist/dyngroup idioms in loaders, stim and extract code | `docs/dlsh_idioms.md` (and `dservctl docs`) |
| A protocol's experimenter view (`set_viz_config`) | `docs/viz_development.md` |
| Registry / dserv.net script sync | `docs/ESS_SYNC_README.md` |
| Setting up a dev machine | `docs/local_systems_setup/`, `docs/pi_setup/` |
| Machines with no network | `docs/OFFLINE.md` |
| How a client finds a dserv (mDNS on-link, registry off-link) | `docs/discovery.md` |

The other files in `docs/` are design notes and plans for specific
subsystems (input layer, extio, transports, settings). Read them when
working in that area.

## Where things live on a dev machine

- **This checkout** is the source of truth for `lib/*.tm`, `config/`
  (including `vizconf.tcl`) and `www/`.
- **The installed copy** is `/usr/local/dserv`: `config/`, `lib/` and the
  `dserv` binary. The running server uses it, not this checkout. Diff
  against it before assuming your change is live.
- **dlsh** loads from `/usr/local/dlsh/dlsh.zip`. A plain `tclsh9.0`
  can use it after `zipfs mount` (see `tools/viztest/viztest.tcl`,
  `load_dlsh`).
- **ESS systems** live in `<system_path>/<project>/<system>/<protocol>/`,
  for example `~/systems/ess/joystick/motiondir/`. Per-user overlays are in
  `<system_path>/overlays/<user>/`. Ask before assuming which copy the
  user wants edited.

## Talking to a running dserv

`essctrl` and `dservctl` (`tools/dservctl`, see `dservctl --help`) talk to
the server on port 2560. The web GUIs are on 2565
(`http://localhost:2565/ess_control.html`).

```bash
essctrl -s ess -c 'dservGet ess/protocol'
essctrl -s ess -c 'getVar ess ::ess::current(system)'
```

- `-s ess` does **not** evaluate inside the ess interpreter. Use
  `getVar ess <var>` to read its variables.
- Dyngroup datapoints such as `stimdg` are binary. Fetch them base64'd:
  `binary encode base64 [dservGet stimdg]`.
- **The dserv you can reach may be driving a live rig.** Reading is fine.
  Don't load or reload systems, change params, or start/stop ESS unless
  the user asks.

## Testing

- **Viz configs:** `tools/viztest`. It runs a protocol's viz offline and
  renders what it draws. See `docs/viz_development.md`.
- **Tcl/C tests:** in `tests/`, registered with CTest
  (`ctest --test-dir build`).
- **Build:** CMake (see `README.md`). Submodules must be initialized.

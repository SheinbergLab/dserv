# Datafiles: .ess → obs → trials

ESS records each session into one `.ess` file (dslog). When the file
closes, dserv's `df` subprocess (`config/dfconf.tcl`, with
`lib/df-2.0.tm`) converts it in two stages:

| stage | makes | how |
|---|---|---|
| 1 | `NAME.obs.dgz` | `df::make_obs`: `dslog::readESS` on the `.ess`, written as is. One row per observation period (BEGINOBS to ENDOBS), with every datapoint logged inside it (`<ds>`/`<dst>`/`<dsn>` columns) and the rest in `<session>` columns |
| 2 | `NAME.trials.dgz` | `df::make_trials`: the system's extractor, `<ess_root>/<system>/<system>_extract.tcl`, plus the protocol's `<protocol>_extract.tcl` when there is one. One row per valid trial, the table dg_viewer and analysis read |

`ess_root` is `<system_path>/<project>`, for example `~/systems/ess`. An
extractor with `::<system>::extract_trials_from_obs` reads the obs DG.
Otherwise `df::load_data` reads the `.ess` again (`::<system>::extract_trials`
on a `df::File`). As of 2026-11 no system has the obs form, so every trials
file comes from the `.ess`.

Both stages write through a hidden `.part_NAME` file and a rename, so a
reader never sees half a file. The temporary name keeps the real
extension because `dg_write` decides by the extension whether to
compress.

After stage 2, df runs the system's analyzer if it has one. It exports
the files if auto-export is on, then deletes the trials file from its
work directory: obs DGs are the cache, and trials are always extracted
again from the current extractor. Work files live in
`<dserv>/work/{obs,trials}` (`/usr/local/dserv/work` when installed).
Exports go to `<export>/{ess,obs,trials}`, and from there to wherever
sessions are kept (`~/data/` on a dev Mac).

## Wide types: `df wide_datafiles`

dslog reads 64-bit datapoints narrowed to 32-bit types unless
`dslog::wideTypes 1` is set. With it off, a 64-bit datapoint keeps its
columns but **loses every value**. The main case is `stim/frame` (int64),
which stim2's frame-locked replay and the render server need. The
setting is `df wide_datafiles`, on by default since dserv 0.60.16. A
file converted with it off has empty `stim/frame` columns. The render
server answers 422 for such a session ("converted with the rig's df
wide_datafiles off"), and dg_viewer's stim2 checkbox reads "stim2: no
clip".

The `.ess` keeps full precision either way, so converting again with
wide types on fixes such a file. Wide DGs need current readers: dlsh
0.17 or later, dgread 1.2.2 or later, and a rebuilt MATLAB MEX and dgview.

## Converting again

**For a session df knows** (one in its catalog, `db/datafiles.db`), use
the **data_manager** page: right-click the file and choose
**Reprocess**. That is `reprocess_datafile` in the df subprocess, which
deletes the work copies and runs both stages again with the rig's
current settings and extractor. The same can be sent by hand:

```bash
essctrl -c 'send df {reprocess_datafile NAME.ess}'
```

```bash
essctrl -c 'send df {reextract_trials NAME.ess}'
```

`reextract_trials` runs stage 2 only, from the obs DG already in the
work directory. `reextract_system SYSTEM` does that for every file of the
system last extracted with an older version of its extractor. Both write into df's
work directory. Copies already exported elsewhere are not touched.

**For files df does not manage** (exported copies in `~/data/obs`,
another rig's files, a machine with no dserv running), use
`tools/dfconvert/dfconvert.tcl`. It runs the same `df::make_obs` and
`df::make_trials`, outside dserv:

```bash
dlsh tools/dfconvert/dfconvert.tcl -obs ~/data/obs -trials ~/data/trials data/essdat/NAME.ess
```

| option | |
|---|---|
| `-obs DIR` | where the obs DGs go (required) |
| `-trials DIR` | also extract trials, into DIR |
| `-trials-only` | stage 2 only, from the obs DGs already in `-obs DIR` |
| `-ess-root DIR` | the extractors' ESS root (default `~/systems/ess`) |
| `-narrow` | wide types off, as df does with `wide_datafiles` off (only to reproduce that) |
| `-lib DIR` | where `df-2.0.tm` is (default this checkout's `lib/`, else `/usr/local/dserv/lib`) |

Existing files are replaced, each only once its new one is written.
Each line reports the obs periods, the `stim/frame` records found inside
them (0 means a narrow conversion), and the trials count. It exits with
status 1 when any file failed. Run it with `/usr/local/bin/dlsh`; the
Mac's `tclsh9.0` has no dlsh.

A session with no valid trials makes no trials file. The extractor may
fail there with an error (planko's ends in `dl_unpack: bad operand`),
just as it does in df.

## Is this file narrow?

```tcl
set g [dg_read NAME.obs.dgz]
expr {[dl_exists $g:<ds>stim/frame] && [dl_length [dl_unpack $g:<ds>stim/frame]] == 0}
```

A result of 1 means a narrow conversion: the frames are in the `.ess`
but not in this DG. stim2's `replay::load` reports the same thing as
`obs_frames_narrow`.

## History

- 2026-11-12: the 28 `human_planko_dropHazard_*_{FIX,FREE}` sessions in
  `~/data/obs` were narrow. They were converted again from
  `data/essdat/*.ess` (77,000 to 140,000 frames each), and their trials
  extracted again with the same rows and columns as before.

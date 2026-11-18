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

## extio streams in a file

Two declarations decide which extio streams a file carries, and
`::ess::file_open` records both:

| who decides | where | covers | if a name is missing |
|---|---|---|---|
| the experiment | `record_streams` param (system → protocol → variant) | analog groups | the file does not open |
| the rig | setting `extio record` (gear → extio, or the **log** column on `extio-config.html`) | analog groups, labelled DI lines, chord groups | the file opens; ESS warns, and the gear's extio light turns red |

Use the rig setting for something wired to the rig rather than needed by
one experiment, such as a photodiode on the screen or a trigger line. Names
are labels: an analog group's name, a DI line's label or a chord group's
name. Use `<box>/<name>` only when two live boxes announce the same name.
extioconf resolves the list against the live boxes into `extio/record`;
`file_open` reads that datapoint and never sends to extio.

- **Analog groups**, from either source, are matched the same way: one
  unbuffered record per block, inside each obs. `df::File
  ain_samples_in_obs` reads them. The number of samples in a block is the
  group's `batch` on the box (`extio-config.html`, analog groups); it is
  not a logging setting.
- **DI lines and chord groups** from the rig list are logged unbuffered
  and not obs-limited, the same as the `tlock din` input: every edge for
  the whole file. Edges between obs land in the obs DG's `<session>`
  columns. For a DI line, the box's `state/sync/*` and `state/in_obs` are
  logged too, so an edge time can be checked against the clock anchor it
  was mapped with.
- **The decode key** is `ess/ain/recorded`. Each analog entry has a
  `source` field (`param`, `rig` or `both`). `extio/record` is also in the
  file: it holds what the rig declared and how each name resolved,
  including names that did not resolve. So a missing photodiode can be
  read as "declared, box absent".

### In the trials DG

A system's extractor writes only the streams it knows by name, and no
extractor knows about a rig's photodiode. So after the system and protocol
extractors run, `df::add_recorded_streams` adds every stream the decode
keys name: the two above, and `tlock/din`, the input a Display++ trigger
output is wired to (setting `tlock din`, logged by `::ess::file_open` when
`tlock enabled` is on). It runs in all three extraction paths: `load_data`,
`load_data_from_obs` and `make_trials`. Rows are picked by the trials'
`obsid`.

| stream | columns | each row |
|---|---|---|
| analog group `G` | `G_t` | ms from obs onset, one per scan (block time + k × interval) |
| | `G_<label>` per channel (`G_ch<N>` if unlabelled) | raw counts, short |
| rig DI line / chord group `N` | `N_t`, `N` | each edge or event inside the obs, and its value |
| Display++ trigger input | `tlock_din_t`, `tlock_din` | the same, whatever the line is labelled |

- `tlock_din` is left out when `extio/record` already added the same
  line under its label. A `tlock/din` that did not resolve when the file
  opened adds nothing.
- Each edge marks the frame that carried a T-Lock packet reaching the
  panel, one refresh after stim2's swap. On the Display++ measured in
  2026-11 (session `..._2611181646`, photodiode at one screen position),
  the first light came 3.08 ± 0.08 ms after the edge. ESS's own stim-on
  event (for example SAMPLE ON) is logged as the swap completes, about 8
  ms (one refresh) before the edge and 11.1 ms before the light.

- `G` gets a `<box>_` prefix only when two boxes' groups share a name.
  Characters other than letters, digits and `_` become `_`.
- A column the extractor already wrote is kept. A system that handles a
  stream itself wins.
- A stream the key names but the obs DG does not carry gets empty lists in
  every row and a line on stdout. The trials stay rectangular, and the
  column still shows that the stream was declared.
- A failure in this step is printed, and the extractor's trials are
  written without the extra columns.
- Old files have no keys and come back unchanged.

`G_t` is as precise as the block times: microseconds with `df
wide_datafiles` on (the default), whole ms with it off. The decoder,
`df::ain_obs_decode`, is shared with `df::File ain_samples_in_obs`, so the
two cannot disagree. `tests/test_df_recorded_streams.tcl` builds a
synthetic obs DG and covers each case.

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

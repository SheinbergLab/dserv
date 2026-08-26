# Camera snapshots in the data path

How an experiment takes full-resolution camera frames during trials and how
those frames (plus other datapoint streams, like extio analog blocks) land
in `.ess` files, obs dgz files, and trials extraction.

## The grab contract (runtime)

The sensor free-runs at `stream_fps` (30) whenever the camera is enabled.
The preview cadence (`camera rate_hz`, default 1/s to `camera/preview`) is
a *watching* knob only — grabs do not depend on it, so there is no reason
to raise it for data collection. `rate_hz 0` (or `set_interval never`)
turns snapshots off entirely: pure acquisition for on-demand grabs, with
`camera/interval` reporting `never` (`grab_last` starves in this mode —
the ring buffer only fills from snapshot frames). Idle cost at 30 fps is
negligible: frames that are neither previewed nor grabbed are requeued
untouched (no copies, no encode; buffer mappings are created once per
configure). Measured on a Pi 5: background acquisition ≈ 3.7% of one
core (libcamera pipeline + IPA threads), +1% for the 1 Hz preview,
~65 ms of one worker core per grab.

A grab is a contract:

1. **Request** — `::ess::camera_grab` (from a state's *action*):
   - stamps `CAMERA REQUEST` (evt 55/0, param = request id),
   - sends `grab_full <req_id>` to the camera subprocess (`sendNoReply`,
     never blocks the state thread).
2. **Capture** — the module services the request with the first frame
   whose exposure *started at or after* the request (`cameraGrabNextFrame`;
   latency ≤ 2 frame periods). The frame is JPEG-encoded on a worker
   thread, off the streaming callback.
3. **Publish** — the JPEG goes **private** to `camera/full`, timestamped
   with the *sensor capture time on dserv's timebase*
   (`SensorTimestamp` ns + `tclserver_clock_epoch_offset_us()`), directly
   comparable with event timestamps. A public JSON completion goes to
   `camera/full/meta`: `{req_id frame_id timestamp_us width height
   jpeg_bytes ok}` (or `ok:0` + `error`).
4. **Resolve** — ESS's `camera/full/meta` callback stamps `CAMERA DONE`
   (or `FAIL`) with the request id and wakes the state machine.

State-system pattern (transitions decide, actions do):

```tcl
$sys add_action  snap { ::ess::camera_grab; timerTick 500 }
$sys add_transition snap {
    if { [::ess::camera_grab_done] } { return next_state }
    if { [timerExpired] } { return next_state }   ;# camera AWOL — don't hang
}
```

The timer backstop matters: if the camera subprocess is absent or wedged,
no meta ever arrives and the trial must not hang on it. `camera_grab_ok`
tells the two resolutions apart when a state cares.

`grab_last` keeps the old ring-buffer behavior (instant, but up to one
preview interval stale, no completion meta) for interactive use.

## Logging

`ess::file_open` matches `camera/full` and `camera/full/meta`
unconditionally (a match nothing publishes costs nothing) and **not
obs-limited**: the async encode can publish a frame just after ENDOBS, and
an obs-limited match would silently drop exactly those frames.

## In the obs dg (`dslog::readESS`)

readESS (dlsh `src/lablib/dslog.c`) now emits:

| column | shape | content |
|---|---|---|
| `<ds>NAME` | n_obs × values | decoded values, concatenated per obs (unchanged) |
| `<dst>NAME` | n_obs × records | per-**record** times, ms from that obs's BEGINOBS (e_times' axis) |
| `<dsn>NAME` | n_obs × records | per-record value counts (split `<ds>` back into records) |
| `<blob>NAME` | n_records | one DF_CHAR byte vector per JPEG/PPM record, whole file |
| `<blobt>NAME` | n_records | per-record capture time, ms from the file's first record |
| `obs_start_ms` | n_obs | each BEGINOBS, ms from the file's first record |

Blob streams are file-level *on purpose*: log position cannot assign an
async-encoded frame to an obs (it may be written after ENDOBS), but its
capture timestamp — plus the REQUEST events — can.

## Extraction (`df::File`)

```tcl
set f [df::File new $filepath]

# camera frames grouped per obs by REQUEST pairing
lassign [$f camera_frames_in_obs] req_t cap_t jpegs
#   req_t: n_obs × requests, ms from obs onset
#   cap_t: paired capture ms (-1 if the grab never resolved)
#   jpegs: n_obs × frames of DF_CHAR bytes (empty when unresolved)
# pairing consumes a frame only for requests with CAMERA DONE evidence,
# so failed grabs never claim a stray (e.g. manual) frame.

# any recorded stream, with per-record times/counts
lassign [$f ds_in_obs em/pupil] vals times counts

# extio analog blocks (record_streams) decoded to per-scan samples
lassign [$f ain_samples_in_obs extio/box02/state/ain/eye] t v
# t: n_obs × scans, float ms from obs onset (block t0 + k*interval_us)
# v: n_obs × channels of DF_SHORT samples
```

Attach to trials with the usual `dl_choose ... $valid_indices`, e.g.
`dl_set $trials:snap_jpeg [dl_choose $jpegs $valid_indices]`.

## Python

`DF_CHAR` reads as int8 in numpy; recover bytes with:

```python
import dgread, numpy as np
d = dgread.dgread('file.obs.dgz')
jpg = np.asarray(d['<blob>camera/full'][k], dtype=np.int8).astype(np.uint8).tobytes()
open('frame.jpg', 'wb').write(jpg)
```

## Fixed on the way (dslog.c)

The readESS overhaul also fixed long-standing defects, verified against
the 471-file corpus (old vs new dumps differ *only* in these):

- JPEG/PPM/JSON payloads silently dropped (stale dtype enum in dlsh's
  `datapoint.h`, frozen at `DSERV_NONE`).
- `DSERV_STRING` streams kept only the **last** string per obs (and leaked
  the rest) — e.g. a mid-obs `em/settings` recalibration vanished.
- `obs_times` wrapped negative past 2³¹ µs (~35.8 min) into a session, and
  per-process `static` anchors corrupted every file after the first in the
  long-lived df subprocess.
- One `ds_datapoint_t` leaked per non-event record (tens of MB per file in
  the df subprocess); `DSERV_DOUBLE` read 8× past its buffer in
  `dslog::read`; empty-obs placeholders were created in the wrong type
  space; the `logger:beginobs`/`endobs` markers grew phantom `<session>`
  columns (colon/slash mismatch).

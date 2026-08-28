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
`camera/interval` reporting `never`. Idle cost at 30 fps is
negligible: frames that are neither previewed nor grabbed are requeued
untouched (no copies, no encode; buffer mappings are created once per
configure). Measured on a Pi 5: background acquisition ≈ 3.7% of one
core (libcamera pipeline + IPA threads), +1% for the 1 Hz preview,
~65 ms of one worker core per grab.

Two grab helpers, one contract. Pick by where the moment of interest is:

| Helper | Use when | Capture vs REQUEST |
|---|---|---|
| `::ess::camera_grab` | The landmark is something you just did (stim on, "snap now") | Always **after**, by ≤ 2 frame periods (~66 ms at 30 fps) |
| `::ess::camera_grab_nearest ?t_us?` | You have a time on dserv's clock (`[now]`, an event stamp) and want the already-buffered frame nearest it | Either side; never waits |
| `::ess::camera_grab_before ms` | The landmark just *ended* — snap what the scene looked like `ms` ago (target selection, lever release) | Typically **before** |

The look-back helpers read history from the ring, which only holds
stream-rate frames when the rig declares the **`camera look_behind`**
setting (below). Both are fire-and-forget from a state *action* — never
from a polled transition — and resolve the same contract:

1. **Request** — the helper stamps `CAMERA REQUEST` (evt 55/0, param =
   request id) and sends `grab_full <req_id>` (or
   `grab_nearest <t_us> <req_id>`) to the camera subprocess
   (`sendNoReply`, never blocks the state thread).
2. **Capture** — `grab_full` is serviced with the first frame whose
   exposure *started at or after* the request (`cameraGrabNextFrame`;
   latency ≤ 2 frame periods). `grab_nearest` picks the buffered frame
   whose capture time is nearest `t_us` — selection and copy in one
   critical section, so a racing ring wrap can't evict the chosen frame.
   Either way the JPEG is encoded on a worker thread, off the streaming
   callback and off the camera interp.
3. **Publish** — the JPEG goes **private** to `camera/full`, timestamped
   with the *sensor capture time on dserv's timebase*
   (`SensorTimestamp` ns + `tclserver_clock_epoch_offset_us()`), directly
   comparable with event timestamps. A public JSON completion goes to
   `camera/full/meta`: `{req_id frame_id timestamp_us width height
   jpeg_bytes ok}` (or `ok:0` + `error` — an empty ring resolves this
   way rather than hanging the trial).
4. **Resolve** — ESS's `camera/full/meta` callback stamps `CAMERA DONE`
   (or `FAIL`) and wakes the state machine. DONE's params are
   `{req_id rel_ms}` — capture minus request, negative for look-back
   grabs — which is what extraction later pairs frames by.

### What a state system adds

**Fire-and-forget (the usual case)** — one line in an existing action.
The contract resolves ~100 ms later on its own; the frame and its
REQUEST/DONE events land in the datafile with no transition changes.
In `match_to_sample`, for example, an opt-in param plus one line in
`sample_on` snaps every sample onset:

```tcl
$sys add_param snap_on_sample 0 variable int
...
$sys add_action sample_on {
    my sample_on
    ::ess::evt_put STIMTYPE STIMID [now] $stimtype
    ::ess::evt_put SAMPLE ON [now]
    if { $snap_on_sample } { ::ess::camera_grab }
    timerTick $sample_time
}
```

Fire-and-forget is safe anywhere the state dwells longer than ~100 ms
(a grab requested right before ENDOBS still works — extraction pairs by
request time, not log position). `camera_grab` also self-resolves as
FAILed if the rig has no camera subprocess, so enabling the param on a
camera-less rig costs nothing.

**Wait-for-contract** — only when the system must *know* the frame was
captured before proceeding (transitions decide, actions do):

```tcl
$sys add_action  snap { ::ess::camera_grab; timerTick 500 }
$sys add_transition snap {
    if { [::ess::camera_grab_done] } { return next_state }
    if { [timerExpired] } { return next_state }   ;# camera AWOL — don't hang
}
```

The timer backstop matters: if the camera subprocess is wedged, no meta
ever arrives and the trial must not hang on it. `camera_grab_ok` tells
the two resolutions apart when a state cares.

`grab_last` keeps the old ring-buffer behavior (instant, but up to one
preview interval stale, no completion meta) for interactive use.

## Look-back snapshots (`camera look_behind`)

`camera_grab_before` answers "what did the scene look like 100 ms before
this event fired?" — e.g. snap the moment of target selection, requested
from the response action *after* the touch landed. That needs history:
the **`camera look_behind`** rig setting (settings gear, or
`settings::put camera look_behind 1 -persist`) parks every stream-rate
frame's DMA buffer in the 16-slot ring — **~500 ms of look-back at
30 fps** — with no per-frame copies or CPU. `rate_hz 0` does not starve
it; preview stays an independent watching knob.

The cost is memory, which is why it's a declared setting and not
always-on: 16 extra ~6 MB DMA buffers per stream at 1080p (~124 MB),
allocated at camera start. On a **Pi 5** the pisp ISP takes these from
ordinary system RAM — rig-verified: the full 16-deep pool allocates at
the stock 64 MB CMA with `CmaFree` untouched, so the setting just
works. On CMA-backed pipelines (Pi 4 / unicam-class) the pool can
exceed stock CMA — the camera still runs and `camera/health` appends
`look_behind unavailable`, or `hold_depth` shrinks to what was granted
— until `/boot/firmware/config.txt` raises it (e.g.
`dtoverlay=vc4-kms-v3d,cma-256`, then reboot). `check_ring_buffer`
reports `hold` (parking active) and `hold_depth` (slots actually
granted). Flipping the setting restarts the stream, since the pool is
sized at configure time.

Measured on a Pi 5 (imx708, 30 fps): a 100 ms look-back returns a frame
within half a frame period of the target, a request older than the
window honestly clamps to the oldest held frame (the meta's
`timestamp_us` shows it), and dserv CPU is unchanged with hold on —
zero-copy means nothing touches a parked frame until a grab asks for
it.

With the setting on, a protocol needs nothing else: the ring is already
parked whenever the camera streams, so `camera_grab_before 100` in a
response action just works. On a rig without the setting (or without a
camera), the helper still resolves — `ok:0` meta, `CAMERA FAIL` — so
enabling a snap param is safe everywhere.

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

### What an extract script adds

The whole block — drop it into any system's `extract_trials`, after
`valid_indices` is computed:

```tcl
if {[$f has_event_type CAMERA] &&
    [$f has_event_occurrences CAMERA REQUEST]} {
    lassign [$f camera_frames_in_obs] cam_req cam_cap cam_jpg
    dl_set $trials:cam_request_t [dl_choose $cam_req $valid_indices]
    dl_set $trials:cam_capture_t [dl_choose $cam_cap $valid_indices]
    dl_set $trials:cam_jpeg      [dl_choose $cam_jpg $valid_indices]
}
```

Per trial that yields: request/capture times (ms from obs onset;
capture `-1` where a grab never resolved, and possibly *negative* for a
look-back grab requested just after BEGINOBS) and a list of JPEG byte
vectors (empty where unresolved). The double guard keeps old files
quiet and skips files where no grab was ever requested.

Pairing is exact, not positional: each CAMERA DONE carries `rel_ms`
(capture minus request), so a request claims the unused frame nearest
`request_time + rel_ms` — within half a frame period, else it reads as
unresolved rather than claiming a neighbor. That stays correct when two
grabs land close together, when a look-back frame *predates* its
request, when a frame is logged after ENDOBS, and it never lets a
failed grab or a manual console grab's stray frame cross-claim. Files
from before `rel_ms` (single-param DONE) pair by the old
first-frame-at-or-after-request rule, which was exact for next-frame
grabs.

### Other streams

```tcl
# any recorded stream, with per-record times/counts
lassign [$f ds_in_obs em/pupil] vals times counts

# extio analog blocks (record_streams) decoded to per-scan samples
lassign [$f ain_samples_in_obs extio/box02/state/ain/eye] t v
# t: n_obs × scans, float ms from obs onset (block t0 + k*interval_us)
# v: n_obs × channels of DF_SHORT samples
```

## Python

`DF_CHAR` reads as int8 in numpy; recover bytes with:

```python
import dgread, numpy as np
d = dgread.dgread('file.obs.dgz')
jpg = np.asarray(d['<blob>camera/full'][k], dtype=np.int8).astype(np.uint8).tobytes()
open('frame.jpg', 'wb').write(jpg)
```


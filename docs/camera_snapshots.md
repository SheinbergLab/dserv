# Camera snapshots in the data path

How an ESS state system takes full-resolution camera frames during trials
and how those frames land in `.ess` files and trials extract. Aimed at an
agent adding snaps to a protocol — edit whatever tree ESS loads on the
rig (`ESS_SYSTEM_PATH` / the lab overlay).

Camera JPEGs are **private**: they go to log files and nowhere else.
Explorer, `dservGet`, websocket subscribe, and `/camera.html` cannot show
the payload. **Do not put images in trialsync.** Frames belong in the
`.ess` logger and the local trials / obs dgz.

## Which helper

Pick one. All are fire-and-forget from a state *action* (never from a
polled transition). They stamp `CAMERA REQUEST` immediately and resolve
later with `CAMERA DONE` or `FAIL`. The JPEG is private on `camera/full`;
public completion is `camera/full/meta`.

| Helper | Use when | Capture vs REQUEST |
|---|---|---|
| `::ess::camera_grab` | The landmark is a command you just issued (stim on, “snap now”) | Always **after** REQUEST, by at most about one frame period (~33 ms at 30 fps) plus encode |
| `::ess::camera_grab_nearest $t_us` | You have a time on dserv’s clock (`[now]`, an event stamp) and want the already-buffered frame nearest that time | May be **before or after** REQUEST; does not wait |
| `::ess::camera_grab_before $ms` | Convenience: nearest to `[now] − ms`. Circles target-select uses **100** | Typically **before** REQUEST |

The sensor must already be **streaming** (`camera/status` is `continuous`).
`camera_grab` does not need the ring. `camera_grab_nearest` / `_before`
need `set_ring_fill 1` **before the trial** so ~16 stream-rate frames
are kept (~500 ms at 30 fps). Empty ring still resolves (`ok:0` meta +
`CAMERA FAIL`) so the trial does not hang.

Preview (`camera rate_hz`, default 1/s to `camera/preview`) is a watching
knob only. Data collection uses `rate_hz 0` (no preview JPEGs). That does
not starve look-behind as long as ring fill is on. Fill is cheap; leaving
it on for the open file is the right default when any trial might call
`camera_grab_before`.

The ess interp registers **`sendNoReply`**, not `send_noreply`. Prefer
the `::ess::camera_grab*` helpers. A `catch` around the wrong name
swallows the error and you get a normal-looking session with **zero**
JPEGs.

Saving Tcl is not enough: **reload** the system/protocol/variant so
in-memory actions pick up the helpers and params. New camera commands
live in the camera subprocess — restart dserv (or re-source
`cameraconf.tcl` there) after changing those.

## Wire it into a protocol

### Fire-and-forget (usual)

One line in an existing action. The contract resolves on its own; juice
and trial timing stay put. Safe anywhere the state dwells longer than
~100 ms (a grab requested right before ENDOBS still works — extract pairs
by request time, not log position). Enabling the param on a camera-less
rig is fine: the helper self-resolves as FAIL.

```tcl
$sys add_param snap_on_sample 0 variable int
...
$sys add_action sample_on {
    my sample_on
    ::ess::evt_put SAMPLE ON [now]
    if { $snap_on_sample } { ::ess::camera_grab }
    timerTick $sample_time
}
```

If the landmark is a *transition* predicate (a dpoint flipped, a complete
flag), do **not** put the grab in the transition — it would fire every
poll. Insert a one-tick state whose action does the grab, then go on
immediately.

### Start the sensor with the datafile

Grabs only hit disk while the logger is matching. `camera enabled`
defaults to 0: the subprocess is up, the sensor is idle, and
`camera_grab` fails with `"Camera not initialized"`.

Start once per file, not per trial (`start` is init + configure +
stream). Use blocking `send camera start` on open; `sendNoReply camera
stop` on close. Only stop what this protocol started, so a gear-enabled
`/camera.html` preview is left alone.

Set `stream_fps` and `set_rotation` **before** `start`. Rotation is
0/90/180/270 clockwise (mounting). `rate_hz` / `apply_capture_hz` is
live; **0** is the data-collection setting. Look-behind also needs
`set_ring_fill 1`.

Worked example: `search` / `circles` snaps ~100 ms before target
selection. The system’s `response` action calls a no-op hook; the
protocol overrides it. File open/close owns the sensor:

```tcl
# search.tcl — fire-and-forget from the one-shot response action
$sys add_action response {
    set resp_time [now]
    ::ess::evt_put RESP 1 $resp_time
    my on_target_select
}
$sys add_method on_target_select {} {}

# circles.tcl
$s add_param snap_on_target 1 variable int
$s add_param snap_before_ms 100 variable int
$s add_param camera_stream_fps 30 variable float
$s add_param camera_rotation 90 variable int
$s add_param camera_rate_hz 0 variable float
$s add_variable camera_started_for_file 0

$s add_method on_target_select {} {
    if { $snap_on_target } { ::ess::camera_grab_before $snap_before_ms }
}

$s set_file_open_callback {
    set camera_started_for_file 0
    if { $snap_on_target } {
        set st ""
        catch { set st [dservGet camera/status] }
        if { $st ne "continuous" } {
            send camera [list set stream_fps $camera_stream_fps]
            send camera [list set_rotation $camera_rotation]
            send camera start
            catch { set st [dservGet camera/status] }
            if { $st eq "continuous" } { set camera_started_for_file 1 }
        }
        if { $st eq "continuous" } {
            send camera [list apply_capture_hz $camera_rate_hz]
            send camera {set_ring_fill 1}
        }
    }
}
$s set_file_close_callback {
    catch { sendNoReply camera {set_ring_fill 0} }
    if { $camera_started_for_file } {
        catch { sendNoReply camera stop }
        set camera_started_for_file 0
    }
}
```

Do not put `camera_grab` in `wait_for_response`’s transition — that is
polled every tick. The `response` action fires once.

### Wait-for-contract (rare)

Only when the system must *know* the frame was captured before
proceeding. Always a timer backstop: if the camera subprocess is wedged,
no meta arrives and the trial must not hang.

```tcl
$sys add_action  snap { ::ess::camera_grab; timerTick 500 }
$sys add_transition snap {
    if { [::ess::camera_grab_done] } { return next_state }
    if { [timerExpired] } { return next_state }
}
```

`grab_last` is interactive only (newest ring frame, possibly stale, no
REQUEST/DONE). Do not use it in a protocol.

## Logging

`ess::file_open` already matches `camera/full` and `camera/full/meta`
(not obs-limited: encode can finish just after ENDOBS). Do **not**
logger-match `camera/preview` — that is the watching stream and would
fill the datafile.

## Did it work?

`dservGet camera/full` always says private, even on success. Check the
**`.ess` file** (about one full-res JPEG per grab; REQUEST/DONE in the
eventlog) or `cam_request_t` / `cam_capture_t` / `cam_jpeg` after
extract.

| In the `.ess` | Meaning |
|---|---|
| no CAMERA REQUEST | the state never called a grab helper (not reloaded, param off, action never entered) |
| REQUEST, no DONE/FAIL | send reached the camera interp, but meta never came back |
| REQUEST + FAIL (FAIL may be in `e_pre`, after ENDOBS) | `camera/full/meta` had `ok:0`. `"Camera not initialized"` means the sensor was idle. Extract: `cam_capture_t` `-1`, empty `cam_jpeg` |
| REQUEST + DONE, no `<blob>camera/full` | logger match / private publish |
| REQUEST + DONE + blob, capture **after** REQUEST by tens of ms | `camera_grab` (next-frame) working |
| REQUEST + DONE + blob, capture **before** REQUEST by ~`snap_before_ms` | `camera_grab_before` working |

`dservGet camera/status` should be `continuous` before the first grab.
Live, test the same command ESS sends. `camera/full/meta` is public:

```tcl
send camera {grab_full 1}
dservGet camera/full/meta
```

Look-behind (ring must already be filling):

```tcl
send camera {set_ring_fill 1}
# wait ~0.5 s
send camera [list grab_nearest 1 [expr {[now] - 100000}]]
dservGet camera/full/meta
```

`timestamp_us` should sit ~100 ms before the request. That proves the
camera contract only, not the logger or extract.

## Extraction (`df::File`)

Drop this into the system’s `extract_trials` after `valid_indices`:

```tcl
if {[$f has_event_type CAMERA] &&
    [$f has_event_occurrences CAMERA REQUEST]} {
    lassign [$f camera_frames_in_obs] cam_req cam_cap cam_jpg
    dl_set $trials:cam_request_t [dl_choose $cam_req $valid_indices]
    dl_set $trials:cam_capture_t [dl_choose $cam_cap $valid_indices]
    dl_set $trials:cam_jpeg      [dl_choose $cam_jpg $valid_indices]
}
```

Per trial: request/capture times (ms from obs onset; capture `-1` if
unresolved) and JPEG byte vectors (empty if unresolved). Each DONE
request claims the unused blob whose capture time is **nearest** the
request, so a look-behind frame still pairs. FAIL never claims a stray
frame.

**Use DF_CHAR, not DF_STRING** for JPEG bytes (Python `dgread` can abort
on overlong NUL if they went through Tcl UTF-8).

readESS keeps JPEGs at file level (`<blob>camera/full`, `<blobt>camera/full`)
because a frame can be logged after ENDOBS. Prefer `camera_frames_in_obs`
or the trials columns over walking the raw `.ess`.

## Python

```python
import dgread, numpy as np
d = dgread.dgread('file.trials.dgz')
arr = d['cam_jpeg'][0][0]   # trial 0, first frame
jpg = np.asarray(arr, dtype=np.uint8).tobytes()
assert jpg[:3] == b'\xff\xd8\xff' and jpg[-2:] == b'\xff\xd9'
# also: d['cam_request_t'][trial], d['cam_capture_t'][trial]
```

`UnicodeDecodeError` on load usually means the column was stored as
DF_STRING instead of DF_CHAR.

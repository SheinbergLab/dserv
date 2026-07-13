# extio-setup

Bench and rig tool for extio boxes: flash firmware, configure pins/groups,
save/apply dump profiles, and watch/drive a box live — from a browser.

A small Go server bridges the browser to a box through one of two drivers:

- **USB serial** (v1) — the box's USB-CDC console, found by USB identity
  `2e8a:100b` (console = first CDC interface, never "highest ttyACM").
  Full config read via `dump`, CLI writes, interactive console, `.uf2`
  flashing, dump profiles. For a box on your bench with nothing else attached.
- **dserv** — a rig's dserv datapoint table (text port 4620). Pick a box from
  `extio/boxes`, see its live pin map from the announced manifest, watch DI
  edges, and write config/commands via `%set extio/<box>/config|cmd/…`.
  For any box on a machine running dserv.
- **combined** (serial console + "events via dserv" checkbox) — the box has
  two USB CDCs: a *console* and a *data* interface. On a rig, dserv's `usbio`
  owns the *data* CDC but leaves the *console* free. Combined mode uses the
  console for config/CLI/diagnostics (full `dump`/`show`) and takes events
  from dserv instead of opening the contended data CDC. Best of both on a dev
  box: full config readback **and** real dserv-timestamped events, no
  contention.

The UI is embedded, so the shipped binary is self-contained.

## Which driver?

The serial driver assumes it is the **sole owner** of the box's USB CDC. On a
rig, dserv's `usbio` module already holds the *data* CDC — and macOS does not
reliably enforce exclusive access, so a second reader silently splits the
frame stream and both sides lose events (the tool detects this and says so,
then falls back). So:

- **Bare bench box** (nothing else attached): serial — console *and* events
  off the box's own data CDC.
- **Box on a machine running dserv:** either dserv (no USB at all — subscribes
  over TCP, so it never contends), or **combined** if you also want the
  console for `dump`/`show`/diagnostics and direct CLI. Combined opens only
  the free console CDC; events come over dserv. Do **not** run plain serial
  against a dserv-owned box — that's the data-CDC collision.

## Run

    go run .                    # localhost:2569, opens your browser
    extio-setup -http :2569    # LAN mode (rig helper): reachable from other machines
    extio-setup -open=false   # don't launch a browser
    extio-setup -fw DIR       # local firmware images (default: auto-detect wiznet-io/dist)
    extio-setup -shelf URL    # firmware shelf for "pull from dserv.net" (default https://dserv.net; "" disables)
    extio-setup -dev          # serve web/ from disk (UI development)

If an instance is already running, a second launch just opens a browser tab
to it and exits.

## Provisioning flow

1. Plug in a blank board (it mounts as the BOOTSEL drive) or a flashed box
   (it enumerates as `extio USB box`).
2. Flash: pick a `.uf2` (a **local** dist image, or one from the **dserv.net**
   shelf), the tool sends `bootsel` if needed, copies the image, and waits for
   re-enumeration. Shelf images are downloaded and **sha256-verified against
   the published manifest before flashing** — a byte mismatch aborts, never
   flashes. When a box is connected, the shelf row shows "update available" if
   the box's `fw` differs from the channel's latest.
3. Configure: click pins on the map, or **Apply profile** to replay a saved
   `dump` (per-line OK/ERR checking; prompts for a new box name so cloning
   a profile doesn't duplicate identity).
4. **Save to flash** persists; **Save profile** downloads the current dump.

## Watch/configure a box over dserv

1. Switch the connection type to **dserv**, enter the rig host (default
   `localhost`), **Connect**. The tool `%reg`s a binary connect-back
   subscription on `extio/*` and seeds retained state.
2. Pick a box from the list (populated from `extio/boxes`). Its pin map,
   labels, and obs/sync pins come from the box's announced manifest; DI edges
   and heartbeats stream in live.
3. Click a pin to set mode/label/obs/sync, or the box name/desc — writes go
   out as `%set extio/<box>/config/…`. Commands (Test pulse, Save, Reboot) go
   as `%set extio/<box>/cmd/…`. The box re-announces after a config change and
   the UI reconverges on it.

Over dserv the box announces mode and labels only, so `pulse_us`, `debounce`,
`active_low`, and the in vs in_pullup distinction are **write-only** (blank a
field to leave it as-is); use the serial driver for the full picture. There is
no interactive CLI console over dserv. Config writes are unsaved until **Save
to flash**.

## Notes

- The pin map's reserved/claimed pins are the W6300-EVB (dual image) table
  from `wiznet-io/PINMAP.md`; ain/oled claims follow the live config.
- The API is plain JSON + SSE. Serial: `/api/connect`, `/api/dump`,
  `/api/console`. dserv: `/api/dserv/{connect,disconnect,state,set}`. Both
  feed one `/api/events` stream (decoded 128-byte datapoint frames). See
  `api.go`, `serial.go`/`data.go` (serial), `dserv.go` (dserv driver).
- Both drivers decode the same 128-byte `>` frames: the serial data CDC
  carries them directly; dserv pushes matched keys as the identical binary
  frames (`%reg … 1`), so `parseFrame` and the event pipeline are shared.
- Fleet view (all boxes on a rig at once) can build on the dserv driver; today
  it shows one selected box.

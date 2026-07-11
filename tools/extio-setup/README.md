# extio-setup

Bench and rig tool for extio boxes: flash firmware, configure pins/groups,
save/apply dump profiles, and talk to the box console — from a browser.

A small Go server bridges the browser to the box's USB-CDC console (found by
USB identity `2e8a:100b`, console = first CDC interface — never "highest
ttyACM"). The UI is embedded, so the shipped binary is self-contained.

## Run

    go run .                    # localhost:2569, opens your browser
    extio-setup -http :2569    # LAN mode (rig helper): reachable from other machines
    extio-setup -open=false   # don't launch a browser
    extio-setup -fw DIR       # firmware images (default: auto-detect wiznet-io/dist)
    extio-setup -dev          # serve web/ from disk (UI development)

If an instance is already running, a second launch just opens a browser tab
to it and exits.

## Provisioning flow

1. Plug in a blank board (it mounts as the BOOTSEL drive) or a flashed box
   (it enumerates as `extio USB box`).
2. Flash: pick a `.uf2`, the tool sends `bootsel` if needed, copies the
   image, and waits for re-enumeration.
3. Configure: click pins on the map, or **Apply profile** to replay a saved
   `dump` (per-line OK/ERR checking; prompts for a new box name so cloning
   a profile doesn't duplicate identity).
4. **Save to flash** persists; **Save profile** downloads the current dump.

## Notes

- The pin map's reserved/claimed pins are the W6300-EVB (dual image) table
  from `wiznet-io/PINMAP.md`; ain/oled claims follow the live config.
- The API is plain JSON + one SSE stream (`/api/console`); see `api.go`.
- dserv-driver mode (configure via `extio/<name>/config/*` datapoints,
  fleet view) is planned; serial is the v1 transport.

# extio-zephyr cheatsheet — build, flash, publish, OTA

Everything here is copy-pasteable for the FRDM-MCXN947 (`box01` as the
example). Long-form reasoning lives in PORTING.md; this is the crib sheet.

---

## Build

West lives in the zephyrproject venv and must run from inside that workspace:

```sh
source ~/zephyrproject/.venv/bin/activate
cd ~/zephyrproject
```

Incremental (the usual case — build dir remembers board and sysbuild):

```sh
west build -d ~/src/dserv/extio-zephyr/build-mcxn-ota
```

From scratch (new build dir, or after `-p always` wiped the cache):

```sh
west build -b frdm_mcxn947/mcxn947/cpu0 ~/src/dserv/extio-zephyr --sysbuild \
    -d ~/src/dserv/extio-zephyr/build-mcxn-ota
```

The OTA-able artifact is
`build-mcxn-ota/extio-zephyr/zephyr/zephyr.signed.bin`.
`build-mcxn947` (no sysbuild) is BUILD-ONLY — flashing it erases MCUboot.

**Version**: `extio-zephyr/VERSION` → the MCUboot image version (`image:
v0.4.0+2` on the banner, `state/fw_ver` in dserv). Bump `VERSION_TWEAK` for
every cut you might publish — two different binaries with the same version
number is exactly the ambiguity the file exists to prevent. A VERSION change
sometimes needs a pristine build (`-p always`, then the from-scratch command)
to reach the signer.

## Patches (after every `west update`)

`west update` erases the carried fixes in `~/zephyrproject/zephyr`. Reapply:

```sh
cd ~/zephyrproject/zephyr
git apply ~/src/dserv/extio-zephyr/patches/*.patch
```

Current set: `enet-qos-rx-fixes` (OTA fatal-bus-error + RX timestamp race —
without it OTA and PTP both regress), `ksz8081-retry-mdio`,
`ptp-mgmt-ds-packing`, `ptp-transport-send-errno` (boot-log noise),
`mcxn947-cpu1-no-board-hook` (cpu1 must not re-run board clock init — the
board Kconfig `select`s the hook for both cores, and a late-released cpu1
re-running it switches MAIN_CLK to FRO12M over a LIVE cpu0, killing
Ethernet + the 1588 clock; without this patch the dual-core image bricks
networking at cpu1 release).
`patches/superseded/` is history — never apply it.

## SWD flash (first install / recovery — OTA is the normal path)

Over the onboard MCU-Link, one USB-C to J17 (power + console + flash):

```sh
pyocd list
```

gives each board's probe UID (box01 ≈ `RCOJ4PBDEHHGK`). Then:

```sh
PYOCD_PROJECT_DIR=~/src/dserv/extio-zephyr west flash \
    -d ~/src/dserv/extio-zephyr/build-mcxn-ota --domain extio-zephyr \
    -r pyocd --dev-id RCOJ4PBDEHHGK
```

```sh
PYOCD_PROJECT_DIR=~/src/dserv/extio-zephyr pyocd reset -t mcxn947 -u RCOJ4PBDEHHGK
```

* `PYOCD_PROJECT_DIR` makes pyocd find `pyocd.yaml`, which pins the DFP pack
  to 19.0.0 — the newest pack (26.06) dies mid-erase and half-bricks the
  board. Without the env var the pin is silently not in effect.
* `--domain extio-zephyr` flashes the app slot only and never touches
  MCUboot's region.
* The reset matters: `pyocd flash` doesn't reliably restart the core.
* Serial console afterwards: `/dev/cu.usbmodem<UID>3`, needs DTR asserted
  (pyserial `s.dtr = True`; bare `cat` reads nothing).

## Publish to the firmware shelf

```sh
cd ~/src/dserv/extio-zephyr
sh publish.sh -n
```

Dry run first — check the version line and the bin path. Then for real:

```sh
sh publish.sh
```

* Needs `DSERV_AGENT_FIRMWARE_TOKEN` in the environment (it's in `.zshrc`,
  so interactive shells have it).
* Version defaults to `git describe` on the tree. If that string is already
  on the shelf (second publish from the same dirty tree), pass a distinct
  one: `sh publish.sh -v 0.49.11-dirty.2`. The shelf rejects `+` in
  versions, and the box picker takes the NEWEST publish that has this
  board's image — so a fresh publish always wins regardless of the string.
* The script verifies the uploaded sha against the local file; trust that
  line, not the upload's exit code.

## OTA a box

All commands run against the box's dserv (the host in the box's
`dserv=…:4620` config). Stage the newest shelf image:

```sh
dservctl extio "extio_ota_push_shelf box01 dev"
```

(pin a version with a third arg: `… box01 dev 0.49.11-dirty.2`). Watch
`extio/box01/state/ota/ack` climb to the image size; `state/ota/state=ok`
means the transfer sha matched. Then the lifecycle, one datapoint each:

```sh
dservctl extio "dservSet extio/box01/cmd/ota/verify 1"
```

Read-back hash + MCUboot-header check. Want `state/ota/flash_verify=1`,
`hdr_ok=1`, and `staged_ver` naming the version you meant to send.

**Check the AGE, not just the value** (`extio_ota_status box01` prints one
per field). These three are retained, so the previous update's `flash_verify=1`
survives the next transfer intact — same 1, same `hdr_ok`, and a `staged_ver`
still naming the OLD version. Observed 2026-08-11 on boxa: a completed fetch
sitting next to verify keys 36 minutes old. `extio_ota_push_shelf` now clears
them before the first byte moves, and `extio_ota_arm` refuses a read-back that
predates the transfer — but a bare `dservSet cmd/ota/arm 1` goes straight to the
box and gets neither. Prefer `extio_ota_verify` / `extio_ota_arm` /
`extio_ota_confirm`, which also surface the box's refusal reason.

```sh
dservctl extio "dservSet extio/box01/cmd/ota/arm 1"
```

`state/ota/arm=armed`, then the box reboots itself into ONE trial boot (the
boot is slower — MCUboot copies the swap). Arm refuses rather than guesses:
`refused_no_verified_image`, `refused_no_image_header`, `refused_in_obs`.

```sh
dservctl extio "dservSet extio/box01/cmd/ota/confirm 1"
```

Send only after the box re-registers and looks alive — the command arriving
at all is the proof that matters (a "publishing but deaf" image can't ack
it). `state/ota/confirm=confirmed`, `trial→0`, `updates` +1.

**"Did the trial take?" is answered by `state/boot`, not by `fw_ver`.**
`boot=trial` means the armed image is running unconfirmed; `revert` means it
ran and wasn't kept; `rejected` means MCUboot declined it. `state/ota/trial=1`
says the same from the bootloader flag rather than the persisted breadcrumb.
Do NOT compare `fw_ver` against `staged_ver` for this: `fw_ver` is the MCUboot
header version and **nothing bumps it between dev commits** — every dev image
is `0.4.0+100` — so equal proves nothing, and a box whose header isn't where
the tooling looks reports the literal string `unreadable`, which matches no
staged version at all. Both were seen on box3, 2026-09-01;
`extio_ota_lc_booted` used to fail the lifecycle on the second one and skip the
confirm, leaving a good image to revert.

**To reject a trial**: don't confirm — power-cycle. The old image returns by
construction and the box reports `revert` in `state/ota/*`.

**Gotchas**

* Never push the image the box is already running — MCUboot rejects the
  swap (`state/ota/rejects` +1). Bump VERSION, rebuild, publish.
* Between arm and confirm, ANY reset reverts. That's the safety, not a bug.
* An OTA holds the analog sampler; it self-releases ~1 s after the last
  chunk, no action needed.

## Where the running version shows

* Boot banner: `image: v0.4.0+2, confirmed` (or `trial`).
* dserv: `dservctl extio "dservGet extio/box01/state/fw_ver"` — the version
  MCUboot actually booted, re-announced on every connect. (`state/fw` is a
  build-time constant and can NOT show an OTA taking effect.)
* Staged-but-not-armed: `state/ota/staged_ver`, published by `cmd/ota/verify`.

## Battery (BLE peripherals — XIAO nRF52840 only)

* `batt` on the console, or `state/batt/mv` / `batt/pct` / `batt/raw` from dserv
  (one reading a minute, `health` level, changed-only). The fleet card shows
  `batt 3.87 V (62%)` with its age.
* **`pct` is a nominal curve, `mv` is the measurement, `raw` is the truth.** The
  divider values in `boards/xiao_ble.overlay` came from Seeed's Arduino variant,
  NOT a schematic — verify against a meter before trusting mV, then fix
  `output-ohms`/`full-ohms`. A wrong ratio moves mV and leaves `raw` alone; a
  wrong enable polarity rails `raw`.
* **On USB you are reading the charger, not a resting cell.** Right for
  calibration (the meter sees the same node), useless as a gauge.
* Why it exists, and what it means for `ble latency`: `wiznet-io/BLE.md` §Power.

## DAC (v0.4.0+18, MCXN947 only)

The wire-contract face of the DAC that `adccal` exercises from the console:

```
dservctl -c "dservSet extio/box01/cmd/dac/0 2048"    # counts 0..4095 (clamped)
```

* Echo `state/dac/0` = the APPLIED code; errors on `state/dac/err`
  ("no such channel" — only ch 0 exists; "not ready").
* Capability-check `state/dac_en` in the manifest (0 on RP2350/teensy builds)
  instead of probing a command that would silently no-op.
* Immediate-set only, by design — amplitude testing, not waveform timing.
  DAC0_OUT is box pin 1 (D1); jumper it to an analog input (extio_test uses
  D1 -> A1) for a mid-scale ADC certification path.
* Silicon caveat: codes below ~700 clamp at ~517 mV (documented in
  PORTING.md) — certify the floor, don't pretend it isn't there.

## Analog throughput budget (what "feasible" means; 2026-08-11)

For a continuous group: base rate **B** Hz, decimate **D**, batch **K**,
channels **N** (per group), union channels **U** (all groups):

    scan rate   G  = B / D                 (scans reaching the group)
    batch_eff   K' = min(K, floor(24 / N)) (24 = AIN_BLOCK_MAX int16 slots;
                                            batch silently shrinks to fit ONE
                                            128-B frame -- ain_group_batch_eff)
    block rate  F  = G / K'                (frames/s this group puts on the wire)

Every stage and its failure mode -- all COUNTED, none silent, but counted
is not the same as delivered:

    stage               budget                   overrun symptom
    sampler (coop -2)   B x sweep(U) < ~60% CPU  st_late (ain/dbg/late)
                        sweep(U) ~ 20 + 12xU us    [>= +61 the saturation
                        (LPADC, polled)            guard yields; earlier fw
                                                   SEIZES THE BOX -- the +59
                                                   bench wedge]
    on-change groups    200 blocks/s ceiling     ain/dbg/throttled
                        (AIN_MAX_BLOCKS_PER_S;     (continuous mode is EXEMPT
                        noise-driven mode only)     by design -- 2026-08-06)
    ain_q               64 blocks burst          st_dropped (ain/dbg/dropped)
    pub bulk queue      96 frames burst          bulk_dropped (dbg/pub_*)
    eth uplink          ~140 us/frame peak       backs up into bulk queue
                        (dbg/send_us); plan
                        SUM(F) <= ~500/s sustained

Feasibility rules of thumb (MCXN947 LPADC):
  * B <= ~5000/U-channels-worth: 6 ch union at 10 kHz is OVER budget (the
    wedge config); 6 ch at 1-2 kHz is comfortable.
  * Per group keep F = (B/D)/K' at or under ~250/s (the proven eye feed);
    lift K (batching) before lifting F -- batch is the tool that buys rate.
  * If G/floor(24/N) still exceeds ~500/s the config is infeasible at ANY
    batch: raise D (decimate) or drop channels.
  * After any analog edit, read ain/dbg/{late,dropped,throttled} + dbg/pub
    drops for a minute. Zero across the board = lossless in fact, not hope.

NOT YET ENFORCED at config time: the firmware clamps batch to the frame and
throttles on-change mode, but an infeasible continuous (rate, decimate,
batch) is accepted and bleeds into the counters. Planned: firmware publishes
per-group effective numbers (G, K', F, feasible) so extio-config.html can
show red at edit time, and refuses applies that break the sampler budget.

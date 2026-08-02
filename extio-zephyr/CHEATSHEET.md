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

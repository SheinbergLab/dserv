# OTA.md — extio firmware update design notes

**Status: design notes only (2026-07-09). Not started.** The interim manual
procedure below covers today's small fleet; this document exists so the real
project starts from decisions, not archaeology. Companion history: README.md
(identity/fw publishes), PINMAP.md (pin budget), TESTING.md (harnesses).

## Interim procedure (works today, USB-accessible boxes)

```sh
# 1. put the box in BOOTSEL -- picotool -f can NOT force it (we own TinyUSB
#    with custom descriptors; there is no picoboot reset interface), which is
#    exactly why the firmware has a bootsel command. Either:
#      console:    bootsel
#      datapoint:  dservSet extio/<name>/cmd/bootsel 1
# 2. the board re-enumerates as an RP2350 boot device, then:
picotool load wiznet-io/dist/wizchip_dserv_config_dual.uf2
picotool reboot
# 3. config survives (persist sector untouched by image flash); verify with
#    `show` or the fleet page (state/fw carries the new git-describe version).
```

Limitation that motivates real OTA: an Ethernet-deployed box in BOOTSEL is a
USB device with nobody plugged into it. Headless eth boxes need self-update.

## Goal

Releases published to the central dserv.net repos; boxes learn an update is
available, download it to the inactive flash slot, install, and prove
themselves — with rollback on any failure and zero risk to a running session.

## Model: tethered peripheral, not autonomous device (2026-07-12)

The framing that drives every decision below: an extio box is **never alone**.
It is always the peripheral end of some system — USB-CDC to a host running
dserv, or Ethernet/WiFi to a dserv on the LAN. "Traditional" OTA is built for
devices that *are* alone (intermittently online, deciding for themselves when
to update, phoning a cloud endpoint over TLS). Almost every one of those
assumptions inverts here — so we keep the safety machinery and drop the
autonomy:

| Traditional OTA assumes | extio reality | Verdict |
|---|---|---|
| Device decides *when* to update | The connected peer knows when it's safe (ess state) | **drop** autonomous timing |
| Device pulls from the internet (TLS/DNS) | Box never touches the internet (principle 1) | **drop** cloud-pull |
| Huge fleet, staged waves | Small lab fleet | **simplify** to canary-then-rest |
| A/B + power-fail atomicity | Same need | **keep** (RP2350 bootrom, free) |
| TBYB + health-check + auto-rollback | Same need | **keep** |
| Image integrity (sha256 / signing) | Same need | **keep** sha; signing = open Q |
| Delta updates to save bandwidth | ~300KB over a LAN | **drop** — not worth it |

**Control plane vs data plane.** The autonomy that traditional OTA puts *in the
device* lives instead in the **connected peer**, which is better placed to hold
it (it knows ess state, the whole rig, the target version):

- **Control plane = the peer** (host for USB, dserv-agent for Ethernet): decides
  *when* and *what*. Compares the box's reported version to the channel target,
  gates on `ess/in_obs`, picks canary order, issues `cmd/ota/begin <version>`.
  The box **never self-initiates** — no autonomous polling.
- **Data plane = the box**: one transport-agnostic state machine
  (`STAGING → VERIFY → ARMED → self-test → buy|rollback`, below). Dumb, safe,
  identical everywhere.

**This is desired-state reconciliation**, and it closes the loop with the
firmware shelf (`dserv-agent/firmware.go`):

- **desired state** = the channel pin on the shelf (`stable/latest.json`)
- **observed state** = the box's `state/build` + `state/fw` + `state/board`
  (published in `publish_ident`; `build` is the exact image match key, `board`
  the compatibility filter — see dserv-agent/README.md "board matrix")
- **reconciler** = the agent (eth) / host (USB), ess-gated; the fleet page
  `www/extio.html` is the rollout console.

**The transport asymmetry that "always connected" forces.** Being connected
removes the need for autonomous *decisions* but NOT for an on-box flash
*mechanism* — because one partner is headless and boxes can't be reflashed
from outside without USB:

| Topology | Broker | How bytes reach the box | On-box A/B |
|---|---|---|---|
| USB-CDC to a host | the host (`extio` subprocess / extio-setup) | host **pushes** `cmd/ota/chunk` over the CDC frame channel, box acks | optional (BOOTSEL works today; A/B is non-disruptive) |
| Ethernet (W6300) | dserv-agent | box **pulls** length+sha frames from the agent's LAN staging socket | **required** (headless — a BOOTSEL box is a USB device with nobody plugged in) |
| WiFi | dserv-agent | same as eth | required, **deferred** (bigger images, no `copy_to_ram`) |

A USB box has no IP and sits *behind* its host → the host pushes over CDC. An
eth box has no host → it pulls from the agent. **One state machine, two feed
directions** — unavoidable, and fine: in both cases bytes come from the box's
*local* peer (which fetched them from the shelf), so the box still never
touches the internet. BOOTSEL/picotool does not disappear — it **demotes to
physical last-resort recovery**, never the normal route.

**Recommendation (2026-07-12):** build the universal on-box A/B updater as the
one mechanism for all transports, and **prove it on the USB bench first** —
where BOOTSEL is still a safety net while the risky machinery earns trust —
before a deployed eth box depends on it. The split-path alternative
(BOOTSEL-for-USB, A/B-only-for-eth) means two update paths to test forever.

## Stage 0 — delivery proof (converged 2026-07-12, transport co-designed with David)

Prove the image crosses the dserv link correctly BEFORE the A/B/TBYB lift. No
partition table, no boot switch — receive → scratch flash → verify sha → report.

**Transport = box PULLS via dserv's raw binary get, NOT pub/sub.** The pub/sub
push path is hard-capped at 128B (`SendClient::send_dpoint` →
`DPOINT_BINARY_FIXED_LENGTH`; `dpoint_to_binary` into a 127B buffer, over-size
→ returns 0, not sent). But dserv's command port has a raw binary get:

```
request : '<' + varlen(u16 LE) + varname            (Dataserver.cpp:2541)
reply   : size(int32 LE) + dpoint_to_binary(varlen+varname+ts+type+datalen+data)
```

`dpoint_to_binary` copies `data.buf` **raw — no base64** (unlike the `@get` /
`dsGetDG` text path, which base64s via `dpoint_to_string`; qpcs.tcl `dsGetDG` is
the base64 reference, deliberately NOT used here — raw streams to flash cleaner).

**Flow:**
1. **Orchestrator = the `extio` subprocess** (`config/extioconf.tcl`, in-process
   in dserv → `dservSet` a raw binary datapoint with no wire-size cap, no
   base64). Stages the image as `extio/<box>/ota/image`, then fires
   `cmd/ota/begin` (small: sha256 + size, fits the frame, rides pub/sub).
2. **Box** on `cmd/ota/begin` (dispatched in `on_frame` before `dserv_dispatch`):
   opens a transient W6300 socket (SN6/SN7 free; SN5 pattern), sends the `<`
   request, reads `size`, skips the dpoint header, **streams `datalen` bytes
   straight to a scratch flash region** (sector-by-sector via `pico_flash.h`;
   never buffers 150KB in SRAM), runs `pico_sha256` (RP2350 hardware) over it,
   publishes `state/ota/{state,progress,result}`.

**Measured constraints (probe 2026-07-12):** images ~150KB; RP2350 hw SHA256
present (`pico_sha256`); scratch region = a spare span below the persist sector
(no PT yet). New box primitive: `box_net_get_binary(key, sink_cb)`. This is
Stage 0 only — Stage 1 adds the A/B slots + TBYB the pulled image then feeds.

**STATUS: Stage 0 RIG-VALIDATED end-to-end 2026-07-12** on box `pico` (W6300-EVB,
eth, dserv=rpi500). Positive: `extio_ota_push pico <120KB>` → staging→100%→**ok**,
on-box hw-SHA256 == host `sha256 -file`, image auto-cleared, **box stayed alive
through the multi-second RT stall** (the `g_wdt_gate=0` guard held — no wedge).
Negative: wrong expected sha → **fail/sha_mismatch** (verify is real). All pieces
(`box_net_get_binary` + vtable, `pico_ota.h` sink/sha, on_frame `cmd/ota/begin`
hook + core-0/core-1 flash handshake, `extioconf` `extio_ota_push`) proven on
silicon. The bytes-crossing-the-wire half is DONE; Stage 1 is the boot lifecycle.

## Stage 1 — A/B slots + TBYB (converged 2026-07-12, bootrom APIs verified in SDK)

Turn the delivery proof into a *bootable* update: the pulled image lands in the
**inactive slot**, boots **once** on trial, and becomes permanent only if it
proves itself. Every failure path converges on "old image boots." **No custom
bootloader** — the RP2350 bootrom does slot selection and rollback itself.

**Reuse from Stage 0 (unchanged):** `box_net_get_binary` · `pico_ota.h` sink+hw
SHA · the **core-0/core-1 flash handshake** (core 1 can't `flash_safe_execute`
under `ASSUME_CORE1_SAFE`) · `extioconf` orchestrator · `state/ota/*` publish.
Stage 1 only changes **where** we write (scratch → inactive slot) and adds the
arm/reboot/buy lifecycle around it.

**Bootrom mechanics (all verified present in the vendored pico-sdk):**
- `rom_load_partition_table` + `rom_get_partition_table_info` — read the PT, find
  our A/B pair and the **inactive** partition's storage base + size at runtime.
- `rom_reboot(BOOT_TYPE_FLASH_UPDATE | NO_RETURN_ON_SUCCESS, delay, inactive_base, 0)`
  — reboot preferring the just-written slot (`p0` = start of updated region).
- **TBYB**: an image built with the try flag boots once; `rom_explicit_buy(workarea,
  size)` commits it. `rom_get_sys_info(SYS_INFO_BOOT_INFO)` →
  `tbyb_and_update_info & BOOT_TBYB_AND_UPDATE_FLAG_BUY_PENDING` tells the new
  image it's on trial. Rollback is AUTOMATIC: buy never called → next reset (our
  Stage-3 watchdog, or power) reverts to the other slot. **The watchdog we already
  ship IS the rollback trigger — no new mechanism.**
- `copy_to_ram` (already on) → the image runs from SRAM, position-independent, so
  **one uf2 runs in either slot**; we flash the same image to whichever is inactive.

**Flash layout — migration target (4MB pico2):**
```
0x000000  partition table (unpartitioned front)
0x004000  slot A   ~512KB   (image ~150KB -> 3x margin)
0x084000  slot B   ~512KB
0x104000  spare/data ~2.9MB
0x3FF000  persist sector (4KB) -- OUTSIDE both slots, reserved in the PT
```

**Fresh-card / provisioning (the real ergonomic cost):** an A/B card NEEDS a PT
for the bootrom to know the slots, so the naive "drag one bare image onto the
BOOTSEL drive" no longer yields a working A/B card (it just recreates the old
single-image-at-flash-start layout — boots, but no rollback). Softeners:
1. Drag-and-drop isn't lost — only the "just the image" shortcut is. A UF2 can
   carry a partition table (its own family id; see `boot/uf2.h`
   RP2350_ARM_S=0xe48bff59, DATA=0xe48bff58), and picotool can **merge PT +
   slot-A image into ONE factory uf2** → provisioning stays a single drag of a
   different file. (Exact `picotool partition create … --embed` recipe = a
   bench-confirm item, but it's standard picotool.)
2. It's ONE-TIME: after the first partition+provision, every update is OTA — no
   drive, no picotool, never touch the box.
3. Recovery unchanged: BOOTSEL + drag a factory uf2 is always the last resort.
4. Keep both worlds: a dev/bench box can stay no-PT single-image (drag a plain
   build exactly like today); only OTA-enrolled boxes get partitioned.

**Build-side changes:**
- **Versioned image** — embed a monotonic version (picobin `1BS_VERSION`, item
  0x48) so a clean boot prefers the newer valid slot. **Default: version from
  `git rev-list --count`** (monotonic int).
- **TBYB flag** — the image carries the try flag for boot-once. **Default:
  always-TBYB + always-buy** — one uniform code path (bench flashes self-test and
  buy too, so a wedging bench flash self-heals the same way a bad OTA does),
  rather than maintaining separate bench vs OTA artifacts.
- **PT artifact** — a `partition_table.json` → `picotool partition create` → PT
  uf2 (the migration payload; or bundled into the factory uf2 above).

**Firmware (new module, ~300 lines, reg-machine-shaped):**
- **Slot resolver** (`pico_ota_slot.h`): at boot, PT-info → which slot we booted,
  its A/B pair, the inactive slot's base+size. Publish `state/ota/slot` (A|B).
- **Retarget the writer**: `pico_ota_begin` takes a runtime `base`+`size` (today
  `PICO_OTA_SCRATCH_OFFSET`/`_BYTES` are compile constants) → point at the inactive
  slot. Same core-0 `flash_ota_erase/program` handshake, whole-slot span.
- **Arm + reboot**: `pico_ota_finish==ok` → publish `state/ota/state=armed` →
  `rom_reboot(FLASH_UPDATE, ~500ms, inactive_base, 0)`.
- **Buy / self-test SM** (runs on the NEW boot): if BUY_PENDING, self-test window
  — transport up + `have_dserv_target` + dserv client connected + **registration
  acked** + `g_rt_beat` advancing for **N s (default 20)** → `rom_explicit_buy` →
  `state/ota/result=bought`. Never satisfied / wedge → watchdog → bootrom reverts
  → old image publishes `state/boot=watchdog` + unchanged `state/fw` (the
  visible-failure contract). CRITICAL ORDER: never buy before self-test passes;
  the watchdog window must be long enough for a healthy image to buy first.
- **Boot-type reporting**: extend `state/boot` with trial/buy-pending/bought so the
  fleet page narrates a trial vs a committed image.

**Orchestrator/dserv (minimal):** `extio_ota_push` ≈ unchanged (box decides slot +
lifecycle); the orchestrator just watches `armed → (reboot) → bought | fail` and
surfaces the trial on the fleet page. Optional `cmd/ota/abort` before arm.

**Chosen defaults (David: "no strong opinions; rollback is the win"):**
always-TBYB+always-buy · 512KB slots · version = `git rev-list --count` · persist
stays raw-offset but PT-reserved. All vetoable later; none block rollback. (These
resolve the "slot size" + close part of the Open-questions list below.)

**Bench-first (J-Link box, before any fleet box; see Adversarial bench checklist):**
1. PT create + `picotool partition info`; **confirm real flash size / aliasing at
   PT creation** (write a marker near the claimed top, read back).
2. A→B OTA of a GOOD image → self-test → buy → runs B.
3. A→B OTA of a WEDGING image → watchdog → **bootrom reverts to A** +
   `state/boot=watchdog` (the money test).
4. Power-cut mid-write → old slot still boots.
5. Then migrate + trial on rig `pico`.

**Rollback invariant:** only the inactive slot is ever written; the new image must
EARN permanence (self-test → explicit buy); any failure — bad sha, wedge, power
loss, failed self-test — leaves buy uncalled → bootrom boots the old slot. Config
persist lives outside both slots, never touched.

## Principles (agreed 2026-07-07/09)

1. **Boxes never touch the internet.** dserv-agent is the broker: it polls the
   dserv.net release repo, verifies sha256, stages artifacts, serves them over
   plain LAN TCP. (No TLS/DNS/HTTP client on the box.)
2. **Never mid-session.** The agent gates rollout on ess state (`ess/in_obs`,
   system stopped) — the box never decides update timing alone.
3. **Safe by construction.** Only the inactive slot is ever written; the new
   image must *earn* permanence (TBYB + self-test + explicit buy); every
   failure path converges on "old image boots."
4. **Config survives.** The persist sector stays outside the A/B slots and the
   blob is versioned append-only (v12→v14 already proved forward-compat).

## RP2350 mechanics (why this is cheap here)

The bootrom natively supports **flash partition tables** with **A/B image
slots**, version-aware boot selection, and **TBYB (try-before-you-buy)**: an
image written with the try flag boots exactly once; unless the running
firmware calls the explicit-buy bootrom API, the next reset reverts to the
other slot. Composition with existing machinery:

- **Self-test = what the box already checks**: transport up, dserv connected,
  registration acked, core-1 heartbeat advancing for N seconds → buy.
- **Rollback detector = Stage-3 watchdog**: a wedged try-image reboots in
  BOX_WDT_MS and the bootrom falls back; `state/boot=watchdog` + `state/fw`
  make the revert visible on the fleet page.
- **copy_to_ram**: the whole image runs from SRAM, so core 0 can stage-write
  the inactive slot while core 1 keeps capturing DI edges (same contract as
  `save`).

## Flash layout — PROBED 2026-07-12 on the rig box `pico` (W6300-EVB)

Measured via `picotool info -a` / `partition info` (bootsel-over-dserv, picotool
on the box's USB host `.50`, no dialout/sudo needed — plugdev udev rule):

- **Image ~150 KB** (binary `0x10000000`–`0x100255e0` = `0x255e0`), NOT the 300KB
  earlier guess. Half-size → slot pressure is a non-issue.
- **No partition table** ("there is no partition table") → the one-time
  migration reflash is confirmed necessary.
- **Physical flash = 2 MB** (picotool `flash size: 2048K`), NOT asserted by OTP.
  **FIXED 2026-07-13:** the W6300-EVB targets (`dual`/`w6300`) now build with
  `-DPICO_FLASH_SIZE_BYTES=2097152` (build.sh `EVB_FLASH`), so persist
  (`FLASH_STORE_OFFSET = PICO_FLASH_SIZE_BYTES − 4K`, pico_flash.h) resolves to
  **0x1FF000** outright instead of 4 MB's 0x3FF000 QSPI-aliasing down to it. Same
  physical sector as the old alias → **deployed boxes keep their persisted
  config across this change.** Plain Pico 2 (`usb`) is genuinely 4 MB, unchanged.
- Image type `ARM Secure` (signed image-def block), `secure boot: 0`.

Layout at 2 MB (images are only ~160 KB, so slots are roomy):

```
0x000000  partition table (~8 KB)
0x002000  slot A   512KB  (partition id 0; committed base, non-TBYB)
0x082000  slot B   512KB  (partition id 1, linked to A; OTA trial, TBYB)
          spare    ~1MB   (unpartitioned; future staged assets/logs)
0x1FF000  persist sector (4KB, last sector, OUTSIDE the slots = FLASH_STORE_OFFSET)
```

This is exactly what `wiznet-io/pt.json` + `provision.sh` write (see "Provisioning
a box" below). **pico2w** images (cyw43 firmware, no copy_to_ram) are much larger
and those boards are 4 MB+ — size them before promising OTA on WiFi boxes;
deferring pico2w OTA is acceptable.

## Box updater state machine (~300–400 lines, module like the reg machine)

```
IDLE -> STAGING   chunks/fetch written to inactive slot (core-0 flash ops,
                  save-queue pattern; erase-as-you-go, progress datapoint)
     -> VERIFY    sha256 over the staged slot vs manifest; mismatch -> IDLE
     -> ARMED     mark slot try-bootable, publish intent, reboot
     == try boot: self-test window (transport+reg+beat, N sec) -> explicit buy
                  else watchdog -> bootrom revert -> old image publishes
                  state/boot=watchdog + unchanged state/fw (visible failure)
```

All phases publish `state/ota/state` + `state/ota/progress` so the fleet page
narrates the rollout. Commands ride the existing channel: `cmd/ota/begin
<version|manifest>`, chunks, `cmd/ota/abort`.

## Transports — UNIFIED on the dserv datapoint channel (2026-07-12)

Original plan had two delivery methods (eth pulls from an agent socket, USB
gets pushed chunks). **Unified: push `cmd/ota/chunk` over the dserv datapoint
channel for ALL transports.** The box already receives every command
(`cmd/pulse`, `cmd/bootsel`, config writes) as datapoints over whatever
transport it's on — USB CDC frames or the W6300 TCP link are the *same* 128-byte
frames to the box's parser. So firmware chunks ride the identical channel:

- **All transports**: `cmd/ota/chunk` datapoints carry offset+crc+payload
  (~109B of the 128-byte frame); box writes the inactive slot and acks via
  `state/ota/ack <offset>`; the orchestrator paces on acks. ~300KB ≈ 2800
  frames. One delivery path in the box, transport-agnostic — no dedicated TCP
  socket, no staging HTTP endpoint, no box-side TCP client.
- **Orchestrator** = whoever can `dservSet extio/<box>/cmd/ota/*` on the dserv
  the box is registered with AND holds the image bytes: the rig's dserv-agent,
  a dserv-side proc, or extio-setup's dserv driver. It fetches + sha-verifies
  from the shelf, then relays chunks. **The box never touches the internet** —
  the orchestrator does; the box only sees LAN datapoints.
- **Integrity**: per-chunk crc catches a dropped/garbled frame (orchestrator
  re-sends that offset); the whole slot's sha256 is verified at VERIFY before
  ARM. A push that stalls times out and aborts (old image untouched).
- **Efficiency note**: datapoint-push is chattier than a streamed pull (2800
  individual sets vs one fetch). Fine on a LAN, gated to `!in_obs`. If churn
  ever bites, a box-initiated pull over a spare W6300 socket remains a drop-in
  alternative for the eth path — but start with the unified push; it reuses
  everything.
- **Recovery of last resort** (all transports): physical BOOTSEL button →
  `picotool load`. Never removed by any of this. Also the ONE-TIME migration
  path: a box with no partition table needs a single USB/BOOTSEL flash to a
  partition-aware, updater-carrying image; after that it's OTA-over-dserv.

## USB OTA — implemented ('D'-frame chunk-push); FULL CHAIN PROVEN + RELIABLE (2026-07-14)

Ethernet boxes PULL the image (transient socket, `box_net_get_binary`). A USB box
has no socket, so the host PUSHES it as dedicated **'D' frames** over usbio — not
named datapoints (no name overhead / no datapoint-table churn) and not a variable
frame (keeps the fixed-128 framer + resync untouched). It's a swappable delivery
front-end feeding the SAME `pico_ota` sink / slot / verify / TBYB machinery as eth.

- **Wire format (128B):** `[0]='D'` (`DSERV_OTA_CHAR`, a 2nd framer start-marker)
  `[1..4]` seq_off u32 LE · `[5..6]` len u16 LE · `[7..10]` crc32 u32 LE (== the
  box's `pico_crc32`, i.e. zlib) · `[11..127]` data (≤117). Strictly sequential:
  `seq_off` must == the sink cursor; per-frame crc catches desync early, whole-image
  sha is the final gate; box acks `state/ota/ack <contiguous-offset>`.
- **Pieces:** `common/dserv_msg.h` (framer accepts 'D'), `pico/wizchip_dserv_config.c`
  (`ota_data_frame` + `ota_usb_service_core1` + `cmd/ota/begin` branch on
  `OTA_IS_USB()`), `modules/usbio/usbio.c` (`usbioSendChunk <seq_off> <bytes>`),
  `config/extioconf.tcl` (`extio_ota_push` branches `state/transport=="usb"`).
- **PROVEN:** the box committed an image end-to-end over USB — `staging → D-frames
  → verify → armed → self-test PASSED (RT+dserv+reg 8s) → explicit_buy COMMITTED`.
  On a plain Pico 2 (4 MB), which also validated 4 MB slot-boot.

**Two host bugs found + fixed:**
1. **`begin` must be sent synchronously.** `dservSet cmd/ota/begin` reaches the box
   via `usbio_forward`, a `dpointSetScript` that is **event-loop deferred**; the
   blocking blast then starves the loop, so `begin` arrived AFTER the blast (box
   dropped every frame with `active=0`). Fix: `usbioSendFrame extio/<box>/cmd/ota/begin
   0 "<sha> <size>"` — written synchronously, so the box stages first.
2. `usbioSendChunk` was mis-called with a stray box arg (it takes `<seq_off> <bytes>`).

**RESOLVED (2026-07-14) — there was NO transfer bug; the USB OTA worked the whole
time. The "tail stall" was a TELEMETRY illusion. Proven by J-Link.**

- **"143208" was never a failure point** — it's just the **last periodic ack**
  (`OTA_ACK_EVERY=4096`; the next ack would be past EOF, so `state/ota/ack` parks
  there for the whole tail regardless of what happens). The box streams the *entire*
  image every run.
- **J-Link (halt + read, box alive) settled it in one look:** mid-transfer
  `g_ota.received == g_ota_size` and `g_ota.state == PICO_OTA_DONE_OK` (sha verified);
  the last boot showed `g_boot_type=FLASH_UPDATE`, `g_boot_trial=1`, `g_ota_bought=1`,
  `g_ota_buy_rc=0`. i.e. **transfer → verify → arm → trial → self-test → buy → COMMIT
  all succeeded.**
- **Why it LOOKED stuck (the real bug = observability):** (1) finalize's terminal
  publishes (`armed`/`ok`) are single sends from the RX path and got **dropped** when
  the TX FIFO was momentarily full (the ack only survives by being re-sent every
  frame); (2) base and trial shared a **version**, so `state/fw` never changed on
  commit; (3) `state/boot` only knew power/soft/watchdog, so a **FLASH_UPDATE boot read
  as `power`**; (4) the trial's re-enumeration drops the USB console — which *is* what
  a successful arm looks like, and is what the early "dead console" reports actually
  were. Net: nothing that would show success was visible, even though it succeeded.
- **Fix = make the outcome observable (`wizchip_dserv_config.c`):**
  - `publish_ota_str_n()` — re-send a terminal OTA state N× so at least one lands.
  - finalize emits `armed`/`ok` (and `fail`) reliably.
  - **`state/ota/state`/`result` = `committed`** announced from **core 1** once the
    trial self-tests + buys (`g_ota_bought && buy_rc==0`; core 0 must not touch the
    CDC). This is THE definitive success signal, visible even when versions match.
  - `state/boot` now reports `trial` (FLASH_UPDATE buy-pending) / `update` (committed
    FLASH_UPDATE) instead of `power`.
  - (Kept from the earlier pass: finalize no longer holds `g_wdt_gate=0`, and
    `dserv_framer_reset()` at begin — harmless robustness, not the actual fix.)
- **Verified live 2026-07-14:** OTA of a distinct-version trial → dserv showed
  `staging → committed`, `result=committed`, `state/boot=trial`, `state/fw` flipped to
  the new version, and the commit **survived a normal reboot** (didn't roll back).
- **Method note:** the whole hunt was prolonged by trusting dropped/stale dserv
  telemetry as ground truth. Once the box was reachable-but-"stuck", **J-Link
  halt+read of `g_ota.*` / the boot-info globals** was the move that ended the
  guessing — datapoints can't see core 0 or a value that never got published.

**Debugging/bench lessons (worth keeping):**
- **`bootsel` over CDC0** (`dservSet extio/<box>/cmd/bootsel 1`, or write `bootsel\r\n`
  to `/dev/cu.usbmodemXXX1`) drops the box into BOOTSEL with **no physical button** —
  as long as the console is alive. This made a fully-automated flash-and-test loop
  possible (`picotool load -p 0/-p 1` both slots → `reboot` → push → `dservGet`).
- **A/B slot targeting bit us for hours:** `picotool load <uf2>` (no `-p`) lands in the
  **inactive** slot and never boots; `-u` (flash-update boot) or an explicit `-p 0`
  into the active slot is required, and a fresh partition table only goes resident
  after `reboot -u` (what `provision.sh` does). Also don't flash a `usb`-build uf2 onto
  a `dual`-build box — check `state/build` first.
- **Observability under load:** a one-shot datapoint published from the RX path
  (`on_frame`) can be silently dropped when the TX FIFO is momentarily full; the ack
  survives only because it's re-sent every frame. Publish diagnostics from the RT
  service loop (like `ota/state`) or re-send N× if you need them reliable.

**dserv-subprocess visibility lesson (drove the orchestrator design):** a subprocess
cannot observe an async datapoint update (usbio injects the ack on another thread)
from **inside a blocking command** — a mid-command `dservGet` is stale even with
`dservAddMatch` + a `vwait`-pump. So the tail-resender is **event-driven**
(`dpointSetScript` on `state/ota/ack`, debounced, resend from the stuck cursor).
**A blocking `dservWait <key> <predicate> <timeout>`** — implemented in C to service
the datapoint ingestion while it blocks — would replace both the sync-`begin` dance
and the event-driven resender with linear code and is worth building.

**Host-side push bounding (2026-07-17, after the J-Link incident wedged dserv):**
a push against a device that vanishes before/mid-blast must abort, never park.
The incident shape: box dropped off USB (J-Link rescue-reset strand), the blast's
per-chunk `write_all` EAGAIN guard (~400 ms, the normal PACING mechanism) ×4275
chunks = ~28 min with the extio subprocess — and dserv's whole command port —
wedged. Fixed in layers, all rig-tested (pre-vanished push + mid-blast bootsel):
- `usbio.c`: `usbioSendFrame/usbioSendChunk` now raise TCL_ERROR on a HARD write
  error (EIO/ENXIO — device detached); EAGAIN-exhaustion stays a short-count
  return (it's indistinguishable from a slow drain; the caller decides).
- `extioconf.tcl`: every 128-byte send is checked (`extio_ota_usb_write128`,
  compatible with old short-count and new error modules); the blast aborts on
  `!usbioAlive` or 3 consecutive failed chunks; a **progress-checked 10 s no-ack
  deadline** (armed after the synchronous blast, re-armed per ack, verifies the
  cursor actually stalled before killing) catches the SILENT shape; aborts
  publish `state/ota/state=fail result=host_io` host-side (the box republishes
  truth when it returns) and free the staged image.
- **macOS zombie-fd findings (why the deadline is the real guard there):** writes
  into a detached-but-open cu.usbmodem fd SUCCEED (kernel swallows, ~250 KB/s)
  and poll() delivers NO POLLHUP until the driver instance is torn down at
  RE-enumeration — so `usbioAlive` lags the vanish on macOS (it is prompt for
  ttyACM on the Linux/Pi deployment hosts) and short-write aborts may never
  trigger. The deadline bounds every shape: worst observed abort ≈ 25 s
  (re-arm chain across queued pre-vanish acks), vs 28 min before. dserv command
  port stayed 26–32 ms responsive through a mid-blast vanish in the rig test.
Deploying the C half needs the rebuilt `dserv_usbio.dylib` + a dserv restart;
the Tcl layer alone (hot-patchable) already bounds everything.

## Agent + release side

**Status update 2026-07-12:** the agent-side *shelf* is now implemented
(`dserv-agent/firmware.go`, `--firmware-dir`): versioned `.uf2`+`.bin` per
channel (`dev` mutable / `stable`+`extio-fw-vN` immutable, `-dirty` refused off
`dev`), server-computed sha256, `manifest.json` contract, open read endpoints
(`/api/firmware/extio…`, `/firmware/extio/…`) + token-gated multipart publish.
See dserv-agent/README.md for the API.

**Status update 2026-07-13 — shelf→OTA path wired (publisher + dserv-side consumer):**
The whole release loop is now `build.sh <t> --tbyb --push` → one call per box.

- **Publisher** (`build.sh`): a `--tbyb` build now copies its sealed flat `.bin`
  (the OTA slot image) to `dist/` and, on `--push`, publishes it with `ota=1`
  alongside the `.uf2` — so the manifest entry carries `OtaCapable`+`Bin`+
  `BinSHA256`. A plain build still ships only the bench-flash `.uf2`. No agent
  change was needed (publish already accepted `bin`/`ota`).
- **Consumer** (`config/extioconf.tcl` → `extio_ota_push_shelf <box> ?channel?`):
  resolves the channel's `latest`, picks the image whose `build` == the box's
  announced `state/build` (with a `state/board` compat guard), pulls its `.bin`,
  sha-verifies against the manifest, and hands off to `extio_ota_push`
  (stage + `cmd/ota/begin`). Shelf URL is `::extio_fw_shelf_url` (default
  `https://dserv.net`; point at a rig-local agent to OTA offline).
- **Binary-safe HTTP** (`src/TclHttps.cpp`): the built-in client's read loops did
  `rawResponse += buffer` (C-string append) — truncating any body at its first
  null byte. Fixed to `.append(buffer, n)`, and added `https_get -outfile <path>`
  (writes raw bytes to a file, returns the byte count) so a firmware `.bin` pulls
  intact without Tcl's UTF-8 re-encoding. Requires a dserv new enough to carry
  both this and the new `extioconf` (they ship together in `config/`).

  Interim consumer for pre-partition boxes stays as below: extio-setup pull →
  BOOTSEL flash. The on-box updater's flat-`.bin` fetch is the same shelf artifact.

## Release signing & publishing — procedure

The publish path takes two secrets, both passed via env, never committed:

- `SIGN_KEY` — path to the **secp256k1** ECDSA private key that signs images
  (`pico_sign_binary`). secp256k1 is mandatory: the RP2350 bootrom verifies only
  that curve — NOT P-256/ed25519.
- `DSERV_AGENT_FIRMWARE_TOKEN` — Bearer token for the shelf publish endpoint
  (`--push`). Read side is open; only publish is gated.

Optional: `FW_SHELF_URL` (default `https://dserv.net`), `PUSH_CHANNEL` (default
`dev`; `stable`/`extio-fw-vN` are immutable and refuse `-dirty`), `OTA_FWVER`
(overrides the `state/fw` label for a distinguishable test image — NOT the
picobin A/B version).

**The signing key today is a throwaway bench key.** For a real release key:

1. **Generate (secp256k1, once, carefully):**
   ```sh
   openssl ecparam -name secp256k1 -genkey -noout -out extio-release.pem
   # or passphrase-encrypted at rest:
   openssl ecparam -name secp256k1 -genkey -noout | openssl ec -aes256 -out extio-release.pem
   ```
2. **Store it OUTSIDE the repo** — password manager (1Password) or an encrypted
   volume — and **back it up in two places**. Losing it after secure boot is
   enabled means you can never sign an update again. `.gitignore` blocks
   `*-release.pem`/`*-signing.pem` (root) and every `*.pem` under `wiznet-io/`
   as a backstop, but the key should not live in the tree at all.
3. **Reference by path at build time:** `SIGN_KEY=/path/to/extio-release.pem`.
   Dev builds can stay unsigned; only release builds set `SIGN_KEY`.

**Signature is ADVISORY until RP2350 secure boot is enabled.** We proved
`rom_explicit_buy` commits an *unsigned* image (secure boot off), so signing
today only gives image *consistency*, not enforced authenticity. Enabling secure
boot burns the public-key hash into **OTP — one-way, brick-risk, and a key hash
can't be cleanly removed once in OTP.** DECISION (current): stay LAN-only with
sha256 integrity enforced + sign for consistency; do NOT burn OTP yet. Revisit
only if boxes ever fetch off-LAN or the threat model adds a hostile LAN/agent
(see "Open questions" → image authenticity vs integrity). If secure boot is ever
adopted: generate the release key first, back it up, then rehearse the OTP burn
on a sacrificial board before any deployed hardware.

**Release command (per build target):**
```sh
export SIGN_KEY=/path/to/extio-release.pem
export DSERV_AGENT_FIRMWARE_TOKEN=…            # from the shelf host's /etc/dserv-agent/env
sh build.sh dual --tbyb --push                 # signed TBYB image + flat .bin -> shelf (dev)
# stable release: sh build.sh dual --tbyb --push --channel stable   (clean tree only)
```
Then, per box on its dserv:
```sh
dservctl extio "extio_ota_push_shelf <box>"    # latest dev; add a channel arg to pin
```

- build.sh gains a `release` mode: picotool-packaged artifacts (partition-
  aware UF2 for manual loads + flat .bin per slot for the updater) + manifest
  JSON (`{version, build, board, variant, size, sha256}`); version = existing
  git describe.
- Publish uploads the artifacts + manifest to the agent's **local firmware
  shelf** (`--firmware-dir` on the dserv.net box, `firmware.go`) — NOT GitHub.
  The shelf holds the actual `.uf2`/`.bin` bytes on the server's own disk; the
  box (and the bench tools) fetch them over the LAN, never the internet. (This
  is distinct from `releases.go`, which only caches GitHub *metadata* for the
  `.deb` components — that path is unrelated to firmware.)
- Agent: watch the shelf → compare each box's reported `state/build`+`state/fw`
  against the channel pin → rollout per policy (pin versions, canary one box,
  manual approve vs auto), gated on ess state. Fleet page = the rollout console.

## Provisioning a box (the one awkward step) — self-contained recipe

A box (blank, or running a pre-partition image) needs ONE manual reflash to lay
down the A/B partition table + slot images. After that, updates are OTA — no more
BOOTSEL. `wiznet-io/provision.sh` automates this, **but you don't need it** — the
whole thing is the copy-paste block below (only `picotool ≥ 2.1` + the repo). Do it
on the bench over USB; **BOOTSEL always recovers**, so a mistake is never fatal.

```sh
cd wiznet-io                                           # all paths below are repo-relative

# --- 1. build the two SIGNED slot images (signing also HASHES them; slot-boot needs the hash) ---
#     Use the target that matches the BOARD: `dual` = W6300-EVB (2 MB). A plain Pico 2 box uses `usb`.
#     Do NOT flash a usb-build image onto a dual box (or vice-versa) -- check `state/build` first.
export SIGN_KEY=/path/to/extio-bench.pem               # any secp256k1 key (sig is advisory; box isn't secure-boot):
                                                       #   openssl ecparam -name secp256k1 -genkey -noout -out extio-bench.pem
sh build.sh dual                                       # -> dist/wizchip_dserv_config_dual_signed.uf2       (slot A, committed base)
sh build.sh dual --tbyb                                # -> dist/wizchip_dserv_config_dual_tbyb_signed.uf2  (slot B, trial/TBYB)

# --- 2. put the box in BOOTSEL (no physical button needed if the console is alive) ---
#     connected box:  dservctl set extio/<box>/cmd/bootsel 1   (or `bootsel` on the CDC0 console)
#     blank board:    hold BOOTSEL while plugging in USB
picotool info                                          # confirm ONE RP2350 is in BOOTSEL before continuing

# --- 3. write the partition table, then make it RESIDENT (the bootrom's copy is stale until `reboot -u`) ---
picotool partition create pt.json /tmp/extio-pt.uf2
picotool load /tmp/extio-pt.uf2
picotool reboot -u
sleep 3 ; until picotool info >/dev/null 2>&1; do sleep 1; done   # wait for it to re-appear in BOOTSEL

# --- 4. load the slots: A = committed base, B = trial (order/`-p` matter; a plain drop can't target a slot) ---
picotool load dist/wizchip_dserv_config_dual_signed.uf2      -p 0
picotool load dist/wizchip_dserv_config_dual_tbyb_signed.uf2 -p 1

# --- 5. boot the app ---
picotool reboot
```

The box reconnects to dserv, now OTA-capable; set name / mode / pins as usual.
Verify: `picotool partition info` shows `0(A)` + `1(B w/ 0)`. Future updates are
over-the-air: `dservctl extio "extio_ota_push_shelf <box>"`.

### …or pull the slot-A image straight from dserv.net (no local build)

`provision.sh --from-shelf` does steps 1 + 3–5 for you — it resolves the channel's
latest, **downloads the slot-A base uf2 and sha256-verifies it against the manifest**,
then runs the same picotool sequence (slot B is left empty; the first OTA fills it):

```sh
# box in BOOTSEL first (dservctl set extio/<box>/cmd/bootsel 1)
cd wiznet-io
./provision.sh --from-shelf                       # channel dev, build dual, latest
./provision.sh --from-shelf --channel stable --build dual --version 0.48.0   # explicit
```

Or grab it by hand and feed it to the manual recipe above (`… -p 0`):

```sh
SHELF=https://dserv.net ; CH=dev ; BUILD=dual
read VER FILE SHA < <(curl -fsS "$SHELF/api/firmware/extio" | python3 -c '
import sys,json; d=json.load(sys.stdin); ch,b=sys.argv[1],sys.argv[2]
c=d["channels"][ch]; v=c["latest"]; m=next(x for x in c["versions"] if x["version"]==v)
i=next(x for x in m["images"] if x["build"]==b); print(v,i["file"],i["sha256"])' "$CH" "$BUILD")
curl -fsS -o "/tmp/$FILE" "$SHELF/firmware/extio/$CH/$VER/$FILE"
echo "$SHA  /tmp/$FILE" | shasum -a 256 -c -       # aborts on mismatch; then: picotool load /tmp/$FILE -p 0
```

This pulls the version's **`uf2`**, which `sh build.sh <target> --tbyb --push` now
publishes as the hashed, non-TBYB **slot-A base** (the same release also publishes the
`bin` = TBYB trial that the box OTA-pulls). Versions published before that build.sh
change carry a TBYB image as their `uf2` — still slot-bootable, just not the intended
committed base; re-`--push` the release to refresh it.

**Recovery** (undo the partition table, back to a flat absolute image):
`picotool load <image>.uf2 --ignore-partitions` (writes 0x0, preserves persist).

**Gotchas that cost real time (2026-07-14):** (a) `picotool load` WITHOUT `-p` lands
in the *inactive* slot and never boots on its own — use `-p 0`/`-p 1` to target a
slot, or add **`-x` to flash-update-BOOT the inactive slot you just filled** (that
is the docked TBYB-trial flow, proven on the Thing Plus 2026-07-17). NOTE the flag
trap: `load -u` means *skip-identical-sectors*, NOT update-boot; `reboot -u` means
BOOTSEL. (b) The fresh PT only goes resident after `reboot -u` — skipping it makes
the slot loads land against a stale table. (c) Slot images must be **signed/hashed**
(the default `dual` build is also `copy_to_ram`) — unsigned images don't slot-boot /
can't `buy`. XIP images DO slot-boot (proven: thingplus-handheld, pico2wusb-class
radio builds) — but radio (cyw43) XIP builds additionally need the
**wiznet-io/patches/ slot-boot fixes** (build.sh auto-applies): the SDK's btstack
TLV bank reads flash through the ATRANS-translated window and wedges
`cyw43_arch_init` under a partition boot — see BLE.md "slot-boot radio wedge".
(d) provision.sh: an explicit slot-A image now leaves slot B EMPTY (the old default
loaded the DUAL tbyb image into whatever board you were provisioning). (e) Radio
boards (pico2_w, Thing Plus 16MB) provision with `PT_JSON=$PWD/pt-pico2w.json`
(1024K slots); the EVB pt.json 512K slots are too small for radio images.
(f) **Nothing may write watchdog scratch[2]/[3] between `rom_reboot` and the reset
firing** — the bootrom PARKS the reboot2 params (the FLASH_UPDATE base!) there
(pico-bootrom-rp2350 `s_varm_api_reboot`), and `pico_ota_arm_update` blocks only
the CALLING core; the other core keeps running through the ~1-2s arm window. The
BLE breadcrumbs stamping scratch[2] silently corrupted the receiver's first native
OTA arm (bootrom matched no partition → booted the OLD slot as `boot=update`, fw
unchanged, no error anywhere). Crumbs moved to scratch[0]/[1]; free registers are
ONLY [0]/[1] (SDK owns [4..7]). Full hazard note at `pico_ota_arm_update`.
(g) TBYB semantics settled: buy-pending follows the IMAGE's EXE_TBYB flag; the
FLASH_UPDATE reboot supplies only the slot preference. Pushing a non-TBYB base
bin over OTA is legal and commits immediately (`boot=update`) — no trial window,
so reserve it for images already proven elsewhere; the fleet path pushes the
TBYB `bin` and gets the full trial → self-test → buy → rollback ladder.

## Adversarial bench checklist (before first fleet rollout)

- power pull mid-slot-write; cable pull mid-fetch (both: old image boots)
- corrupt image (bad sha) → VERIFY rejects, never armed
- image that boots but can't reach dserv → no buy → auto-revert (verify the
  buy-vs-watchdog-arming ORDER: must not buy before self-test passes, must
  not be watchdog-killed before a healthy image can buy)
- repeated try loops don't ping-pong (bootrom try-once semantics)
- persist blob survives A↔B and downgrade (older image + newer blob = the
  known-rejected case → defaults; decide if downgrade should also restore
  an older config or keep-and-warn)
- mid-session guard: agent refuses rollout while in_obs/system running
- wrong-board image → the updater refuses a manifest whose `board` != its own
  baked `BOX_BOARD_ID` (a pimoroni image must never land on a sparkfun board),
  and only fetches the image whose `build` == its own `BOX_BUILD_TARGET`

## Open questions

- physical flash size per board batch (probe; may differ W6300-EVB vs plain
  Pico 2 stock)
- slot size: DEFAULTED to 512KB (Stage 1); revisit only if pico2w image sizing
  (cyw43 blob, no copy_to_ram) forces bigger slots on WiFi boxes
- **image authenticity vs integrity**: sha256 catches corruption but not a
  malicious image from a compromised agent/LAN. LAN-only distribution + sha256
  is defensible for a lab behind its own network; the upgrade is RP2350 secure
  boot + an ed25519 signature in the manifest, verified on-box. Keep sha-only
  or add signing? Revisit if boxes ever fetch off-LAN.
- **reconciliation autonomy**: fully declarative (agent auto-reconciles every
  box to the channel pin whenever `!in_obs`) vs. approve-per-rollout (agent
  proposes on the fleet page, human clicks "roll"). Lean approve-per-rollout
  for a research rig — auto-update mid-study is a failure mode, not a feature.
- whether the updater lives in both slots' images from day one (it must —
  otherwise the first OTA strands the fleet on a non-updatable image)

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
- **Physical flash size not asserted by OTP** (`flash devinfo 0x0c00`: no CS0
  size field, picotool prints no size). `PICO_BOARD=pico2` build → 4MB assumed;
  QSPI aliasing still means confirm-at-PT-creation (write a marker near the
  claimed top, read back — aliasing reveals a smaller part).
- Image type `ARM Secure` (signed image-def block), `secure boot: 0`.

Proposed layout at 4MB (generous, since images are only ~150KB):

```
0x000000  partition table
          slot A   1MB   (images ~150KB today; huge margin under 520KB SRAM)
          slot B   1MB
          spare    ~2MB  (future: staged assets, logs)
0x3FF000  persist sector (4KB, OUTSIDE the slots -- FLASH_STORE_OFFSET today)
```

At 2MB physical: 2×512KB slots + persist still fits ~150KB images ~3× over.
Caveat:
**pico2w** images (cyw43 firmware, no copy_to_ram) are much larger — size
them before promising OTA on WiFi boxes; deferring pico2w OTA is acceptable.

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

## Migration (the one awkward step)

Existing boxes have no partition table: each needs ONE manual reflash to a
partition-table + updater-capable image (BOOTSEL/picotool as today; the
partition table itself is written via picotool or the first boot's init).
Bench-validate the migration + A/B mechanics on the J-Link box before
touching deployed hardware. After that generation, updates are OTA.

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

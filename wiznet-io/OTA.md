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

## Flash layout (decide after probing)

**First step of the project: probe the real flash size** (`picotool info`
shows the JEDEC part). The build assumes `PICO_BOARD=pico2` → 4MB, but QSPI
address aliasing means the working persist-at-"4MB−4KB" does NOT prove 4MB
physical. Proposed layout at 4MB:

```
0x000000  partition table
          slot A   1MB   (images ~300KB today, hard-capped by 520KB SRAM)
          slot B   1MB
          spare    ~2MB  (future: staged assets, logs)
0x3FF000  persist sector (4KB, OUTSIDE the slots -- FLASH_STORE_OFFSET today)
```

At 2MB physical: 2×512KB slots + persist still fits current images. Caveat:
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

## Transports

- **Ethernet**: box opens one plain TCP connection to the agent's staging
  endpoint (W6300 has 8 sockets; reuse the non-blocking send/recv patterns
  from the reg machine). Simple length+sha framed fetch; HTTP not required.
- **USB**: chunked over the existing 128-byte framed channel —
  `cmd/ota/chunk` datapoints carrying offset+crc+payload (~109B), box acks via
  `state/ota/ack` and the host paces on acks. ~300KB ≈ 2800 frames ≈ seconds.
- **Recovery of last resort** (all transports): physical BOOTSEL button →
  `picotool load`. Never removed by any of this.

## Agent + release side

**Status update 2026-07-12:** the agent-side *shelf* is now implemented
(`dserv-agent/firmware.go`, `--firmware-dir`): versioned `.uf2`+`.bin` per
channel (`dev` mutable / `stable`+`extio-fw-vN` immutable, `-dirty` refused off
`dev`), server-computed sha256, `manifest.json` contract, open read endpoints
(`/api/firmware/extio…`, `/firmware/extio/…`) + token-gated multipart publish.
See dserv-agent/README.md for the API. Still TODO below: the *publisher*
(build.sh `--push` / `dservctl fw push`) and the *consumer* (extio-setup pull
→ BOOTSEL flash; then the on-box updater fetching the flat `.bin`).

- build.sh gains a `release` mode: picotool-packaged artifacts (partition-
  aware UF2 for manual loads + flat .bin per slot for the updater) + manifest
  JSON `{version, target, size, sha256}`; version = existing git describe.
- Push artifacts + manifest to the dserv.net repo (same infra as dserv-agent
  component releases; registry service already runs).
- Agent: watch releases → stage locally → compare each box's `state/fw` →
  rollout per policy (pin versions, canary one box, manual approve vs auto),
  gated on ess state. Fleet page = the rollout console.

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
- slot size final call (512KB vs 1MB) after pico2w image sizing
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

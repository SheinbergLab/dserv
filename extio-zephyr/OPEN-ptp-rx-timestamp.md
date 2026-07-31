# OPEN: MCXN947 gets no RX hardware timestamps — the MAC never reports one

Status: **unresolved**, 2026-07-31. Delete this file and fold anything useful into
PORTING.md when it is fixed.

## Symptom

A board that was PTP-syncing fine was reflashed after the Arduino pin-map / analog
work, and now prints, forever, about once a second:

    W: Port 1 drops Sync without valid RX timestamp

`state/ptp/ns` free-runs from boot (reads seconds-since-boot, not epoch). Sync
messages arrive normally — this is not a network, transport, switch or BMCA
problem, and the box is otherwise completely healthy (DHCP, link=1, registers with
dserv, console responsive).

## THE ONE MEASUREMENT THAT MATTERS

`patches/enet-qos-rx-fixes.patch` adds a counter array to the driver, read over
SWD. This is the instrument; do not reason from the log line alone.

    cd /Users/sheinb/src/dserv/extio-zephyr
    ADDR=$(nm build-mcxn-ota/extio-zephyr/zephyr/zephyr.elf | grep -w extio_rxts | awk '{print "0x"$1}')
    pyocd commander -t mcxn947 -c "read32 $ADDR 0x20"      # was 0x30007834

Index meanings:

| idx | name | means |
|---|---|---|
| 0 | pkts | frames reaching the timestamp block |
| 1 | sv | `RX_STATUS1_VALID_FLAG` (RDES3 bit 26) was set |
| 2 | ta | `RX_TIMESTAMP_AVAILABLE_FLAG` (RDES1 bit 14) was set |
| 3 | ctx | context descriptor found |
| 4 | stamp | timestamp actually applied to the pkt |
| 5 | tsdrop | MAC took a snapshot then discarded it (ring full) |
| 6 | orphan | context descriptor recycled as an orphan |
| 7 | rearm | full descriptor rearms |

**Observed: `pkts` climbs ~22/s. EVERY OTHER COUNTER IS ZERO.**

That is the whole finding. `sv=0` and `ta=0` mean the MAC never marks a single
received frame as carrying a timestamp, so the context-descriptor logic — the thing
the patch fixes, and the thing everyone reaches for first — **is never reached at
all**. `tsdrop=0` proves the bounded wait never even runs. The bug is UPSTREAM of
every line of that patch.

## Hardware state, read over SWD (all look correct)

    MAC_TIMESTAMP_CONTROL  0x40100B00 = 0x00037f03
        TSENA|TSCFUPDT|TSENALL|TSCTRLSSR|TSVER2ENA|TSIPENA|TSIPV6ENA|TSIPV4ENA|
        TSEVNTENA, SNAPTYPSEL=0b11   -> timestamping IS enabled, for ALL frames
    MAC_SYSTEM_TIME        0x40100B08 = seconds advancing  -> 1588 counter RUNS
    MAC_CONFIGURATION      0x40100000 = 0x0020e803  (bit 27 IPC = 0)
    DMA_CH0_RX_CONTROL     0x40101108 = 0x00010101  (SR=1, RBSZ decodes to 512 B)

So: timestamping enabled, clock running, frames arriving, and no frame ever comes
back with the status bit that says a timestamp exists.

## RULED OUT (each with a method, not an argument)

* **Stale image / patch not applied.** `git apply --check --reverse` of
  `patches/enet-qos-rx-fixes.patch` succeeds (already applied); the built object
  contains the patch's log string; the freshly built image was flashed and grew to
  254464 B. Verified three ways.
* **MCUboot running an old image from slot1.** Boot log: `Secondary image:
  magic=unset`, `Boot source: none`, `Jumping to the first image slot`. The board
  runs exactly what is flashed.
* **Analog load starving the service loop.** `ain enable 0` + `save` + reboot: still
  34 drops in 40 s, zero clock sets. (The `analog: adc@10d000 …` boot line prints
  regardless of sampler state — it is the hardware-init message, not a status.)
* **IPC / checksum offload.** `MAC_CONFIGURATION` bit 27 is 0 — but the driver
  never sets it anywhere, so it was equally 0 during the runs that DID sync. It is
  a constant, not the change.
* **Transport (L2 vs UDP), switch, IGMP, BMCA, priority1.** Settled 2026-07-29 and
  written up in PORTING.md; Sync messages demonstrably arrive.

## NOT trustworthy — do not build on these

* **`CONFIG_NET_BUF_DATA_SIZE` 512 -> 128 test.** I flashed a 128 build, and
  afterwards `DMA_CH0_RX_CONTROL` still decoded to a 512-byte RBSZ. That
  contradiction is unexplained, so the "128 changes nothing" result is void. The
  config was restored to 512 (its rationale is documented in the board conf, lines
  53-71: margin for the OTA blast, 4x fewer ring slots per frame).
  **Re-run this properly** — verify RBSZ matches the build before drawing anything
  from it.

## UNTESTED, and cheap — do this first

**Flash the SAME image to the other board.** The 8/8 sync run on 2026-07-29 was on
a *different* physical card (probe `RCOJ4PBDEHHGK`); this one is `MJ2PGO1SNNGIV3`.
"It worked before" on THIS board is from David's recollection, not a measurement in
that session. If the other board syncs with the identical binary, this is hardware
or per-board state, and no amount of source bisecting will find it.

## Then: bisect

The board synced before the Arduino pin-map / analog reflash, so start at
`e63c241f`'s parent and walk forward. Candidate commits:

    e63c241f  the analog strip goes all-Arduino -- ain channel k IS Ak
    ca02d46e  analog samples were stamped on the wrong clock, ...
    4d144b0c  analog becomes a runtime subsystem, and the OTA bus error is fixed

`4d144b0c` is the one that also raised `CONFIG_NET_BUF_DATA_SIZE` to 512 and
introduced the consolidated driver patch, so it touches the RX path twice.

## Build / flash / read cycle

    cd /Users/sheinb/src/dserv/extio-zephyr        # MUST cd here: pyocd.yaml pins
                                                   # CMSIS pack 19.0.0. From the repo
                                                   # root pyocd uses 26.06.00, which
                                                   # dies PART WAY THROUGH ERASING
                                                   # and leaves a half-wiped bootloader
    source ~/zephyrproject/.venv/bin/activate
    export ZEPHYR_BASE=$HOME/zephyrproject/zephyr
    west build -b frdm_mcxn947/mcxn947/cpu0 . --sysbuild -d build-mcxn-ota
    west flash -r pyocd -d build-mcxn-ota          # build-mcxn-ota ONLY; build-mcxn947
                                                   # links at offset 0 and erases MCUboot

**The driver patch lives in `~/zephyrproject/zephyr` and a `west update` erases
it.** Check before every session:

    git -C ~/zephyrproject/zephyr apply --check --reverse \
        /Users/sheinb/src/dserv/extio-zephyr/patches/enet-qos-rx-fixes.patch

Success prints nothing and means it IS applied.

## Board / rig identity

* Board under test: MCU-Link probe `MJ2PGO1SNNGIV3`, USB to the Mac, console at
  `/dev/cu.usbmodem<probe>3` (needs DTR asserted; `cat` reads nothing).
* Image `v0.4.0+7`, dserv target 192.168.88.46, DHCP address 192.168.88.54,
  registers as `extio/box`.
* Host-side check: `DSERV_HOST=raspberrypi.local sh host/clock_sane.sh extio/box`
  — reports EPOCH vs UPTIME and is freshness-aware.

## Method notes for whoever picks this up

Four hypotheses died in a row here (stale image, analog, MCUboot swap, IPC), each
plausible and each disproved in one measurement. The counters are what made that
fast — reach for `extio_rxts` before theorising, because it distinguishes "the MAC
never offered a timestamp" from "we lost one that was offered", and those have
completely different causes.

Also: on this board a rebuild is a CHANGED VARIABLE. The RX context-descriptor race
was build-layout-sensitive — one binary synced 8/8, another failed 4/4, and 64 bytes
of unrelated code flipped it. So A/B-ing a config option proves nothing unless each
arm is measured across several boots.

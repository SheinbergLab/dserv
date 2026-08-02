# Moving the extio certification to raspberrypi.local

The Mac chapter closed 2026-08-02: box02 on fw v0.4.0+24, USB-HS, 219+/223
trials green, every red trial classified — the residue is a ~1.5% **Mac
host** delivery tail (20–90 ms stalls), not a box defect. The Pi is the
production host; this runbook makes the hop mechanical.

## What travels how

| thing | vehicle |
|---|---|
| dserv host code (extioconf OTA/routing fixes, binary-safe usbio module, ess_queues fixes, virtual_extio, these scripts) | git pull + rebuild + install (modules too — usbio must be rebuilt on Linux) |
| firmware v0.4.0+24 | already on the box (confirmed permanent); travels with it |
| `extio_test` system (systems tree is NOT git) | rsync (below) or registry push |
| configs.db project/configs/queues | recreated on the Pi: `scripts/extio_test/setup_configs.sh <boxname>` (idempotent) |
| loopback jumpers | move with the board (map in the system README) |

## Steps

1. **Update dserv on the Pi** (pull, build incl. modules, install, restart).
   The extio subprocess must run the NEW extioconf.tcl (OTA routing + drip)
   and the NEW dserv_usbio module (binary-safe) — over Ethernet neither is
   in the data path, but OTA-over-USB from the Pi needs both.

2. **Check the Pi's dlsh carries the dslog streaming API** (the extract's
   µs raw pass needs it, and a Pi dlsh.zip release was already pending from
   the sound work):

       dlsh -e 'package require dslog; puts [info commands dslog::open]'

   Empty output = update dlsh.zip on the Pi first.

3. **Sync the system** (from the Mac):

       rsync -av ~/systems/ess/extio_test/ pi:/path/to/systems/ess/extio_test/

   (or push through the script registry if preferred — either works, rsync
   is unambiguous).

4. **Create the project/configs/queues on the Pi**:

       scripts/extio_test/setup_configs.sh <boxname>

   NOTE: the box's persisted name is **box02** — it keeps that name on the
   Pi unless renamed. Configs/queues are per-box-name.

5. **Attach the box via Ethernet** and verify before any session:
   - `dservctl get extio/boxes` shows it; `state/fw_ver` = 0.4.0+24
   - `state/sync/source` reaches `ptp` (ptpconf.tcl pushes the offset;
     the analog boot-hold ends at PTP lock instead of the 60 s ceiling)
   - box config is RAM-resident: `bind_box` re-pushes everything at every
     session start, so nothing needs manual config

6. **Run**: `wiring_check` first, then `certify.sh --box box02`. Everything
   is measure-first on the new transport; compare against:
   - `at_abs` → DI: **23 µs median** (the original PTP baseline)
   - `at` → DI over USB sw-anchor was 56–150 µs typical
   - the Mac host tail (20–90 ms delivery stalls, ~1.5% of at-trials,
     the events variant is the sensitive probe — run it ×3 early); the
     Pi's tail is one of the campaign's key open questions
   - analog: sweep `late` should stay ~0 (the +22 lattice), blocks gap
     ≤ 5× interval, `di_fifo_drop` 0

7. **Freeze gates from the Pi distributions** (extio_test_analyze.tcl
   `gates` dict) once runs are green — Ethernet+PTP numbers are the ones
   the fleet lives by.

## What Ethernet changes

The whole +19..+21 CDC story is out of the data path (native TCP push both
directions), PTP replaces sw-anchoring (true `at_abs` test), and command
delivery rides the same socket the rig fleet uses. The firmware's +22
lattice, +23 sched ledger, and +24 edge FIFO apply identically — they were
never USB-specific, USB was just the light that found them.

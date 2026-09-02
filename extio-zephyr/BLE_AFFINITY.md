# BLE_AFFINITY.md — which central owns which peripheral

**Status: BUILT AND RIG-VERIFIED (2026-09-01).** Sections 1-5 are implemented
(b6f43099 firmware + console, bf067204 host verbs + roster) and every claim below
was checked on hardware. Remaining: the fleet-page states in §3, which now have
everything they need (`state/ble/paired` plus `usbioBoxes`).

VERIFIED, dongle1 with xiao2 adopted and a second peripheral advertising:

    adv_matched     81 -> 96 -> 109    (+15, +13)   it IS advertising
    adv_unadopted   78 -> 93 -> 106    (+15, +13)   refused, in lockstep
    conn_try         3 ->  3 ->   3    FLAT         never even attempted

conn_try staying flat is what proves it is the allowlist and not a failed
connection. Then opening a window: conns 1->2 within 7 s, `paired` became
`xiao2,box` at 14 s once the name was learned, and dserv saw the new box at
21 s. The gap between connecting and being adopted is the one-service-pass delay
from binding on the main loop rather than the BT thread (§4).

Also verified: empty list stays PROMISCUOUS across an upgrade (the migration in
§7), adoption survives `save` + reboot (persist v29), and an UNSAVED adoption
reverts on reboot.

Originally written after two centrals and two peripherals ran on one USB-only
host for the first time, which is the configuration that made this necessary.

Two documents stay canonical and are not re-litigated here:

* **`wiznet-io/BLE.md` §"Pairing / security"** — already specifies the shape for
  the Pico tier: LE Secure Connections Just Works, a **CLI pairing window**
  (`ble pair 60`) on the receiver, result persisted to a bonded-address
  allowlist, and outside that window the receiver connects only to allowlisted
  addresses so multi-rig rooms do not cross-talk. This plan is the Zephyr
  realisation of that, plus the adopt/release verbs the wired boxes already have.
* **The wired adoption model** (`config/extioconf.tcl`, `www/extio.html`,
  `extio_adopt` / `extio_factory`) — the three ownership states, the
  owner-initiated handoff, and the "unsaved adopt is reversible" convention are
  all borrowed wholesale.

## 1. THE SAFETY PROPERTY IS ALREADY THERE — in the radio

The wired model's foundation is that a box has ONE connect-back slot:
`box_net_eth_server_service()` accepts only while `srv_conn < 0`, so `link:up`
means the slot is taken and **nobody else can connect**. A healthy box cannot be
stolen even deliberately, which is why transfers must be initiated by the
current owner.

A BLE peripheral has exactly that, enforced by hardware rather than by policy:

* `periph.conf` sets `CONFIG_BT_MAX_CONN=1`, and
* a connected peripheral **stops advertising** (a legacy connectable advertiser
  stops on connect — `box_ble_periph.c`).

Invisible and unclaimable while held. So **this plan does not need to protect a
held peripheral, and must not pretend to.** What is missing is only:

1. who claims a peripheral that is FREE, and
2. how an owner lets go.

That distinction matters because the observed symptom misled us. Two centrals in
one room looked like "peripherals compete and one doesn't get accepted"; the
ledgers showed `create_err 0`, `est_err 0`, `dropped` climbing then stopping,
and it was a supervision timeout plus re-advertise plus re-adopt after a central
was unplugged. Not contention. **The race is only ever for a free peripheral.**

## 2. THE DECISION: ownership lives on the CENTRAL

The wired rule is that a box must never self-release its target, because the
asymmetry favours it: a false "abandoned" un-adopts the fleet and needs hand
re-pointing, while a false "still mine" leaves a box visibly stranded and one
click from recovery.

**That asymmetry INVERTS here, and it is the whole decision.** A peripheral that
remembered its central and refused all others would be *bricked* when that
central died or was reflashed: no console worth reaching (the XIAO's is a USB
CDC on a device in someone's hand; the MDK dongle is sealed in a case), no LAN,
no second radio. A central that forgets a peripheral merely leaves it
advertising — recoverable from the fleet page.

So: **the allowlist lives on the central; the peripheral stays dumb.** Put the
authority where recovery is cheap.

The pragmatic argument agrees with the principled one. Peripherals are the
battery, sealed, hardest-to-reflash end of the fleet — and this design needs
**no peripheral firmware change at all**.

## 3. The states map onto the ones the fleet page already draws

| wired term | BLE equivalent | how the host sees it |
|---|---|---|
| free (`target 0.0.0.0`) | advertising, in no allowlist | `adv_matched` rising with `conns` unchanged |
| mine | in this central's allowlist | `state/ble/paired` |
| spoken-for | connected to a DIFFERENT central | **already available** — see below |

`stranded` and `heldByOther` stay separate axes, not extra states, exactly as in
`www/extio.html`.

**"Spoken-for" needs nothing new.** The usbio multiplexing work (0c89842e) made
each device report the box prefixes publishing on it, learned from traffic:

    usbioBoxes usbio0  ->  extio/xiao1 extio/xiao2 extio/dongle2

A relayed peripheral's frames arrive on its central's port, so that IS the
ownership map, available today. Conflating "mine" with "spoken-for" is what
makes a naive adopt button steal a peripheral out of a running rig — the same
trap `www/extio.html` already avoids for wired boxes.

## 4. What to build

**Central firmware** (`box_ble.c`, `dserv_config.h`):

* A persisted allowlist of `{bt_addr_le_t, learned name}`. Size it at
  `CONFIG_BT_MAX_CONN` (8) — a central that can hold eight should be able to
  remember eight.
* `cmd/ble/pair <secs>` — open an adoption window. During it, `scan_cb` accepts
  any matching advertiser; outside it, only allowlisted addresses.
* `cmd/ble/release <name>` — disconnect that peer and drop it from the
  allowlist. The owner's half of the handshake.
* `scan_cb` gains the allowlist filter, next to the `peer_by_addr()` duplicate
  check added in ac867a52.
* `state/ble/paired` — the roster, comma-separated, **published even when
  empty**. "This central has adopted nothing" is a statement an absent key
  cannot make, and older firmware publishes nothing at all: **absent ≠ empty**,
  the same discipline the group roster and the shelf's `imgVersion` needed.

**Host** (`config/extioconf.tcl`, `www/extio.html`):

* `extio_ble_adopt <peripheral> <central>` and `extio_ble_release <peripheral>`.
* **Owner-initiated, release before claim**, for the wired reason: release frees
  the peripheral, and only then is the new central the sole entrant in the race.
  Reversed, the release cannot be delivered.
* Adoption is **unsaved by default**. An unsaved adopt returns the peripheral to
  its old central on reboot — "lend me this handheld for an hour" — and
  persisting is a separate deliberate act, exactly as for wired boxes.
* A retained tombstone for pages, since **dserv never pushes deletions**:
  `extio/released "<peripheral>"` alongside the existing `extio/renamed` and
  `extio/forgotten`.

## 5. THE IDENTITY WRINKLE, and why the pairing window solves it

The allowlist keys on BLE **address**. Humans, the fleet page, and every host
verb speak **names** (`xiao1`). And a central only learns a peripheral's name
AFTER connecting — `learn_name()` reads it from the frames the peripheral
publishes, because the advertised GAP name is not trusted for routing.

So an address cannot be adopted by name before the first connection. That is not
an obstacle: **the pairing window is exactly when address ↔ name gets bound and
recorded together**, which is what a pairing window is for. Adopt-by-name works
from then on.

Consequence to design for: a factory-reset peripheral comes back as `extio/box`
with the same address, so the allowlist entry's name goes stale. Key on address,
treat the name as a label refreshed on every `learn_name()`, and never route on
a remembered name alone.

## 6. What to skip, deliberately

**Bonding.** `central.conf` already sets `CONFIG_BT_SMP=y` and BLE.md specifies
LE Secure Connections Just Works, but an address allowlist delivers the affinity
without it. Bonding buys link encryption and resilience to address privacy —
both worth having, neither on the path to fixing the race. Adding it later does
not invalidate the allowlist; a bonded set is just a stronger key for the same
table.

## 7. Open questions

* **What should a central do with a non-allowlisted peripheral it can see?**
  Ignoring it silently makes an un-adopted peripheral look broken. Counting it
  (`adv_unadopted`) makes it a reading, which is the lesson every BLE bug this
  year has taught: the ledgers found all three.
* **Should a central hold a peripheral it can no longer reach dserv through?**
  The wired answer is yes — a box never self-releases. Probably the same here,
  but a handheld held by a central whose host is gone is invisible to everyone,
  which the wired case never was (the beacon kept advertising).
* **Migration.** Existing rigs have peripherals adopted by nobody. A central
  with an EMPTY allowlist must keep today's behaviour (connect to anything) or
  every deployed pair stops working on upgrade. Empty = promiscuous, and the
  first adopt is what switches a central into selective mode.

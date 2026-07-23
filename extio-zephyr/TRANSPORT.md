# TRANSPORT.md — uplink arbitration for the extio-zephyr hub

**Status: design, 2026-07-22, pre-block-#6.** Written because the RW612 can run
USB, Ethernet, and BLE *simultaneously* (unlike the Pico "dual" build, which was
Ethernet **or** USB, boot-selected). That capability forces a question the Pico
never had to answer: when several links are live at once, which one owns the
box↔dserv relationship, and how do the others relate to it? Companion:
[README.md](README.md) (build roadmap), [../wiznet-io/BLE.md](../wiznet-io/BLE.md)
(frozen radio contract), [../wiznet-io/BENCH_NXP.md](../wiznet-io/BENCH_NXP.md).

## 1. The role taxonomy (the design's foundation)

The three "transports" are **not peers**. They serve three different roles:

| Role | Links | Cardinality | Arbitrated? |
|------|-------|-------------|-------------|
| **Uplink** (box ↔ dserv) | USB-CDC data, Ethernet TCP, Wi-Fi TCP | **exactly one active** | **yes** |
| **Ingress** (peripheral → box) | BLE central (handhelds), local GPIO/analog | all active always | no |
| **Management** (out-of-band) | USB-CDC console / CLI | always available | no |

The critical realization: **BLE is not an uplink.** A BLE handheld is a *source* —
its frames enter the box and are re-published as datapoints, which leave over
whatever uplink is active. So BLE (block #6) plugs into the *publish path*, not
the arbiter. Local GPIO/analog inputs are the same kind of thing. Arbitration is
purely about the **uplink**: the box↔dserv pipe.

## 2. Decisions

1. **Exactly one authoritative uplink at a time.** The datapoint namespace
   (`extio/<name>/…`) is transport-agnostic by design, so publishing the same
   points over two uplinks to one dserv would *collide/duplicate*. One uplink
   owns the box↔dserv relationship; the others stand by.
2. **Selection = Ethernet-if-carrier, else USB — plus a physical override strap.**
   Both USB and Ethernet are first-class. AUTO (the default `transport_mode`,
   reusing the frozen field) picks **Ethernet when the PHY reports carrier/link,
   otherwise USB** — the Pico's proven auto policy. A forced `transport_mode`
   (ETH/USB) pins one. Above the persisted mode sits a **physical mode strap** (a
   GPIO, the RW612 analog of the Pico's GP28): open = honor the persisted/auto
   policy (safe default), tied = force a transport — *authoritative*, so a
   persisted `eth` + no cable can never wedge boot (the extio boot-hang lesson).
   Wi-Fi, when added, is a lower-priority auto candidate. No new wire field.
3. **The USB console is always available — even in Ethernet mode.** It is the
   management/interaction channel (CLI: status, reconfigure, set the dserv
   target), not a data path — so you can plug USB into *any* machine to talk to a
   box whose data uplink is Ethernet to a *different* machine. That is why USB and
   Ethernet are both first-class yet independent: one is the box↔dserv **data**
   uplink, the other a human/host **management** port. (In USB-uplink mode the
   data CDC additionally carries frames; the console CDC is the same either way.)
4. **BLE and Wi-Fi ingress feed the publish path**, routed out the active uplink
   by the arbiter — no arbitration change when block #6 (BLE) or a Wi-Fi source
   lands.

## 3. The uplink seam

Each uplink translation unit exports one vtable — the RW612-idiomatic descendant
of the Pico's `box_net_iface.h`, but **health-arbitrated at runtime** rather than
boot-selected:

```c
typedef struct {
    const char *name;
    int (*init)(const box_config_t *cfg);              /* bring the link up (iface/usbd) */
    int (*health)(void);                               /* 1 = usable right now, 0 = down   */
    int (*connect)(const box_config_t *cfg);           /* establish the dserv session      */
    int (*poll)(uint8_t *buf, int max);                /* inbound; BOX_NET_RESET on (re)connect */
    int (*send)(const uint8_t *buf, int len);          /* one 128-byte frame out           */
    int (*self_register)(const box_config_t *cfg);     /* announce to dserv on (re)connect  */
} box_uplink_if;

extern const box_uplink_if box_uplink_eth;   /* wraps box_net_eth  */
extern const box_uplink_if box_uplink_usb;   /* wraps box_net_usb  */
/* extern const box_uplink_if box_uplink_wifi;  -- later */
```

Health / connect per link:
- **Ethernet:** health = PHY link ∧ DHCP lease ∧ socket connected; connect = TCP
  to `dserv_ip:port`; self_register = `%reg`/`%match` to dserv.
- **USB:** health = enumerated ∧ host draining (DTR on the data CDC); connect =
  no-op (enumeration is implicit); self_register = emit `%match` down the data
  CDC (the host module owns forwarding — the v2 auto-register model).

## 4. The arbiter

`box_uplink.c` holds the vtables in priority order and runs a small state machine:

- **`box_uplink_service()`** (called each service pass, or from a low-rate work
  item): evaluate `health()` of each candidate; if `transport_mode` is forced,
  use that one; else pick the highest-priority healthy link. On a change of
  active link, `connect()` + `self_register()` the new one.
- **`box_uplink_send(frame,len)` / `box_uplink_poll(buf,max)`**: route to the
  active link (drop/again when none healthy).
- **Hysteresis:** a link must read healthy for a promote window and unhealthy for
  a (longer) demote window before the arbiter acts — the Pico's PHY-sense
  debounce lesson, so a marginal cable or a host reopening the tty doesn't flap.
- **Failover cost:** DI/BLE events are box-timestamped, so a dropped or late
  frame during a switch loses delivery, not *timing*; the watchdog re-establishes
  liveness and `self_register()` re-wires the config path. A small bounded
  outbound queue can bridge a brief gap (drop-oldest) — optional, v1 may skip it.

## 5. How block #6 (BLE) and Wi-Fi slot in

BLE is **ingress**, so block #6 is `box_ble.{h,c}` = a **multi-peripheral**
central: the goal is *many* nRF54-class clients (nRF54L15 per NORDIC.md)
connected at once, each sharing small data packets. Each client is a distinct
named ingress source (`extio/<client>/…`), source-stamped + echo-synced (BLE.md),
over the frozen `d5e7000x` GATT pipe with per-client bonding. On each received
frame the box calls **`box_uplink_send()`** — so a press on any of N handhelds
arrives over BLE and leaves over the active uplink, transparently, with no
arbiter change.

The hard constraint block #6 must meet is the RW612 BLE stack's **concurrent-
connection ceiling** (BENCH_NXP D8) — now a *fleet* requirement, not a check:
N simultaneous central links, small frequent packets, per-link source identity
and sync. If N exceeds the stack's ceiling, that bounds the fleet-per-hub (and
argues for multiple hubs or connection cycling — a block-#6 design point).

Wi-Fi, when it lands, is *both* a new uplink vtable (`box_uplink_wifi`) *and*
potentially an ingress; the seam already accommodates both.

## 6. Service-loop shape (post-arbiter)

```c
for (;;) {
    box_uplink_service();                           /* health + failover + (re)register   */
    int n = box_uplink_poll(rx, sizeof rx);         /* inbound from the active uplink      */
    if (n == BOX_NET_RESET) dserv_framer_reset(&fr);
    else if (n > 0) dserv_framer_feed(&fr, rx, n, on_frame, NULL);

    while (box_gpio_poll_di(&cfg, &ev))  publish_di(&ev);      /* local ingress  -> uplink */
    while (box_ble_poll(&bframe))         box_uplink_send(bframe, DSERV_MSG_LEN); /* block #6 */
    if (watchdog_due())                   box_uplink_send(wd_frame, DSERV_MSG_LEN);
    k_msleep(1);
}
```

## 7. v1 vs deferred

- **v1:** the vtable seam + arbiter with config-driven AUTO selection, boot-time
  pick-healthy, USB console always-on, BLE ingress via `box_uplink_send`. This
  covers the common case — a box on a stable configured uplink — and matches the
  Pico's proven `transport_mode` model.
- **Deferred (documented, not built):** aggressive runtime failover with the
  bridging queue; **multi-homing** (publishing to *different* dservs over
  different uplinks simultaneously — legitimately useful for a laptop-debug +
  rig-production split, but a real complexity step and explicitly out of v1).

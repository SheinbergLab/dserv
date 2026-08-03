# systemd units

| unit | what it is |
|---|---|
| `dserv.service` | the dataserver itself |
| `dserv-ptp4l@.service` | PTP grandmaster for the extio boxes, templated on interface |
| `dserv-phc2sys@.service` | rate-locks that interface's PHC to the system clock |
| `dserv-disable-eee@.service` | forces Energy-Efficient Ethernet OFF on an interface at boot |
| `wifi-powersave-off.conf` | NetworkManager drop-in (not a unit): Wi-Fi power save off + retry-forever |

## Host power-management pair (every new Linux host)

Distro defaults leave two power features on that this platform cannot afford:
EEE on the wired port (LPI wake jitter, right on the PTP/box path — found
active on rpi500's grandmaster port) and Wi-Fi power save (the known brcmfmac
goes-deaf/drops-association syndrome, worst at in-cage signal levels; plus
multi-ms ssh latency bubbles on management links).

    sudo cp dserv-disable-eee@.service /etc/systemd/system/
    sudo systemctl daemon-reload
    sudo systemctl enable --now dserv-disable-eee@eth0     # your wired iface
    sudo ethtool --show-eee eth0                           # want: status: disabled

    sudo cp wifi-powersave-off.conf /etc/NetworkManager/conf.d/
    sudo systemctl reload NetworkManager                   # new associations
    sudo iw dev wlan0 set power_save off                   # the current one
    iw dev wlan0 get power_save                            # want: off

Caveats: first-time enable of the EEE unit renegotiates the link (~5 s down),
so not mid-session; the NM drop-in is deliberately conf.d-level because
netplan-generated profiles are regenerated and per-profile `nmcli modify`
edits get silently reverted. If a cage machine still drops Wi-Fi with power
save off, the next knob is `wifi.scan-rand-mac-address = no` (commented in
the conf file).

## PTP pair

Install once per host, substituting the interface — `eth0` on the rig Pi,
`enp86s0` on office-stim:

    sudo cp dserv-ptp4l@.service dserv-phc2sys@.service /etc/systemd/system/
    sudo systemctl daemon-reload
    sudo systemctl enable --now dserv-ptp4l@eth0 dserv-phc2sys@eth0

Check:

    systemctl status dserv-ptp4l@eth0 dserv-phc2sys@eth0
    journalctl -u dserv-phc2sys@eth0 -f     # want "s2" and a small offset

`phc2sys` is `BindsTo` the ptp4l unit, so stopping or losing ptp4l takes it down
too rather than leaving it disciplining against a dead master.

### Before enabling on a NEW host

Confirm the NIC actually has a PHC, because this is not a graceful degradation —
without one there is no `/dev/ptpN`, `ptp_anchor.sh` cannot compute `D`, and the
whole approach is unavailable rather than merely less accurate (a Pi 4 is in
exactly this position):

    ethtool -T <iface>

Look for `PTP Hardware Clock: <n>` (not `none`) plus `hardware-transmit`,
`hardware-receive`, `hardware-raw-clock`. Also read the **Hardware Receive
Filter Modes** list: if it shows only `none` and `all`, `--hwts_filter full` is
required, which is why both units carry it. Both NICs measured so far are like
that, so treat it as the norm rather than a quirk.

If `/dev/ptpN` is root-only, add a udev rule so unattended tooling (and
`host/phc_offset`) does not need sudo:

    printf 'SUBSYSTEM=="ptp", KERNEL=="ptp[0-9]*", GROUP="plugdev", MODE="0660"\n' \
      | sudo tee /etc/udev/rules.d/60-ptp.rules
    sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=ptp

### Portability (amd64, non-Pi ARM, other distros)

The units are plain text and architecture-independent — amd64 and arm64 are no
different. Three things actually vary between hosts, and only the last is a real
gate:

**Interface name.** Already the template parameter, so `@eth0` / `@enp86s0` /
`@eno1` all work from the same file.

**Binary path.** `ExecStart=` uses `/usr/sbin/ptp4l`, which resolves on Debian,
Ubuntu and RHEL, and on Arch too (its `/usr/sbin` is a symlink into `/usr/bin`).
Confirm on any new host and adjust if it differs:

    command -v ptp4l phc2sys

**A PHC on the NIC.** This is the gate — see the `ethtool -T` check above. No
PHC means no `/dev/ptpN`, and the approach is unavailable rather than degraded.

Note the ordering is deliberately loose: `After=` and `Wants=`, never `BindsTo=`
or `Requires=`. Those propagate STOP, and a unit stopped by a dependency is NOT
restarted by `Restart=` — so a NIC flap, or ptp4l simply restarting, would strand
phc2sys down permanently and silently. That is the exact failure these units
exist to prevent, so resilience beats strictness here.

### Why not the distro `ptp4l@` / `phc2sys@` units

Debian's linuxptp ships both (disabled by default), which is why ours carry the
`dserv-` prefix rather than shadowing them. The deciding reason to keep our own
is **direction**: the stock phc2sys unit disciplines the system clock FROM the
PHC — the slave-host case — while we need the reverse, because this host is the
grandmaster and dserv stamps from `CLOCK_MONOTONIC`. Enabling the stock unit by
mistake would put phc2sys and the NTP daemon in a fight over the system clock,
and the symptom is slow wander that looks like a box problem. A distinctly named
unit makes that mistake structurally impossible; a drop-in override would not,
since the stock unit stays enable-able.

Secondary: the flags and the *reasoning* for them travel with the repo, instead
of living in `/etc/linuxptp/*.conf` plus a drop-in that a fresh host never gets.

### What these units do NOT do

They bring up the PTP *protocol* leg — box 1588 counters disciplined to this
host's PHC. They do **not** anchor boxes to dserv's timeline. dserv stamps from
`CLOCK_MONOTONIC` + a constant, a different oscillator, so the constant
`D = dserv_us - ptp_us` must be measured on the host and pushed to each box as
`cmd/ptp/offset` — currently `extio-zephyr/host/ptp_anchor.sh`, run manually,
**and an anchor never survives a box reboot or reflash**.

Until that is automated, after enabling these units you still need:

    sh extio-zephyr/host/ptp_anchor.sh extio/<box>

`extio-zephyr/host/start_ptp.sh` and `start_phc2sys.sh` remain for ad-hoc bench
restarts, but they `setsid` into `/tmp` with no supervision and do not survive a
reboot — these units are the durable path.

## Choosing a role: `dserv-ptp-setup`

The package copies these templates into `/etc/systemd/system` from its postinst
and **enables nothing**. That copy is inert on purpose: a `foo@.service` with no
instance cannot start, so shipping them costs nothing, while *instantiating* one
is a site decision the installer cannot make.

    dserv-ptp-setup candidates            # interfaces with a PHC
    sudo dserv-ptp-setup grandmaster eth0 # this host defines the site's time
    sudo dserv-ptp-setup client enp86s0   # this host follows the grandmaster
    dserv-ptp-setup status                # role, conflicts, time source
    sudo dserv-ptp-setup disable

Exactly **one** host per site is the grandmaster. Clients need no address for it
— PTP's BMCA elects the master from its Announce messages, which is the main
advantage over NTP. The only value that must match is the domain number (0).

### The two roles are mirror images, and the NTP rule inverts

|            | grandmaster                        | client                              |
|------------|------------------------------------|-------------------------------------|
| `ptp4l`    | `--priority1 127`                  | `-s` (slaveOnly)                    |
| `phc2sys`  | `-c IF -s CLOCK_REALTIME` (sys→PHC)| `-s IF -c CLOCK_REALTIME` (PHC→sys) |
| NTP/chrony | **must keep running** — the source | **must be off** — PTP is the source |

On a grandmaster, `phc2sys` pushes the NTP-disciplined system clock into the PHC;
stop NTP and the site distributes free-running quartz. On a client the direction
reverses, so an NTP daemon would steer `CLOCK_REALTIME` against `phc2sys`. Two
disciplinarians on one clock do not error — they fight, and it reads as drift.
The client unit declares `Conflicts=` on the usual NTP units so systemd stops
them rather than relying on anyone remembering.

`slaveOnly` on the client is a safety property, not a preference: without it the
host joins the election and can *win* when the real grandmaster reboots, silently
re-basing the site's timeline. That is the clock step `ptp/d_delta_us` detects.

### What the helper refuses to do

- enable a role on an interface with no PHC (`ptp4l` would run and report offsets
  of exactly `0` forever)
- run alongside the distro's `ptp4l@`/`phc2sys@`/`timemaster` units
- run alongside a hand-started `ptp4l`/`phc2sys` — which is what office-stim was
  found in on 2026-07-28: correct flags, but nothing would have restarted them
  after a reboot, and no check reported that. `status` now says so out loud.

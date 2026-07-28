# systemd units

| unit | what it is |
|---|---|
| `dserv.service` | the dataserver itself |
| `dserv-ptp4l@.service` | PTP grandmaster for the extio boxes, templated on interface |
| `dserv-phc2sys@.service` | rate-locks that interface's PHC to the system clock |

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

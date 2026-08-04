# Running dserv on an offline / air-gapped machine

dserv is designed so that a machine with no LAN or internet runs
experiments normally: everything between subprocesses, the web GUIs, ESS,
and local stim2 goes over loopback. Outbound network is used only for the
optional dserv.net registry (script/config sync, subject roster), mesh
heartbeats, cloud trial ingest, and extio OTA fetches — all of which
degrade cleanly when unreachable. This document is the recipe for making
that degradation *silent and instant* instead of "eventually, after
timeouts".

## TL;DR for a fresh offline install

1. Install while briefly networked (the bootstrap and OS packages need it).
2. Create the offline marker:

   ```sh
   touch /usr/local/dserv/local/offline
   ```

3. Do **not** create `local/pre-registry.tcl` (no registry) and do not
   install `/etc/dserv/trial_ingest_secret` (no cloud ingest).
4. Linux only — stop boot from waiting ~2 min for a network that will
   never come, and avoid resolver stalls:

   ```sh
   sudo systemctl mask systemd-networkd-wait-online.service NetworkManager-wait-online.service
   ```

   (dserv.service `Wants=network-online.target`, which is right for
   connected rigs; masking the waiter is the per-machine override.)
5. Confirm the registry agent is not enabled (it is not by default):
   `systemctl is-enabled dserv-agent` → disabled/not-found.

## The offline flag

One flag, two spellings, same effect:

- marker file `local/offline` (contents ignored), or
- environment `DSERV_OFFLINE=1` (any value other than empty/`0`).

`config/dsconf.tcl` checks the marker before any subprocess starts and
exports `DSERV_OFFLINE=1` so every interp inherits it, and publishes
`system/offline` (0/1) for UIs. Effects when set:

- `https_get/https_post/https_put/https_delete` (TclHttps, used by the
  registry client, ess_sync, mesh, extio OTA) **refuse any non-loopback
  host instantly** — before DNS resolution, which the `-timeout` option
  never bounded. Callers see the error
  `offline mode (DSERV_OFFLINE): refusing outbound connection to <host>`.
- the `scripts` subprocess skips its deferred boot sync with a log line
  instead of attempting a pull;
- `trialsync` keeps queuing trials to its SQLite outbox but attempts no
  POSTs (the backlog drains normally after the flag is cleared);
- the mesh heartbeat skips silently.

**Loopback is exempt everywhere**: targets on `localhost` / `127.x` /
`::1` still work, so a registry or ingest server running on the box
itself is unaffected.

The flag is read per call, so it can be toggled at runtime from any
subprocess interp (`set ::env(DSERV_OFFLINE) 1`) — handy for taking a
configured rig off the network temporarily without touching its
`local/pre-registry.tcl`. For the reverse (clearing it at runtime), also
remove `local/offline` or it returns on the next restart.

## What still needs thought on an air-gapped box

- **Wall clock**: no NTP means the system clock drifts. dserv's internal
  event timing is unaffected, but datafile names and wall-clock
  timestamps will wander — set the clock by hand occasionally.
- **Web GUIs**: fully local. CodeMirror (script editor), Ace (tutorial)
  and the UI fonts are vendored under `www/js/vendor/` and `www/fonts/`.
  The one exception is Spike Explorer's in-browser ANOVA kernel, which
  streams Pyodide + numpy/scipy from jsdelivr (far too large to vendor);
  it reports "needs internet access" and everything else on the page
  works.
- **Updates / new systems**: land them by USB stick (rsync the systems/
  tree, or `sudo make install` from a checkout) — or connect briefly,
  clear the flag, sync, re-flag.

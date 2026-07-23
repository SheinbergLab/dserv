# Request-Path Timing

How to measure whether dserv's serialized request path has headroom on a
given machine, and how to read the result without drawing the wrong
conclusion. Written while evaluating a possible move from Raspberry Pi 5
to NXP i.MX95 (Cortex-A76 → Cortex-A55).

## What is instrumented

Each `TclServer` has one interpreter and one `process_requests` thread
consuming a single FIFO. Everything that interpreter does — client
scripts, datapoint triggers, `dservWhen` callbacks, timers — is serialized
through it. `RequestTiming` (`src/RequestTiming.h`) stamps three points
per request:

| quantity | meaning |
|---|---|
| queue residency | `t_dequeue - t_enqueue` — how long it waited |
| execution | `t_done - t_dequeue` — how long the work took |
| **rho** | `sum(execution) / wall` — how close to saturation |

### Commands

```tcl
dservTiming on        ;# also resets the window
dservTiming stats     ;# totals, rho, queue/exec percentiles, by-type
dservTiming slow      ;# the 12 slowest individual requests, with labels
dservTiming labels    ;# per-label totals, ordered by time spent
dservTiming off       ;# releases the sample ring
```

Per-interpreter, off by default. Disabled cost is three `clock_gettime`
calls (~47 ns on an M-series Mac) and 328 bytes. Enabled adds ~96 KB.
Cheap enough to leave on in production.

## Why rho is the number that matters

The queue has a **single consumer**, so waiting time scales as
`rho/(1-rho)`. A modest increase in per-request work produces a
disproportionate increase in latency once the thread nears saturation:
going from rho 0.3 to 0.75 turns a mean wait of 0.43 service-times into
3 — a 7x latency increase from a 2.5x compute increase.

Crucially, **execution time alone will not show this coming.** In the
saturation sweep below, median execution stayed flat at ~468 us across
the entire load range while queue residency grew 1800x. Anyone
instrumenting only `Tcl_Eval` would have concluded the system was
perfectly healthy at every level.

## Reading the output: work vs waiting

This is the single most important interpretive point, and getting it
wrong inverts the conclusion.

A large execution time means one of two completely different things:

- **Real interpreter work.** Scales with core speed. An A55 at roughly
  0.4x the per-thread throughput of an A76 makes this ~2.5x worse.
- **A blocking wait** — a `!` remote call into stim, a synchronous device
  read, a `send` to another subprocess. Bounded by the peer, *not* by the
  local CPU. A wait on a 60 Hz frame is 16.7 ms on any core.

Only the first is a reason to worry about a slower CPU. `dservTiming
labels` exists to tell them apart: look at the mean, not just the max,
and at what the label actually names.

Worked example from the baseline run: `ess` showed `dpoint_script timer/0`
at **10.1 ms mean** over 383 calls. That looks alarming until you notice
stim was running and ESS blocks on `!` calls until stim completes its
frame — so it is a wait, and it will not get worse on an A55. Meanwhile
the genuine interpreter work in the same interpreter (`dpoint_set
eventlog/events`) was 13.5 us mean. Two orders of magnitude apart, and
only the small one scales.

## Running a measurement

### 1. Check the clock is cheap on this platform

```sh
cc -O2 -o clockcost scripts/timing/clockcost.c && ./clockcost
```

`CLOCK_MONOTONIC_RAW` is a fast vDSO call on macOS and modern arm64
Linux, but on older kernels it can fall back to a real syscall costing
hundreds of ns — which would change the overhead assessment by an order
of magnitude. Check before trusting anything else.

Also pin the cpufreq governor to `performance` on both machines being
compared, or you are measuring DVFS policy rather than silicon.

### 2. Drive a reproducible session

```sh
scripts/timing/run_virtual_session.sh start
scripts/timing/collect_rho.py 300
scripts/timing/run_virtual_session.sh stop
```

The virtual subject is what makes cross-platform comparison valid. It
responds on a fixed schedule, blind to trial type, so two machines
receive identical input — a real animal never produces the same trial
sequence twice, which confounds any A/B.

### 3. Validate the instrument on a new platform (optional)

```sh
scripts/timing/rho_sweep.py 2000 3
```

Drives concurrent clients and should reproduce `q ~ (C-1) * S`. On the
Mac baseline it matched to 0.4%, which is good evidence the numbers mean
what they claim.

## Gotchas

Every one of these silently produced wrong numbers before being caught.

**Per-IP connection limit is 8** (`MAX_CONNECTIONS_PER_IP`,
`src/TclServer.h`). All localhost clients share one bucket. Exceed it and
the extra connections are *rejected* while the harness happily reports a
smaller experiment under a larger label. `rho_sweep.py` now holds one
persistent control connection and warns on short connects.

**Reused request structs must re-stamp.** `tcp_client_process` and
`message_client_process` build one `client_request_t` per *connection*
and reuse it for every request. The construction-time default initializer
therefore measures connection age, not queue wait — which showed up as a
suspiciously constant "queue time" equal to half the run duration. Any
new enqueue path that reuses a struct needs
`req.t_enqueue = request_timing_now_ns()` before `push_back`.

**Closed-loop load cannot sample low rho.** A client that re-sends the
instant it is answered sits at rho ~0.8 with a single connection. The
sweep measures the saturated regime only. Genuinely open-loop arrivals
(datapoints on port 4620, hardware events, timers) are what reach the
interesting part of the curve.

**Start `virtual_eye` explicitly.** The virtual-subject recipe uses
`require_fixation 0`, so the paradigm never asks for an eye and there is
*no eye traffic at all*. That stream (250 Hz, with full trigger/notify/
logger fan-out) is a top-three contributor, so omitting it badly
underestimates rho. `run_virtual_session.sh` starts it for you.

**Keep the window inside one block.** A block is finite (~90 trials). If
it ends mid-window the system goes idle and rho is diluted toward zero.
`collect_rho.py` warns when this happens.

**Headless underestimates.** No stim2 means the display path and the
ess→stim hop are absent; no real extio box means its ingress stream is
missing. Both bias rho downward — treat a headless number as a floor.
Conversely the virtual subject never pauses or disengages, which biases
trial rate *upward*; that direction is conservative for a headroom test.

**`virtual_subject.tcl` is not in the .deb.** It is an example, so a
packaged install has no copy under `$DSPATH/config`. Copy it next to the
harness (`scp config/virtual_subject.tcl <host>:~/timing/`);
`run_virtual_session.sh` looks there second.

**Paths passed to `source` must be absolute.** They are evaluated inside
dserv, which resolves relative paths against ITS cwd, not the calling
script's — so `./virtual_subject.tcl` silently fails to load even when it
sits right next to the script that named it.

**`dservctl` exits 0 even when the Tcl it sent raises.** Its return code
proves nothing, so it must not gate anything and must not be left bare
under `set -e` (which will kill a script with no message at all). Probe
for the effect you wanted instead — `run_virtual_session.sh` checks
`expr 6*7` against `42`.

**Check where stim2 actually runs.** Production rigs point at a remote
stim2 host, all-in-one trainers run it locally. The `ess` blocking
numbers mean completely different things in the two topologies (see
below), so record which one you measured.

## First baseline: macOS, M-series, 2026-07-22

dserv with 21 interpreters, `pursuit/ballistic/ballistic_detect`,
virtual subject, 250 Hz virtual eye, stim running.

Saturation sweep (synthetic Tcl loop, S = 467 us):

```
 conns  thruput    rho |    q_p50    q_p99    q_max |   x_p50
     1     1677  0.779 |      1.9      9.0    425.4 |   464.8
     2     2006  0.947 |    397.6    441.6    747.9 |   468.6
     4     2050  0.971 |   1333.5   1414.7  16836.0 |   469.1
     7     2102  0.991 |   2741.6   2870.7  18369.2 |   469.7
```

Per-interpreter under the real paradigm: **max rho 0.033**, `ess` at
0.0155. `ess` could absorb ~45x more work before approaching the knee.
Utilization is not the constraint on this hardware.

What the tails actually were, once attributed:

- **OS scheduling**, not dserv. Handlers with ~50 us means showed ~40 ms
  maxima — an ~900x outlier on code that formats eight bytes, appearing
  simultaneously across independent queues. Expect this to differ on a
  tuned Linux rig with `SCHED_FIFO`.
- **Blocking on stim.** `ess` `timer/0` at 10.1 ms mean, `button_simulate`
  at 14.6 ms mean (~one 60 Hz frame). Waits, not work.
- **A real bug.** `extio` spent 66.8 ms *every* 2 s probing for USB boxes
  with nothing attached — `exec ioreg` on Darwin, most of it fork overhead
  from a 20-thread process. Fixed by globbing `/dev/cu.usbmodem*` first:
  66.8 ms → 0.9 ms, rho 0.033 → 0.0005. Linux was never affected (a
  by-id glob, no exec).

## Cross-platform results, 2026-07-23

Same paradigm, same virtual subject, same 250 Hz eye, `performance`
governor everywhere. 300 s windows, ~55 trials each.

| host | SoC | stim2 |
|---|---|---|
| Mac | Apple M-series | local |
| pi4dev | Pi 4, Cortex-A72 @1.8 GHz | local |
| rpi500 | Pi 500, Cortex-A76 | **remote (separate host)** |

### Real interpreter work — the clean CPU comparison

Labels with no blocking in them. This is what scales with core speed.

| label | Mac | pi4 (A72) | rpi500 (A76) | A72/A76 |
|---|---|---|---|---|
| `dpoint_set eventlog/events` | 13.5 us | 15.5 us | **9.4 us** | 1.65x |
| `dpoint_script pursuit/coh_event` | 185.8 us | 230.0 us | **143.5 us** | 1.60x |
| `dpoint_script proc/windows/status` | 90.6 us | 118.6 us | **82.9 us** | 1.43x |
| `dpoint_script timer/2` | 1354 us | 1173 us | **691 us** | 1.70x |

**A72 -> A76 is ~1.5x**, and the A76 beats the Mac on every one. Use this
as the calibration point when reasoning about an A55.

### Blocking on stim — depends entirely on topology

| host | stim2 | `timer/0` mean | `button_simulate` mean | `ess` rho |
|---|---|---|---|---|
| Mac | local | 10,143 us | 14,610 us | 0.0155 |
| pi4 | local | 18,212 us | 27,224 us | **0.0286** |
| rpi500 | remote | 9,340 us | 14,702 us | **0.0150** |

rpi500's *remote* stim is twice as fast as pi4's *local* stim: a network
round-trip to a box with a real GPU costs less than rendering locally on
weak graphics. Local rendering roughly doubles both the blocking time and
`ess` rho.

Note rpi500 carries MORE real traffic than pi4 — `slider` and `ain` at
250 req/s each from actual hardware — and still shows lower rho
throughout.

### Linux tails are far better than macOS

The ~40 ms stalls in the macOS baseline were the OS, as suspected:

| handler (mean ~45 us) | Mac max | pi4 max | rpi500 max |
|---|---|---|---|
| `virtual_eye` `virtualeyeTimer/0` | 43,636 us | **780 us** | **152 us** |
| `em` `eyetracking/virtual` | 52,978 us | **601 us** | **233 us** |

~900x outliers on macOS, ~5-18x on Linux. Do not read macOS tail numbers
as representative of a rig.

## Conclusions

Utilization is not the constraint anywhere: max rho was 0.033 (macOS,
before the extio fix), 0.0286 (pi4) and 0.0164 (rpi500). Even a 2019 A72
leaves `ess` ~25x of headroom before the queueing knee.

The concern that motivated all this — CPU-bound Tcl throughput on a
slower core — is a rounding error. Real interpreter work moved only
~1.5x across two CPU generations. What consumes wall-clock is *waiting*.

**The i.MX95 question therefore splits by deployment:**

- **Production rigs (remote stim2).** The host never renders, so the
  Mali-G310 is irrelevant and so is the stim2 DRM/KMS work. Only the
  interpreter column matters, and an A55 near or below the A72 still
  leaves comfortable margin. This looks like a safe swap.
- **All-in-one trainers (local stim2).** The GPU dominates: local
  rendering on weak graphics doubled both blocking time and `ess` rho.
  Here the Mali-G310 question is live, and the DRM/KMS port attacks
  exactly this number by removing the compositor from the present path.

Still to measure: an actual trainer (pi4dev is a dev box, not a trainer),
and a trainer re-measured after the DRM/KMS port. If that pulls local
blocking from ~18 ms toward the ~9 ms offloaded figure, the trainer case
stops being GPU-bound too.

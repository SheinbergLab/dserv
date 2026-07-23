#!/usr/bin/env python3
"""
Per-interpreter request-path timing for a running dserv.

Enables dservTiming across every interpreter, runs a measurement window,
then reports where each sits relative to saturation and attributes the
slow requests to actual code.

  ./collect_rho.py [seconds]        default 300

Read the output with docs/request_timing.md open: the important column is
rho, and a large exec time means very different things depending on the
label it is attributed to (real work scales with core speed, a blocking
round-trip does not).
"""
import subprocess
import sys
import time

TIMEOUT = 30


def send(interp, cmd):
    args = ["dservctl", "-c", cmd] if interp == "dserv" else ["dservctl", interp, cmd]
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=TIMEOUT)
        return r.stdout.strip()
    except subprocess.TimeoutExpired:
        return ""


def field(report, name):
    toks = report.split(name)[1].replace("{", " ").replace("}", " ").split()
    return {toks[0]: float(toks[1]), toks[2]: float(toks[3]),
            toks[4]: float(toks[5]), toks[6]: float(toks[7])}


def scalar(report, name):
    toks = report.replace("{", " ").replace("}", " ").split()
    return float(toks[toks.index(name) + 1])


def split_entries(s):
    """Split a Tcl list of {..} groups, respecting one level of nesting."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "{":
            depth += 1
            if depth == 1:
                cur = ""
                continue
        elif ch == "}":
            depth -= 1
            if depth == 0:
                out.append(" ".join(cur.split()))
                continue
        if depth >= 1:
            cur += ch
    return out


def main():
    window = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0

    names = send("dserv", "dservGet dserv/interps").split()
    if not names:
        sys.exit("no interpreters reported -- is dserv running?")

    status = send("dserv", "dservGet ess/status")
    system = "/".join(send("dserv", "dservGet ess/" + k)
                      for k in ("system", "protocol", "variant"))
    print(f"# {len(names)} interpreters | {window:.0f}s window")
    print(f"# ess: {system} [{status}]")
    if status != "running":
        print("# WARNING: ess is not running -- rho will reflect an idle system")
    print()

    for n in names:
        send(n, "dservTiming on")

    obs0 = send("dserv", "dservGet ess/obs_id")
    time.sleep(window)
    obs1 = send("dserv", "dservGet ess/obs_id")
    status_end = send("dserv", "dservGet ess/status")

    rows = []
    for n in names:
        rep = send(n, "dservTiming stats")
        if "rho" not in rep:
            continue
        try:
            rows.append((scalar(rep, "rho"), n, scalar(rep, "total"),
                         scalar(rep, "rate_hz"),
                         field(rep, "queue_us"), field(rep, "exec_us")))
        except (ValueError, IndexError):
            continue

    rows.sort(reverse=True)
    hdr = (f"{'interp':<16} {'req/s':>7} {'rho':>7} | {'q_p50':>8} {'q_p99':>9} "
           f"{'q_max':>9} | {'x_p50':>8} {'x_p99':>9} {'x_max':>9}")
    print(hdr)
    print("-" * len(hdr))
    for rho, n, total, rate, q, x in rows:
        if total == 0:
            continue
        print(f"{n:<16} {rate:7.1f} {rho:7.4f} | {q['p50']:8.1f} {q['p99']:9.1f} "
              f"{q['max']:9.1f} | {x['p50']:8.1f} {x['p99']:9.1f} {x['max']:9.1f}")

    idle = [n for rho, n, total, *_ in rows if total == 0]
    if idle:
        print(f"\nidle (no requests): {' '.join(idle)}")
    print(f"\ntrials during window: {obs0} -> {obs1}   ess at end: {status_end}")
    if status_end != "running" and status == "running":
        print("!! the block ended mid-window -- rho is diluted by idle time, rerun")

    for rho, n, total, *_ in rows[:6]:
        if total == 0:
            continue
        slow = send(n, "dservTiming slow")
        labels = send(n, "dservTiming labels")
        if not slow.strip():
            continue
        print(f"\n{'=' * 78}\n{n}: slowest individual requests\n{'=' * 78}")
        for e in split_entries(slow)[:8]:
            print("  " + e)
        print(f"\n{n}: where the time goes (by label, top 8)")
        for e in split_entries(labels)[:8]:
            print("  " + e)

    for n in names:
        send(n, "dservTiming off")


if __name__ == "__main__":
    main()

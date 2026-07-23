#!/usr/bin/env python3
"""
Characterize the dserv serialized request path.

The newline handler (tcp_client_process) blocks on its reply queue after
each push, so a single connection can only have one request outstanding.
Offered load is therefore driven by CONCURRENCY: N connections each in a
send/reply loop.  Sweeping N walks utilization up toward saturation and
shows what happens to queue residency on the way.

Reports, per step:
  rho    fraction of wall time the process thread spent inside eval
  q_*    queue residency percentiles (us) - how long requests waited
  x_p50  median execution time (us) - the actual work
"""
import socket, sys, threading, time

HOST, PORT = "localhost", 2570


# dserv enforces MAX_CONNECTIONS_PER_IP = 8 (TclServer.h:150), and every
# client here is 127.0.0.1 so they all share one bucket.  Hold a single
# persistent control connection -- which is what a real client does anyway --
# and leave the rest of the budget for load.
MAX_PER_IP = 8
_ctrl = None


def cmd(text, timeout=30.0):
    global _ctrl
    if _ctrl is None:
        _ctrl = socket.create_connection((HOST, PORT), timeout=timeout)
    _ctrl.sendall(text.encode() + b"\n")
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = _ctrl.recv(65536)
        if not chunk:
            raise IOError("control connection closed")
        buf += chunk
    return buf.decode().strip()


def field(report, name):
    toks = report.split(name)[1].replace("{", " ").replace("}", " ").split()
    return {toks[0]: float(toks[1]), toks[2]: float(toks[3]),
            toks[4]: float(toks[5]), toks[6]: float(toks[7])}


def scalar(report, name):
    toks = report.replace("{", " ").replace("}", " ").split()
    return float(toks[toks.index(name) + 1])


class Client(threading.Thread):
    """One connection, closed-loop: send, wait for reply, repeat."""

    def __init__(self, script, duration):
        super().__init__(daemon=True)
        self.script, self.duration = script, duration
        self.sent = 0

    def run(self):
        try:
            s = socket.create_connection((HOST, PORT), timeout=30)
        except Exception:
            return
        payload = self.script.encode() + b"\n"
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < self.duration:
            try:
                s.sendall(payload)
                buf = b""
                while not buf.endswith(b"\n"):
                    c = s.recv(65536)
                    if not c:
                        raise IOError
                    buf += c
            except Exception:
                break
            self.sent += 1
        try:
            s.close()
        except Exception:
            pass


def measure(script, nclients, duration):
    clients = [Client(script, duration) for _ in range(nclients)]
    t0 = time.perf_counter()
    for c in clients:
        c.start()
    time.sleep(0.25)                    # let every client connect first
    cmd("dservTiming on")               # enabling also resets the window
    for c in clients:
        c.join()
    elapsed = time.perf_counter() - t0
    report = cmd("dservTiming stats")
    connected = sum(1 for c in clients if c.sent > 0)
    if connected != nclients:
        print(f"  !! only {connected}/{nclients} clients connected "
              f"(per-IP limit {MAX_PER_IP})")
    return report, sum(c.sent for c in clients) / elapsed, connected


def main():
    work = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
    script = "for {set i 0} {$i<%d} {incr i} {}" % work

    print(f"# host: this Mac | service work: Tcl loop x{work} | {duration}s/step")

    rep, _, _ = measure(script, 1, 2.0)
    S_us = field(rep, "exec_us")["p50"]
    print(f"# service time S = {S_us:.1f} us  ->  saturation ~ {1e6/S_us:.0f} req/s")
    print(f"# per-IP connection limit {MAX_PER_IP}, one held for control\n")

    hdr = (f"{'conns':>6} {'thruput':>8} {'rho':>6} | {'q_p50':>8} {'q_p95':>8} "
           f"{'q_p99':>8} {'q_max':>8} | {'x_p50':>7}")
    print(hdr)
    print("-" * len(hdr))

    for n in range(1, MAX_PER_IP):      # 1..7, leaving a slot for control
        rep, thru, connected = measure(script, n, duration)
        q, x = field(rep, "queue_us"), field(rep, "exec_us")
        print(f"{connected:6d} {thru:8.0f} {scalar(rep,'rho'):6.3f} | "
              f"{q['p50']:8.1f} {q['p95']:8.1f} {q['p99']:8.1f} {q['max']:8.1f} | "
              f"{x['p50']:7.1f}")

    cmd("dservTiming off")


if __name__ == "__main__":
    main()

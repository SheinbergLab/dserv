# Finding a dserv

Two mechanisms, for two situations. Neither needs the other.

| You are...                                  | Use                                   |
|---------------------------------------------|---------------------------------------|
| on the same link as the dserv (lab LAN, rig switch, a box's own Wi‑Fi) | **mDNS**: browse for `_dserv._tcp` |
| anywhere else, or on Wi‑Fi with client isolation | **the registry**: `GET <registry>/api/v1/mesh?workgroup=<wg>` |

A client that wants "the local dserv if there is one, otherwise something
sensible" does them in that order, then falls back to a configured host.

## On the link: mDNS / DNS-SD

Every dserv advertises one DNS-SD record through the host's own mDNS
responder: mDNSResponder on macOS, avahi-daemon on Linux (via its Bonjour
shim, `libavahi-compat-libdnssd1`, a dependency of the .deb). dserv itself
sends no multicast; `modules/mdns` hands the record over and sleeps on the
responder's IPC socket. The registration is made by the `mesh` subprocess
(`config/meshconf.tcl`) and is independent of the registry heartbeat and
of offline mode.

| Field     | Value                                                       |
|-----------|-------------------------------------------------------------|
| type      | `_dserv._tcp`                                               |
| instance  | the hostname (the responder appends ` (2)` on a collision)  |
| port      | `2560`, the message listener (what `dservctl` / `essctrl` use) |
| TXT `web` | the HTTP/WebSocket port, `2565`                             |
| TXT `newline` | the newline listener, `2570`                            |
| TXT `dp`  | the datapoint pub/sub listener, `4620` (`%reg` / `%match`; what a streaming client such as VideoStream subscribes on) |
| TXT `ssl` | `1` if the web port is HTTPS                                |
| TXT `wg`  | the workgroup (absent when none is declared)                |
| TXT `ver` | dserv version                                               |

Live state (running system, subject, ESS status) is deliberately **not**
in the TXT record. It changes every trial; the advertisement never does.
Ask dserv once connected (`dservGet ess/system`, or the web API).

The outcome is published as `mesh/mdns/state`:
`registered <granted name>`, `pending`, `error <dns_sd code>`, `unregistered`,
or `off no mdns module` on a build without the shim.

### Browsing

```bash
# macOS
dns-sd -B _dserv._tcp                       # list instances
dns-sd -L "hb-4" _dserv._tcp                # one instance's port + TXT

# Linux
avahi-browse -r -t _dserv._tcp              # list + resolve, then exit
```

```python
# Python, any platform: pip install zeroconf
from zeroconf import Zeroconf, ServiceBrowser, ServiceListener

class L(ServiceListener):
    def add_service(self, zc, type_, name):
        info = zc.get_service_info(type_, name)
        txt = {k.decode(): v.decode() for k, v in info.properties.items()}
        print(info.server, info.parsed_addresses(), info.port, txt)
    def remove_service(self, *_): pass
    def update_service(self, *_): pass

zc = Zeroconf()
ServiceBrowser(zc, "_dserv._tcp.local.", L())
```

Swift clients use `NWBrowser(for: .bonjour(type: "_dserv._tcp", domain: nil))`;
.NET has the `Zeroconf` package; Go has `grandcat/zeroconf` (what
`tools/dserv-term -discover` uses).

More than one dserv on a subnet is normal: each answers with its own
instance name, and a client filters on `wg` or lets the user pick.

### When it does not work

- **Nothing answers but dserv is up**: on Linux check
  `systemctl status avahi-daemon`; dserv logs `mDNS advertisement
  unavailable` at startup and `mesh/mdns/state` reads `error ...`.
- **Wi‑Fi with client isolation** (many campus networks) drops multicast
  between clients. Nothing on either end can fix that; use the registry.
- **Two subnets**: mDNS is link-local by design. Use the registry.

## Off the link: the registry

Every dserv in a workgroup POSTs a heartbeat to its registry every 5 s
(`config/meshconf.tcl`; the registry URL and workgroup come from the rig's
settings, `registry url` / `registry workgroup`). The registry keeps the
list and hands it back:

```bash
curl 'https://dserv.net/api/v1/mesh?workgroup=brown-sheinberg'
```

Each node carries `hostname`, `ip`, `port` (the web port), `ssl`, `status`
and the `customFields` the box chose to send (system, protocol, subject,
and on Linux the network path). A local `dserv-agent` serves the same list
from its cache at `http://localhost/api/v1/mesh`, and every dserv
republishes it as the `mesh/peers` datapoint.

Caveats: a box in offline mode sends no heartbeats and is absent here; the
`ip` is whatever the box believes its own address is (a Mac may report
`127.0.0.1`), so filter by subnet with that in mind.

## History

Core dserv once carried a UDP heartbeat broadcaster and an in-process mDNS
responder; both left in December 2025 when discovery moved to the
registry. The mDNS advertisement returned in October 2026 as
`modules/mdns`, this time through the OS responder. The extio boxes still
use a UDP beacon (`modules/extiodisc`, port 5011) because a microcontroller
cannot cheaply run a responder; that is a different direction (dserv
finding boxes) and is unrelated to finding a dserv.

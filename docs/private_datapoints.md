# Private datapoints

A datapoint carrying `DSERV_DPOINT_PRIVATE_FLAG` (0x100, see
`src/Datapoint.h`) is **write-only from every client's point of view**:
it reaches log files and nothing else. The motivating case is camera
imagery — frames should be recordable on the rig but must not be
viewable by anything that can reach dserv's ports.

## Semantics

| Surface | Private point behavior |
|---|---|
| Log clients (`dservLogger*`) | **Delivered** — flags word (with the PRIVATE bit) is written to the `.ds` file |
| Subscription fan-out (`%reg` TCP clients, websocket subscriptions, Tcl `dservAddMatch`/`dpointSetScript`) | Never delivered (`SendTable::forward_dpoint`) |
| `dservGet` (any interp), `%get` (TCP), websocket `get` | Denied — Tcl gets `dpoint "..." is private`, wire protocols report not-found |
| Trigger scripts (`triggerAdd`) | Never fired |
| `dservWhen` level-seeding | Skipped |
| `dservKeys`, `dservInfo`, `dservTimestamp`, `dservExists` | **Visible** — names and metadata stay readable so a rig can confirm a private producer is alive without seeing its payload |
| `dservCopy` | Allowed; the copy keeps the PRIVATE bit |

Because *no* Tcl interp can read the payload, a network client that can
eval Tcl (essctrl, websocket `eval`) cannot launder a private point by
republishing it under a new name. The only code that can touch the
payload is compiled code loaded from the rig's own filesystem (the
producing module, `dpoint_process` processors) and the logger — i.e. the
root of trust is host access, not network access.

## Producing private points

From a C/C++ module, set the bit before publishing:

```c
dp->flags |= DSERV_DPOINT_PRIVATE_FLAG;
tclserver_set_point(tclserver, dp);
```

The camera module does this for all image payloads (PPM/JPEG) via
`tclserver_set_point_private()`; its `frame_notify` and `*/meta` JSON
points stay public.

From Tcl (any interp): `dservSetPrivate varname value` — the string
equivalent of `dservSet`. Exposing this is safe: marking a point
private only ever restricts access, and overwriting an existing name
replaces the data rather than revealing it.

There is deliberately no way to *unset* privacy on a live point; each
publish carries its own flags, so a producer that stops setting the bit
starts publishing public values (of its own data only).

## What this does not protect against

- Anyone with filesystem/SSH access to the rig (they can read the log
  files, or install a module).
- A malicious compiled module or dpoint processor.
- Traffic analysis: names, types, sizes and update times remain visible.
- Integrity: writes are not gated, so any client can still overwrite or
  `dservClear` a private name (replacing data with their own — no leak,
  but not tamper-proof either).

## Implementation notes

- Attribute bits live in `DSERV_DPOINT_ATTR_MASK` (0xFF00). The logger
  historically treated *any* nonzero flags as "statically owned control
  point — don't copy"; those ownership tests in `LogClient::log_point`
  mask attribute bits out, or a private point would be freed twice.
  Any future code that dispatches on `flags != 0` must do the same.
- Wire parsers (`dpoint_from_string`, `dpoint_from_binary`) zero flags,
  so the bit cannot be injected from the network; outgoing wire formats
  don't carry flags, so allowed consumers see no difference.
- `DatapointTable::update()`'s in-place fast path copies flags along
  with the timestamp, so the table always reflects the latest write.
- The bit lands in the `.ds` log flags word, so a future log replay
  that re-sets points would re-create them private.
- Tests: `tests/test_private.tcl` (--tscript: get/keys/copy/trigger
  semantics) and `tests/test_private_logger.tcl` (--cscript: dpoint
  script suppression + byte-level log verification).

## Future: privileged consumers

If an in-process Tcl consumer ever needs to read private payloads
(gaze-on-frame analysis, an authenticated viewer relay), the plan is a
per-subprocess read capability with three locks: grantable only while
startup config is being sourced (one-way latch), only to subprocesses
with no listening ports, and `send`/`send_noreply` into a privileged
subprocess refused from unprivileged interps. Until something needs
that, it stays unbuilt.

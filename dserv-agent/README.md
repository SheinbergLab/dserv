# dserv-agent

A standalone management agent for dserv systems. Provides HTTP/WebSocket APIs for remote management, updates, and monitoring. Runs independently of dserv so you can manage the system even when dserv is down.

## Features

- **Web Dashboard**: Built-in web interface for system management
- **Service Control**: Start/stop/restart dserv via API or web UI
- **Update Management**: Check for and install dserv updates with progress streaming
- **System Monitoring**: View system status, load, and service health
- **File Management**: Upload files to target systems
- **System Reboot**: Optionally allow remote system reboot
- **WebSocket Proxy**: Forward commands to dserv when it's running
- **Authentication**: Optional token-based authentication

## Installation

### From Source

```bash
# Clone and build
git clone https://github.com/SheinbergLab/dserv-agent
cd dserv-agent
make build

# Install (requires root)
sudo make install

# Configure authentication (recommended)
sudo cp /etc/dserv-agent/env.example /etc/dserv-agent/env
sudo nano /etc/dserv-agent/env

# Enable and start
sudo systemctl enable dserv-agent
sudo systemctl start dserv-agent
```

### Cross-Compilation

```bash
# Build for all platforms
make build-all

# Or specific target
make build-linux-arm64
```

## Configuration

### Command Line Options

```
Usage: dserv-agent [options]

Options:
  -listen string
        HTTP listen address (default ":2580")
  -token string
        Bearer token for authentication
  -service string
        systemd service name for dserv (default "dserv")
  -dserv-ws string
        WebSocket URL to dserv (default "ws://localhost:2570/ws")
  -allow-reboot
        Allow system reboot command
  -upload-dir string
        Directory for file uploads (default "/tmp/dserv-uploads")
  -timeout duration
        HTTP client timeout (default 5m0s)
  -v    Verbose output
```

### Environment Variables

Authentication token can be set via environment:

```bash
export DSERV_AGENT_TOKEN="your-secret-token"
```

Or in `/etc/dserv-agent/env`:

```
DSERV_AGENT_TOKEN=your-secret-token
```

Generate a secure token:
```bash
openssl rand -hex 32
```

## API Reference

All API endpoints require authentication if a token is configured. Pass the token as:
- Header: `Authorization: Bearer <token>`
- Query param: `?token=<token>`

### HTTP Endpoints

#### GET /api/status
Returns comprehensive system and service status.

Query params:
- `updates=true` - Include update availability info

Response:
```json
{
  "agent": {
    "version": "0.1.0",
    "uptime": "2h30m",
    "startTime": "2024-01-15T10:30:00Z"
  },
  "dserv": {
    "status": "active",
    "version": "v1.2.3",
    "pid": 1234,
    "connected": true
  },
  "system": {
    "hostname": "lab-pi-01",
    "os": "linux",
    "arch": "arm64",
    "uptime": "5d2h",
    "loadAvg": "0.15 0.20 0.18"
  }
}
```

#### POST /api/dserv/{action}
Control dserv service. Actions: `start`, `stop`, `restart`, `status`

#### GET /api/update/check
Check for available dserv updates.

#### POST /api/update/install
Install latest dserv update. Progress streamed via WebSocket.

#### POST /api/system/reboot
Reboot the system (requires `-allow-reboot` flag).

Query params:
- `delay=5` - Seconds to wait before reboot (default: 5)

#### POST /api/upload
Upload a file. Multipart form with `file` field.

Form fields:
- `file` - The file to upload (required)
- `path` - Destination path (optional)

#### GET /api/files/{path}
List directory or download file.

#### DELETE /api/files/{path}
Delete a file.

### WebSocket API

Connect to `/ws` (with `?token=<token>` if auth enabled).

#### Message Format

Request:
```json
{
  "type": "action_type",
  "id": "optional-request-id",
  "action": "optional-sub-action",
  "payload": {}
}
```

Response:
```json
{
  "type": "action_type_response",
  "id": "request-id",
  "success": true,
  "data": {},
  "error": "error message if failed"
}
```

#### Message Types

**status** - Get current status
```json
{"type": "status"}
```

**dserv** - Control dserv service
```json
{"type": "dserv", "action": "start|stop|restart|status"}
```

**update_check** - Check for updates
```json
{"type": "update_check"}
```

**update_install** - Install update (progress pushed automatically)
```json
{"type": "update_install"}
```

**proxy** - Forward message to dserv
```json
{"type": "proxy", "payload": {"cmd": "eval", "script": "ess::start"}}
```

#### Push Messages

The server pushes these messages during operations:

**update_progress**
```json
{
  "type": "update_progress",
  "data": {"stage": "downloading", "asset": "dserv_v1.2.3_arm64.deb"}
}
```

**update_complete**
```json
{
  "type": "update_complete",
  "success": true,
  "data": {"version": "v1.2.3", "status": {...}}
}
```

**update_error**
```json
{
  "type": "update_error",
  "error": "Installation failed: ..."
}
```

**dserv_message** - Messages from dserv (when proxying)
```json
{
  "type": "dserv_message",
  "data": {...}
}
```

## Environment for GITHUB_TOKEN
```
sudo mkdir -p /etc/dserv-agent
printf 'GITHUB_TOKEN=%s\n' 'github_pat_NEW_TOKEN_HERE' | sudo tee /etc/dserv-agent/env >/dev/null
sudo chown root:root /etc/dserv-agent/env
sudo chmod 600 /etc/dserv-agent/env
```

Then in the service file:
```
EnvironmentFile=-/etc/dserv-agent/env
```

## Web Interface

Access the dashboard at `http://<host>:2580/`

Features:
- Real-time system and service status
- Start/stop/restart dserv
- Check for and install updates with progress
- System reboot (if enabled)

## Security Considerations

1. **Always use authentication in production** - Generate a strong token
2. **Use HTTPS** - Put behind a reverse proxy with TLS, or modify to use TLS directly
3. **Firewall** - Restrict access to the agent port
4. **Reboot permission** - Only enable `-allow-reboot` if needed

## Integration with dserv

The agent complements dserv by providing out-of-band management:

- **When dserv is running**: Agent can proxy commands to dserv
- **When dserv is down**: Agent can restart dserv, install updates, reboot system
- **Dashboard**: Provides a management UI independent of dserv's web interface

Recommended port setup:
- dserv: 2565 (HTTP), 2570 (internal)
- dserv-agent: 2580

## extio firmware shelf (server mode)

A registry-mode agent (e.g. dserv.net) can house versioned firmware for the
extio box (`wiznet-io/`) so bench tools (`tools/extio-setup`) and, later, the
on-box A/B updater (`wiznet-io/OTA.md`) all pull the same artifact from one
place. Enable it with `--firmware-dir <root>`. Implemented in `firmware.go`.

**Channels.** `dev` is mutable (overwrite allowed, `-dirty` builds accepted);
every other channel (`stable`, `extio-fw-vN`, …) is immutable — a version's
target can be published once, and a `-dirty` build is refused.

**Endpoints.**

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/api/firmware/extio` | open | all channels + their version manifests |
| GET | `/api/firmware/extio/<channel>` | open | one channel (`latest` + versions) |
| GET | `/api/firmware/extio/<channel>/<version>` | open | one version manifest |
| GET | `/firmware/extio/<channel>/<version>/<file>` | open | download a `.uf2`/`.bin` |
| POST | `/api/firmware/extio/<channel>` | token | publish one target image |

Read side is unauthenticated on purpose (a fresh box / bench tool needs bare
access, same as `/api/releases`). **Publish is gated by a dedicated
`--firmware-token`** (or `DSERV_AGENT_FIRMWARE_TOKEN` env, so the secret can
live in the `EnvironmentFile` instead of `ExecStart`/`ps`) — this locks
publishing *without* touching the rest of the agent's API. If no firmware token
is set, publish falls back to the general `--token`, which is itself open when
unset (matching every other endpoint). On a public server, set one:

```
# /etc/dserv-agent/env  (chmod 600)
DSERV_AGENT_FIRMWARE_TOKEN=<secret>
```

**Board matrix.** The box's `build.sh` has two independent axes plus a
compatibility key, so each image carries three identity fields:

| Field | Source | Role |
|---|---|---|
| `build` | `build.sh` target name | **unique key** — the on-box updater fetches the image whose `build` == its own baked build, so it never strays onto another variant |
| `board` | `PICO_BOARD` | **hard compatibility filter** — bench flash of an unknown board and the on-box updater both refuse a `board` mismatch (a pimoroni image must never land on a sparkfun board) |
| `variant` | `BOX_TARGET` | descriptive role (`usb\|dual\|w6300\|pico2w`); not unique on its own — the three WiFi builds all share `pico2w`, which is why `build` is the key |

`build` values today: `usb`, `dual`, `w6300` (all `board=pico2`), `pico2w`,
`picoplus2w`, `thingplus` (each a distinct WiFi `board`). `otaCapable` is set
per image — WiFi builds are `false` today (bigger images, no `copy_to_ram`),
so they ship through the shelf for **bench flashing** but are gated out of
self-update. One version holds all its build images.

**Publish** is multipart, called once per build `.uf2`:
fields `version` (req, `git describe`), `build` (req, the `build.sh` target),
`board`, `variant`, `ota` (`1`/`true`), `dirty` (`1`/`true`), `notes`; files
`uf2` (req) and optional `bin` (flat slot image for the on-box updater).
sha256 is computed server-side, never trusted from the client.

```bash
curl -F version=extio-fw-v3 -F build=dual -F board=pico2 -F variant=dual \
     -F uf2=@wiznet-io/dist/wizchip_dserv_config_dual.uf2 \
     -H "Authorization: Bearer $DSERV_AGENT_FIRMWARE_TOKEN" \
     https://dserv.net/api/firmware/extio/stable
```

**Manifest contract** (`manifest.json`, one per version; the puller verifies
`sha256` before flashing):

```json
{
  "family": "extio", "channel": "stable", "version": "extio-fw-v3",
  "dirty": false, "publishedAt": "2026-07-12T23:15:09Z",
  "images": [
    {"build":"dual","board":"pico2","variant":"dual","otaCapable":false,
     "file":"wizchip_dserv_config_dual.uf2","size":312480,"sha256":"…",
     "bin":"…","binSha256":"…"}
  ]
}
```

**On-disk layout** under the firmware root:

```
<root>/extio/<channel>/<version>/<file>.uf2   # image(s), one per target
<root>/extio/<channel>/<version>/manifest.json
<root>/extio/<channel>/latest.json            # {"version": "…"} pointer
```

Publisher: `wiznet-io/build.sh <target> --push` builds a target and publishes
just that image (dev channel by default; `PUSH_CHANNEL`/`--channel` to override,
`FW_SHELF_URL` for a non-default host, `DSERV_AGENT_FIRMWARE_TOKEN` required).
Next: the consumer — extio-setup's Firmware panel gains a "pull from dserv.net"
path that reuses the existing BOOTSEL flash.

## Development

```bash
# Run with verbose output
make run

# Format code
make fmt

# Build for testing
make build
./dserv-agent -v -token test123
```

## License

MIT License - see LICENSE file

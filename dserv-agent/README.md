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

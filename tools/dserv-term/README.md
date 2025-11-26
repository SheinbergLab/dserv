# Dserv Terminal - Simplified Version

This is a simplified, terminal-friendly version of the Tcl debugger console. It's designed for reliable use over SSH without the complexity of split-pane viewports.

## What's Different

### Simplified Design
- **Single scrolling output**: Uses terminal's native scrollback instead of viewports
- **No layout management**: No viewport height calculations that can go wrong
- **Inline events**: Async events appear inline with `[event:source]` tags
- **Terminal scrollbar**: Just use your terminal's native scroll

### Same Functionality
- ✅ Multi-interpreter support (switch with `:use`)
- ✅ Command routing (`/interp cmd`)
- ✅ Meta commands (`:help`, `:list`, etc.)
- ✅ Tab completion
- ✅ Command history (arrow keys)
- ✅ Stack traces (toggle with `:trace`)
- ✅ TCP and WebSocket backends
- ✅ Mesh discovery (TCP mode)

## Building

```bash
go build -o dserv-term
```

## Usage

```bash
# Auto-discover and connect
./dserv-term

# Connect to specific host
./dserv-term -host 192.168.1.100

# Specify port (default 2560)
./dserv-term -port 2560

# WebSocket mode
./dserv-term -mode ws -ws ws://localhost:9000/debug

# Simulation mode (for testing)
./dserv-term -sim
```

## Commands

### Meta Commands
- `:help` - Show help
- `:list` - List available interpreters
- `:use <name>` - Switch active interpreter
- `:trace` - Toggle stack traces on errors
- `:clear` - Clear screen
- `:refresh` - Refresh tab completions
- `:scan` - Actively scan for hosts (waits 3 seconds)
- `:hosts` - List discovered hosts

- `:connect <n>` - Connect to host number
- `:reconnect` - Reconnect to current host (also attempted automatically after disconnect)
- `:quit` - Exit

### Command Routing
- `command` - Send to active interpreter
- `/interp command` - Send to specific interpreter

### Keyboard Shortcuts
- **Tab** - Autocomplete commands
- **↑/↓** - Navigate command history
- **Ctrl+L** - Clear screen
- **Ctrl+C** - Quit

## Prompt Format

```
● [dserv] > your command here
```

- **●** - Connection status (green=connected, red=disconnected) - updates dynamically
- **[dserv]** - Active interpreter (colored)
- **>** - Prompt

## Output Format

```
● [dserv] > info commands
→ after append array auto_execok ...

[event:ess] System RUNNING

✗ can't read "foo": no such variable
```

- **● [interp] >** - Command you entered (● shows connection status)
- **→** - Response from active interpreter
- **[interp] →** - Response from different interpreter
- **[event:source]** - Async events
- **✗** - Errors

## Advantages Over Viewport Version

1. **More reliable over SSH** - No viewport rendering glitches
2. **Simpler code** - Less complexity = fewer bugs
3. **Native scrolling** - Use terminal features you're familiar with
4. **Clearer output** - No competing panes for attention
5. **Works at any size** - No height calculation issues

## For Web/GUI Usage

For a prettier web interface with split panes and rich formatting, use the WebSocket backend with a custom web client. This terminal version is optimized for SSH and command-line use where simplicity and reliability are priorities.

## Automatic Reconnection

The terminal handles connection issues transparently:

**Silent reconnection on command failure:**
- If you send a command while disconnected, it automatically reconnects and retries
- You won't see any error messages if reconnection succeeds
- The command just works as if nothing happened

**Background reconnection on unexpected disconnect:**
- If the connection drops unexpectedly (server crash), you'll see a disconnect message
- After 2 seconds, it automatically reconnects in the background
- If successful, you'll see "✓ Reconnected successfully"
- UI remains responsive during reconnection

**Example of silent reconnection:**
```
● [dserv] > pwd                    ← Server is down
→ /Users/sheinb/src/dserv/build   ← Silently reconnected and executed!
```

**Example of visible reconnection:**
```
● [dserv] > pwd
✗ Disconnected: connection reset    ← Server crashed mid-command
● [dserv] >                          ← 2 seconds later...
✓ Reconnected successfully           ← Automatically reconnected
```

Manual reconnection is still available with `:reconnect` if needed.


## Tab Completion Cache

Tab completion uses a cache of available commands, procs, and variables from each interpreter. The cache is automatically refreshed:

- **On connection/reconnection**: Refreshes the **active** interpreter's cache only
- **When switching interpreters** (`:use <n>`): Refreshes the **new** interpreter's cache
- **Manual refresh**: Use `:refresh` to update the active interpreter's cache

The cache enables fast tab completion without querying the backend on every keystroke.

**Note**: Only the active interpreter's completions are shown. When using `/interp command` syntax to send to a different interpreter, that interpreter's cache will be loaded on-demand if needed.

## Tcl Namespace Commands

Tcl namespace commands (like `::ess::start`) work normally. The `::` prefix is recognized as Tcl namespace syntax, not a meta command:

```
● [dserv] > ::ess::start        ← Works! Sent to Tcl
● [dserv] > :help               ← Meta command
```

Tab completion works with namespace commands - try typing `::ess::` and hitting Tab to see available commands in that namespace.
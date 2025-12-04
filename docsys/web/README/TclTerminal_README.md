# TclTerminal Component

A lightweight, reusable terminal component for Tcl interpreters with command history, tab completion, and VS Code-style theming.

## Features

✅ **Command History** - Navigate with ↑/↓ arrows, persistent across sessions
✅ **Tab Completion** - Auto-complete commands, procedures, and variables
✅ **Keyboard Shortcuts** - Ctrl+C (clear line), Ctrl+L (clear terminal)
✅ **Text Selection** - Select and copy terminal output
✅ **Dark/Light Themes** - VS Code inspired color schemes
✅ **Minimal Dependencies** - Just needs a WebSocket connection
✅ **Clean API** - Simple to integrate into any page

## Files

- `TclTerminal.js` - Main component class (~500 lines)
- `TclTerminal.css` - Styling (dark/light themes, compact mode)
- `TclTerminal_demo.html` - Working example with dserv

## Quick Start

### 1. Include the files

```html
<link rel="stylesheet" href="TclTerminal.css">
<script src="TclTerminal.js"></script>
```

### 2. Create a container

```html
<div id="terminal" style="height: 500px;"></div>
```

### 3. Initialize with connection

```javascript
// Assumes you have a DservConnection
const terminal = new TclTerminal('terminal', connection, {
    interpreter: 'dserv',
    welcomeMessage: 'Welcome to Tcl Terminal',
    tabCompletion: true
});
```

## Options

```javascript
new TclTerminal('container-id', connection, {
    interpreter: 'dserv',              // Interpreter name for prompt
    useLinkedSubprocess: true,         // Use linked subprocess (default) or connect directly
    showWelcome: true,                 // Show welcome message on init
    welcomeMessage: 'Welcome...',      // Custom welcome text
    historyKey: 'tcl-terminal-history',// localStorage key for history
    maxHistory: 100,                   // Max history entries
    tabCompletion: true                // Enable tab completion
});
```

### Connection Modes

**Linked Subprocess (default):**
```javascript
const terminal = new TclTerminal('terminal', conn, {
    useLinkedSubprocess: true  // or just omit, it's the default
});
```
- Creates a private subprocess that auto-cleans up when page closes
- Isolated from other sessions
- Perfect for documentation/tutorial pages

**Direct Connection:**
```javascript
const terminal = new TclTerminal('terminal', conn, {
    useLinkedSubprocess: false,
    interpreter: 'dserv'  // or any subprocess name
});
```
- Connects directly to main dserv or a named subprocess
- Shared state across sessions
- Useful for system administration or debugging

## Public API

```javascript
// Write output
terminal.write('Hello', 'info');     // Types: result, error, info, warning, command
terminal.showResult('Output');
terminal.showError('Error message');
terminal.showInfo('Info message');
terminal.showWarning('Warning');

// Control
terminal.clear();                     // Clear terminal
terminal.focus();                     // Focus input
terminal.setInterpreter('newname');  // Change interpreter name
```

## Usage with dserv

### Basic Integration

```javascript
// Create connection with linked subprocess
const conn = new DservConnection({
    createLinkedSubprocess: true,
    dservHost: 'localhost:2565',
    forceSecure: true
});

await conn.connect();
await conn.sendToLinked('package require dlsh');

// Create terminal
const terminal = new TclTerminal('terminal', conn, {
    interpreter: conn.getLinkedSubprocess()
});
```

### In Documentation Pages

Perfect for interactive Tcl examples:

```html
<h3>Try It</h3>
<div id="tcl-console" style="height: 300px;"></div>

<script>
// Initialize after connection
const console = new TclTerminal('tcl-console', conn, {
    welcomeMessage: 'Try Tcl commands here',
    showWelcome: true
});
</script>
```

### Multiple Terminals

You can have multiple terminals on one page:

```javascript
const terminal1 = new TclTerminal('term1', conn1, {
    interpreter: 'interpreter1',
    historyKey: 'term1-history'
});

const terminal2 = new TclTerminal('term2', conn2, {
    interpreter: 'interpreter2',
    historyKey: 'term2-history'
});
```

## Features in Detail

### Command History

- Stored in localStorage (persistent across sessions)
- Navigate with ↑/↓ arrow keys
- Separate history per terminal (via `historyKey` option)
- Maximum 100 entries by default (configurable)

### Tab Completion

Press Tab to auto-complete:
- Commands, procedures, variables
- **Context-aware**: Completes inside nested commands like `dl_tcllist [dl_fro<TAB>`
- Variable names after `set`, namespace names after `namespace eval`, etc.

**Requirements:**
The backend Tcl interpreter must have a `complete` command that handles context-aware completion. This command receives the current input and returns a list of possible completions.

Example backend implementation:
```tcl
proc complete {input} {
    # Your context-aware completion logic
    # Return Tcl list of matches
}
```

Press Tab multiple times to cycle through matches.

### Keyboard Shortcuts

- **Enter** - Execute command
- **↑/↓** - Navigate history
- **Tab** - Auto-complete (cycles through matches)
- **Ctrl+C** - Clear current line
- **Ctrl+L** - Clear terminal

### Text Selection

- Click and drag to select text
- Cmd/Ctrl+C to copy
- Automatically cleans up extra newlines when copying

### Themes

**Dark Theme (default):**
```css
.tcl-terminal { /* Dark VS Code theme */ }
```

**Light Theme:**
```javascript
document.getElementById('terminal').classList.add('light');
```

**Compact Mode:**
```javascript
document.getElementById('terminal').classList.add('compact');
```

## Styling

Override CSS variables for custom colors:

```css
.tcl-terminal {
    background: #000000;
    color: #00ff00;
}

.tcl-terminal-command {
    color: #ffff00;
}

.tcl-terminal-error {
    color: #ff0000;
}
```

Or use the built-in light theme by adding the `light` class to the container.

## Comparison to Original

**Removed (app-specific):**
- WebSocket event listeners (uses connection directly)
- Interpreter switching UI (single interpreter focus)
- Datapoint subscriptions
- Error logging integration
- App-specific command parsing (`/interp` syntax)

**Kept (reusable):**
- Command history with localStorage
- Tab completion
- All keyboard shortcuts
- Text selection handling
- Theme support
- Clean API

**Simplified:**
- Single interpreter per terminal instance
- Direct connection object instead of event emitter
- Cleaner API surface
- Fewer dependencies

## Browser Support

Works in all modern browsers:
- Chrome/Edge 90+
- Firefox 88+
- Safari 14+

## Future Enhancements

Possible additions:
- ANSI color code support
- Command aliases
- Custom tab completion handlers
- Multi-line input mode
- Command output folding
- Export terminal history

## License

Free to use in your dserv projects.

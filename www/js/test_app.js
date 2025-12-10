/**
 * test_app.js - Test application for docs API
 */

// DOM elements
const statusIndicator = document.getElementById('status-indicator');
const statusText = document.getElementById('status-text');
const btnConnect = document.getElementById('btn-connect');
const btnDisconnect = document.getElementById('btn-disconnect');
const customCmd = document.getElementById('custom-cmd');
const btnSend = document.getElementById('btn-send');
const resultPre = document.getElementById('result');
const resultTime = document.getElementById('result-time');
const btnCopy = document.getElementById('btn-copy');
const logDiv = document.getElementById('log');
const btnClearLog = document.getElementById('btn-clear-log');

// Connection
let conn = null;

/**
 * Update connection status UI
 */
function updateStatus(status, message) {
    statusIndicator.className = 'status-indicator ' + status;
    statusText.textContent = message;
    
    const isConnected = status === 'connected';
    btnConnect.disabled = isConnected;
    btnDisconnect.disabled = !isConnected;
    btnSend.disabled = !isConnected;
    
    document.querySelectorAll('.api-test').forEach(btn => {
        btn.disabled = !isConnected;
    });
    
    log(message, status === 'connected' ? 'success' : status === 'error' ? 'error' : 'info');
}

/**
 * Add log entry
 */
function log(message, type = '') {
    const entry = document.createElement('div');
    entry.className = 'log-entry ' + type;
    
    const time = document.createElement('span');
    time.className = 'time';
    time.textContent = new Date().toLocaleTimeString();
    
    entry.appendChild(time);
    entry.appendChild(document.createTextNode(message));
    
    logDiv.insertBefore(entry, logDiv.firstChild);
    
    // Limit log entries
    while (logDiv.children.length > 50) {
        logDiv.removeChild(logDiv.lastChild);
    }
}

/**
 * Format JSON with syntax highlighting
 */
function formatJson(obj) {
    const json = JSON.stringify(obj, null, 2);
    return json
        .replace(/"([^"]+)":/g, '<span class="json-key">"$1"</span>:')
        .replace(/: "([^"]*)"/g, ': <span class="json-string">"$1"</span>')
        .replace(/: (\d+)/g, ': <span class="json-number">$1</span>')
        .replace(/: (true|false)/g, ': <span class="json-boolean">$1</span>')
        .replace(/: (null)/g, ': <span class="json-null">$1</span>');
}

/**
 * Display result
 */
function showResult(data, duration) {
    resultTime.textContent = `${duration}ms`;
    
    try {
        const obj = typeof data === 'string' ? JSON.parse(data) : data;
        resultPre.innerHTML = formatJson(obj);
    } catch (e) {
        // Not JSON, show as plain text
        resultPre.textContent = data;
    }
}

/**
 * Send command and display result
 */
async function sendCommand(command) {
    if (!conn || !conn.connected) {
        log('Not connected', 'error');
        return;
    }
    
    log(`→ ${command}`, 'info');
    const start = performance.now();
    
    try {
        const result = await conn.send(command);
        const duration = Math.round(performance.now() - start);
        showResult(result, duration);
        log(`← Response (${duration}ms)`, 'success');
    } catch (e) {
        log(`Error: ${e.message}`, 'error');
        resultPre.textContent = `Error: ${e.message}`;
        resultTime.textContent = '';
    }
}

/**
 * Connect to dserv
 */
async function connect() {
    // Detect if we're on the dev server (different port than dserv)
    const isDev = window.location.port === '8000';
    
    conn = new DservConnection({
        subprocess: 'docs',
        // In dev mode, connect to dserv on port 2565 with wss://
        dservHost: isDev ? 'localhost:2565' : null,
        forceSecure: isDev,  // Force wss:// even though dev server is http
        onStatus: updateStatus,
        onMessage: (msg) => log(`Unsolicited: ${msg}`),
        onError: (err) => log(`Error: ${err}`, 'error'),
    });
    
    try {
        await conn.connect();
        // Connect editor for completions
        connectEditorToWs();
    } catch (e) {
        log(`Connection failed: ${e.message}`, 'error');
    }
}

/**
 * Disconnect
 */
function disconnect() {
    if (conn) {
        conn.disconnect();
        conn = null;
    }
}

// Event listeners
btnConnect.addEventListener('click', connect);
btnDisconnect.addEventListener('click', disconnect);

btnSend.addEventListener('click', () => {
    const cmd = customCmd.value.trim();
    if (cmd) {
        sendCommand(cmd);
    }
});

customCmd.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        const cmd = customCmd.value.trim();
        if (cmd) {
            sendCommand(cmd);
        }
    }
});

document.querySelectorAll('.api-test').forEach(btn => {
    btn.addEventListener('click', () => {
        const cmd = btn.dataset.cmd;
        customCmd.value = cmd;
        sendCommand(cmd);
    });
});

btnCopy.addEventListener('click', () => {
    const text = resultPre.textContent;
    navigator.clipboard.writeText(text).then(() => {
        btnCopy.textContent = '✓';
        setTimeout(() => { btnCopy.textContent = '📋'; }, 1000);
    });
});

btnClearLog.addEventListener('click', () => {
    logDiv.innerHTML = '';
});

// Initial state
updateStatus('disconnected', 'Click Connect to start');
log('Ready - click Connect to connect to dserv');

// Initialize Tcl Editor
let tclEditor = null;

async function initEditor(useEmacs = false) {
    // Destroy existing editor if any
    if (tclEditor) {
        tclEditor.destroy();
    }
    
    tclEditor = new TclEditor('tcl-editor', {
        theme: 'dark',
        fontSize: '14px',
        tabSize: 4,
        keybindings: useEmacs ? 'emacs' : 'default',
    });
    
    // Wait for editor to be ready
    document.getElementById('tcl-editor').addEventListener('editor-ready', () => {
        // Set some sample code
        tclEditor.setValue(`# Test Tcl code
# Try typing dl_ and pressing Tab for completion

proc hello {name} {
    return "Hello, $name!"
}

hello "World"
`);
        
        // Connect to WebSocket if available
        connectEditorToWs();
        
        log('Tcl editor initialized (CodeMirror 6)', 'info');
    }, { once: true });
}

// Initialize editor on page load
initEditor(false);

// Wire up editor buttons
document.getElementById('btn-run-editor').addEventListener('click', async () => {
    if (!tclEditor) return;
    const code = tclEditor.getValue();
    if (code.trim()) {
        await sendCommand(code);
    }
});

document.getElementById('btn-format-editor').addEventListener('click', () => {
    if (tclEditor) {
        tclEditor.format();
        log('Code formatted', 'info');
    }
});

document.getElementById('btn-lint-editor').addEventListener('click', () => {
    if (tclEditor) {
        const result = tclEditor.lint();
        if (result.isValid) {
            log('Lint: No issues found ✓', 'success');
        } else {
            result.errors.forEach(err => {
                log(`Lint error (line ${err.line}): ${err.message}`, 'error');
            });
            result.warnings.forEach(warn => {
                log(`Lint warning (line ${warn.line}): ${warn.message}`, 'info');
            });
        }
        showResult(result, 0);
    }
});

document.getElementById('btn-clear-editor').addEventListener('click', () => {
    if (tclEditor) {
        tclEditor.setValue('');
        tclEditor.focus();
    }
});

// Emacs mode toggle
document.getElementById('emacs-mode').addEventListener('change', (e) => {
    const useEmacs = e.target.checked;
    const currentCode = tclEditor ? tclEditor.getValue() : '';
    
    initEditor(useEmacs).then(() => {
        // Restore code after reinit (with delay for editor-ready)
        setTimeout(() => {
            if (tclEditor && currentCode) {
                tclEditor.setValue(currentCode);
            }
            log(`Keybindings: ${useEmacs ? 'Emacs' : 'Default'}`, 'info');
        }, 100);
    });
});

// Connect editor to WebSocket when connected
function connectEditorToWs() {
    if (tclEditor && conn && conn.connected) {
        // Create an adapter for the editor's expected ws.eval() interface
        const wsAdapter = {
            eval: async (cmd) => {
                try {
                    const result = await conn.send(cmd);
                    return { result };
                } catch (e) {
                    return { error: e.message };
                }
            }
        };
        tclEditor.setWebSocket(wsAdapter);
        log('Editor connected to WebSocket for completions', 'info');
    }
}

// Auto-connect on load (optional - comment out if you prefer manual)
// connect();
/**
 * TclTerminal.js
 * Reusable terminal component for Tcl interpreters
 * 
 * Usage:
 *   const terminal = new TclTerminal('container-id', connection, options);
 */

class TclTerminal {
    constructor(containerId, connection, options = {}) {
        this.container = document.getElementById(containerId);
        if (!this.container) {
            throw new Error(`Container element with id "${containerId}" not found`);
        }
        
        this.connection = connection; // DservConnection or similar
        this.options = {
            interpreter: options.interpreter || 'dserv',
            useLinkedSubprocess: options.useLinkedSubprocess !== false,
            showWelcome: options.showWelcome !== false,
            welcomeMessage: options.welcomeMessage || 'Tcl Terminal - Type "help" for assistance',
            historyKey: options.historyKey || 'tcl-terminal-history',
            maxHistory: options.maxHistory || 100,
            tabCompletion: options.tabCompletion !== false,
            ...options
        };
        
        // State
        this.inputElement = null;
        this.history = JSON.parse(localStorage.getItem(this.options.historyKey) || '[]');
        this.historyIndex = this.history.length;

        // Interpreters reachable via /name switching; fed by the host page
        // through updateAvailableInterps() (dserv/interps datapoint)
        this.availableInterps = ['dserv'];

        // Relay command advertised by the current interpreter in its
        // <name>/proxy datapoint (e.g. stim's stimSend forwards to the
        // stim program). Commands are wrapped `<proxy> {cmd}`; completion
        // and errorInfo still address the subprocess interp itself.
        this.currentProxy = null;

        // Tab completion state
        this.completionMatches = [];
        this.completionIndex = -1;
        this.completionOriginal = '';
        this.completionPending = false;
        this.waitingForCompletion = false;
        this.completionPrefix = '';
        
        this.init();
    }
    
    init() {
        this.container.className = 'tcl-terminal';
        
        if (this.options.showWelcome) {
            this.showInfo(this.options.welcomeMessage);
        }
        
        this.createInputLine();
        this.setupEventListeners();
    }
    
    setupEventListeners() {
        // Click anywhere in terminal to focus input (but allow text selection)
        let mouseDownTime = 0;
        let mouseDownPos = { x: 0, y: 0 };
        
        this.container.addEventListener('mousedown', (e) => {
            mouseDownTime = Date.now();
            mouseDownPos = { x: e.clientX, y: e.clientY };
        });
        
        this.container.addEventListener('mouseup', (e) => {
            // Check if this was a click (not a drag for selection)
            const timeDiff = Date.now() - mouseDownTime;
            const distance = Math.sqrt(
                Math.pow(e.clientX - mouseDownPos.x, 2) + 
                Math.pow(e.clientY - mouseDownPos.y, 2)
            );
            
            // If quick click with minimal movement and no text selected
            const selection = window.getSelection();
            if (timeDiff < 300 && distance < 5 && 
                (!selection || selection.toString().length === 0) &&
                e.target !== this.inputElement) {
                this.inputElement.focus();
            }
        });
        
        // Clean up copied text to remove extra blank lines
        this.container.addEventListener('copy', (e) => {
            const selection = window.getSelection();
            if (selection.rangeCount > 0) {
                const selectedText = selection.toString();
                // Remove multiple consecutive newlines
                const cleanedText = selectedText.replace(/\n\n+/g, '\n');
                e.clipboardData.setData('text/plain', cleanedText);
                e.preventDefault();
            }
        });
    }
    
    createInputLine() {
        // Remove existing input if any
        const existingInput = this.container.querySelector('.tcl-terminal-input-container');
        if (existingInput) {
            existingInput.remove();
        }
        
        const container = document.createElement('div');
        container.className = 'tcl-terminal-input-container';
        
        const prompt = document.createElement('span');
        prompt.className = 'tcl-terminal-prompt';
        prompt.textContent = `${this.options.interpreter}>\u00A0`; // \u00A0 is non-breaking space
        
        this.inputElement = document.createElement('input');
        this.inputElement.type = 'text';
        this.inputElement.className = 'tcl-terminal-input';
        this.inputElement.autocomplete = 'off';
        this.inputElement.spellcheck = false;
        
        container.appendChild(prompt);
        container.appendChild(this.inputElement);
        this.container.appendChild(container);
        
        this.inputElement.focus();
        this.scrollToBottom();
        
        // Event handlers
        this.inputElement.addEventListener('keydown', (e) => this.handleKeyDown(e));
    }
    
    handleKeyDown(e) {
        // Handle Tab for completion
        if (e.key === 'Tab') {
            e.preventDefault();
            if (this.options.tabCompletion) {
                this.handleTab();
            }
            return;
        }
        
        // Any other key resets completion state
        if (this.completionPending && e.key !== 'Tab') {
            this.completionPending = false;
            this.completionMatches = [];
            this.completionIndex = -1;
        }
        
        switch (e.key) {
            case 'Enter':
                e.preventDefault();
                this.submitCommand();
                break;
            case 'ArrowUp':
                e.preventDefault();
                this.navigateHistory(-1);
                break;
            case 'ArrowDown':
                e.preventDefault();
                this.navigateHistory(1);
                break;
            case 'c':
                if (e.ctrlKey) {
                    e.preventDefault();
                    this.inputElement.value = '';
                }
                break;
            case 'l':
                if (e.ctrlKey) {
                    e.preventDefault();
                    this.clear();
                }
                break;
        }
    }
    
    handleTab() {
        if (this.completionPending && this.completionMatches.length > 0) {
            // Cycle through existing matches
            this.completionIndex = (this.completionIndex + 1) % this.completionMatches.length;
            this.inputElement.value = this.completionMatches[this.completionIndex];
            this.inputElement.setSelectionRange(this.completionMatches[this.completionIndex].length,
                                               this.completionMatches[this.completionIndex].length);
            return;
        }

        // Request new completions
        const input = this.inputElement.value;

        if (!input.trim()) {
            return; // Don't complete empty input
        }

        // "/na<TAB>" completes interpreter names locally
        if (input.startsWith('/') && !/\s/.test(input)) {
            const partial = input.slice(1);
            const matches = this.availableInterps
                .filter(name => name.startsWith(partial))
                .map(name => '/' + name);
            this.applyCompletionMatches(matches, ' ');
            return;
        }

        // "/name cmd<TAB>" completes in that interp, keeping the /name prefix;
        // anything else completes in the current interp
        this.completionPrefix = '';
        let targetInterp = null;
        let completeInput = input;
        const oneOff = input.match(/^\/(\S+)\s+([\s\S]*)$/);
        if (oneOff) {
            if (!this.availableInterps.includes(oneOff[1])) {
                return;
            }
            targetInterp = oneOff[1];
            this.completionPrefix = input.slice(0, input.length - oneOff[2].length);
            completeInput = oneOff[2];
            if (!completeInput.trim()) {
                return;
            }
        }

        this.completionOriginal = input;
        this.waitingForCompletion = true;

        // Use backend 'complete' command for context-aware completion
        // This handles nested commands like "dl_tcllist [dl_fro<TAB>"
        const completionScript = `complete {${completeInput}}`;

        this.executeCommand(completionScript, true, targetInterp);
    }
    
    submitCommand() {
        const command = this.inputElement.value.trim();
        
        if (!command) {
            // Show empty prompt line (like hitting enter in a real terminal)
            this.showCommand('');
            this.createInputLine();
            return;
        }
        
        // Show command in terminal
        this.showCommand(command);
        
        // Handle local commands
        if (command === 'help') {
            this.showHelp();
            this.createInputLine();
            return;
        }
        
        if (command === 'clear' || command === 'cls') {
            this.clear();
            return;
        }
        
        // Add to history
        if (this.history[this.history.length - 1] !== command) {
            this.history.push(command);
            if (this.history.length > this.options.maxHistory) {
                this.history.shift();
            }
            localStorage.setItem(this.options.historyKey, JSON.stringify(this.history));
        }
        this.historyIndex = this.history.length;

        // Slash commands: /name switches interpreter, /name cmd is a one-off
        if (command.startsWith('/')) {
            this.handleSlashCommand(command);
            return;
        }

        // Execute command
        this.executeCommand(command, false);
    }

    async handleSlashCommand(command) {
        const match = command.match(/^\/(\S*)\s*([\s\S]*)$/);
        const target = match[1];
        const oneOff = match[2];

        if (!target) {
            this.showInfo(`Available interpreters: ${this.availableInterps.join(', ')}`);
            this.showInfo('Usage: /name to switch, /name command for a one-off');
            this.createInputLine();
            return;
        }

        if (!this.availableInterps.includes(target)) {
            this.showError(`Unknown interpreter: ${target}. Available: ${this.availableInterps.join(', ')}`, false);
            this.createInputLine();
            return;
        }

        if (!oneOff) {
            if (this.options.useLinkedSubprocess) {
                // Switching would be a lie: routing always prefers the
                // linked subprocess. One-offs still work.
                this.showWarning(`This terminal is bound to its own subprocess; use /${target} <command> for one-offs`);
                this.createInputLine();
                return;
            }
            this.setInterpreter(target);
            this.currentProxy = await this._fetchProxy(target);
            this.showInfo(this.currentProxy
                ? `Switched to ${target} (relaying via ${this.currentProxy})`
                : `Switched to ${target}`);
            this.createInputLine();
            return;
        }

        const proxy = await this._fetchProxy(target);
        this.executeCommand(oneOff, false, target, proxy);
    }

    // A subprocess can advertise a relay command in <name>/proxy
    // (e.g. stim/proxy = stimSend). Returns it, or null for none.
    async _fetchProxy(interp) {
        if (interp === 'dserv') {
            return null;
        }
        try {
            const response = await this.connection.send(`dservGet ${interp}/proxy`, interp);
            const proxy = (response || '').trim();
            if (!proxy || proxy.startsWith('!TCL_ERROR')) {
                return null;
            }
            return proxy;
        } catch (e) {
            return null;
        }
    }

    /**
     * Feed the list of reachable interpreters (the dserv/interps datapoint,
     * as a space-separated string or an array). 'dserv' is always included.
     */
    updateAvailableInterps(interps) {
        const list = Array.isArray(interps)
            ? interps
            : String(interps || '').trim().split(/\s+/).filter(Boolean);
        this.availableInterps = ['dserv', ...list.filter(name => name !== 'dserv')];

        // If the current interpreter's subprocess exited, fall back to dserv
        if (!this.options.useLinkedSubprocess &&
            !this.availableInterps.includes(this.options.interpreter)) {
            this.showWarning(`Interpreter ${this.options.interpreter} is gone; back to dserv`);
            this.setInterpreter('dserv');
        }
    }
    
    // Route a command to targetInterp if given, else to the terminal's
    // current interpreter (or its linked subprocess)
    _routeCommand(command, targetInterp = null) {
        if (targetInterp === null && this.options.useLinkedSubprocess) {
            return this.connection.sendToLinked(command);
        }
        const target = targetInterp || this.options.interpreter;
        if (target === 'dserv') {
            // For main dserv connection, send directly without wrapping
            return this.connection.sendRaw(command);
        }
        return this.connection.send(command, target);
    }

    async executeCommand(command, isCompletion, targetInterp = null, proxy = null) {
        // Relay through an advertised proxy where one applies (e.g. the
        // stim> prompt, or a /stim one-off); completion always addresses
        // the subprocess interp itself
        let effectiveProxy = null;
        if (!isCompletion) {
            if (targetInterp !== null) {
                effectiveProxy = proxy;
            } else if (this.currentProxy && !this.options.useLinkedSubprocess &&
                       this.options.interpreter !== 'dserv') {
                effectiveProxy = this.currentProxy;
            }
        }
        const toSend = effectiveProxy ? `${effectiveProxy} {${command}}` : command;

        try {
            const response = await this._routeCommand(toSend, targetInterp);

            if (isCompletion) {
                this.handleCompletionResponse(response);
            } else {
                this.handleCommandResponse(response, targetInterp, effectiveProxy);
            }
        } catch (e) {
            if (isCompletion) {
                this.waitingForCompletion = false;
            } else {
                this.showError(e.message || 'Command failed', false);
                this.createInputLine();
            }
        }
    }
    
    handleCompletionResponse(response) {
        this.waitingForCompletion = false;

        try {
            // Parse Tcl list; restore any /name one-off prefix
            const matches = this.parseTclList(response)
                .map(m => this.completionPrefix + m);
            this.applyCompletionMatches(matches);
        } catch (e) {
            this.completionPending = false;
        }
    }

    applyCompletionMatches(matches, singleMatchSuffix = '') {
        if (matches.length === 0) {
            // No matches
            this.completionPending = false;
        } else if (matches.length === 1) {
            // Single match - auto-complete
            const value = matches[0] + singleMatchSuffix;
            this.inputElement.value = value;
            this.inputElement.setSelectionRange(value.length, value.length);
            this.completionPending = false;
        } else {
            // Multiple matches - set up for cycling
            this.completionMatches = matches;
            this.completionIndex = 0;
            this.completionPending = true;

            // Show first match
            this.inputElement.value = matches[0];
            this.inputElement.setSelectionRange(matches[0].length, matches[0].length);
        }
    }

    handleCommandResponse(response, targetInterp = null, proxy = null) {
        // Check for Tcl error
        if (response && response.startsWith('!TCL_ERROR ')) {
            const errorMsg = response.substring('!TCL_ERROR '.length);
            this.showError(errorMsg);

            // Request errorInfo for stack trace (from the interp that errored)
            this.requestErrorInfo(targetInterp, proxy);
        } else if (response) {
            this.showResult(response);
        }
        this.createInputLine();
    }

    async requestErrorInfo(targetInterp = null, proxy = null) {
        try {
            let errorInfo = null;

            // A proxied command's error usually happened in the relayed
            // program, whose errorInfo only the proxy can reach. If the
            // relay itself is what failed, this fetch fails the same way
            // and the subprocess's own errorInfo below has the trace.
            if (proxy) {
                const relayed = await this._routeCommand(`${proxy} {set errorInfo}`, targetInterp);
                if (relayed && !relayed.startsWith('!TCL_ERROR')) {
                    errorInfo = relayed;
                }
            }

            if (errorInfo === null) {
                const local = await this._routeCommand('set errorInfo', targetInterp);
                if (local && !local.startsWith('!TCL_ERROR')) {
                    errorInfo = local;
                }
            }

            if (errorInfo && errorInfo.trim()) {
                this.showErrorInfo(errorInfo);
            }
        } catch (e) {
            // Silently ignore if we can't get errorInfo
        }
    }
    
    parseTclList(str) {
        // Parse Tcl list - handles braces, quotes, and escapes
        if (!str || str.trim() === '') return [];
        
        // Fast path for simple space-separated words (99% of cases)
        if (!str.includes('{') && !str.includes('"')) {
            return str.trim().split(/\s+/);
        }
        
        // More robust parser for complex cases
        const items = [];
        let current = '';
        let braceLevel = 0;
        let inQuotes = false;
        let escaped = false;
        
        for (const ch of str) {
            if (escaped) {
                current += ch;
                escaped = false;
                continue;
            }
            
            if (ch === '\\') {
                escaped = true;
                continue;
            }
            
            if (ch === '{' && !inQuotes) {
                if (braceLevel === 0 && current === '') {
                    braceLevel++;
                } else {
                    current += ch;
                    braceLevel++;
                }
            } else if (ch === '}' && !inQuotes) {
                braceLevel--;
                if (braceLevel === 0 && current !== '') {
                    items.push(current);
                    current = '';
                } else {
                    current += ch;
                }
            } else if (ch === '"' && braceLevel === 0) {
                inQuotes = !inQuotes;
                if (!inQuotes && current !== '') {
                    items.push(current);
                    current = '';
                }
            } else if ((ch === ' ' || ch === '\t' || ch === '\n') && braceLevel === 0 && !inQuotes) {
                if (current !== '') {
                    items.push(current);
                    current = '';
                }
            } else {
                current += ch;
            }
        }
        
        if (current !== '') items.push(current);
        return items;
    }
    
    navigateHistory(direction) {
        const newIndex = this.historyIndex + direction;
        
        if (newIndex < 0 || newIndex > this.history.length) {
            return;
        }
        
        this.historyIndex = newIndex;
        
        if (this.historyIndex === this.history.length) {
            this.inputElement.value = '';
        } else {
            this.inputElement.value = this.history[this.historyIndex];
        }
    }
    
    // Helper to insert content before the input line
    _insertBeforeInput(element) {
        const inputContainer = this.container.querySelector('.tcl-terminal-input-container');
        if (inputContainer) {
            this.container.insertBefore(element, inputContainer);
        } else {
            this.container.appendChild(element);
        }
        this.scrollToBottom();
    }
    
    showCommand(text) {
        const line = document.createElement('div');
        line.className = 'tcl-terminal-line tcl-terminal-command';
        line.textContent = `${this.options.interpreter}> ${text}`;
        this._insertBeforeInput(line);
    }
    
    showResult(text) {
        if (!text) return;
        const line = document.createElement('div');
        line.className = 'tcl-terminal-line tcl-terminal-result';
        line.textContent = text;
        this._insertBeforeInput(line);
    }
    
    showError(text, expandable = true) {
        const line = document.createElement('div');
        line.className = 'tcl-terminal-line tcl-terminal-error';

        const errorText = document.createElement('span');
        errorText.textContent = 'Error: ' + text;
        line.appendChild(errorText);

        // Expander only makes sense when a stack trace may follow
        if (expandable) {
            const expandIcon = document.createElement('span');
            expandIcon.className = 'tcl-terminal-error-expand';
            expandIcon.textContent = ' ▶';
            expandIcon.title = 'Click to show stack trace';
            line.appendChild(expandIcon);

            // Store for later when we get errorInfo
            this.lastErrorLine = line;
        }

        this._insertBeforeInput(line);
    }
    
    showInfo(text) {
        const line = document.createElement('div');
        line.className = 'tcl-terminal-line tcl-terminal-info';
        line.textContent = text;
        this._insertBeforeInput(line);
    }
    
    showWarning(text) {
        const line = document.createElement('div');
        line.className = 'tcl-terminal-line tcl-terminal-warning';
        line.textContent = text;
        this._insertBeforeInput(line);
    }
    
    showErrorInfo(text) {
        if (!this.lastErrorLine) return;
        
        // Skip first line of errorInfo (it's usually just the error message again)
        const lines = text.split('\n');
        const stackTrace = lines.slice(1).join('\n').trim();
        
        if (!stackTrace) {
            // No additional info, remove the expand icon
            const expandIcon = this.lastErrorLine.querySelector('.tcl-terminal-error-expand');
            if (expandIcon) expandIcon.remove();
            this.lastErrorLine = null;
            return;
        }
        
        const content = document.createElement('div');
        content.className = 'tcl-terminal-error-info-content';
        content.style.display = 'none';
        content.textContent = stackTrace;
        
        const expandIcon = this.lastErrorLine.querySelector('.tcl-terminal-error-expand');
        if (expandIcon) {
            // Make it clickable
            expandIcon.style.cursor = 'pointer';
            expandIcon.onclick = () => {
                const isExpanded = content.style.display !== 'none';
                content.style.display = isExpanded ? 'none' : 'block';
                expandIcon.textContent = isExpanded ? ' ▶' : ' ▼';
                expandIcon.title = isExpanded ? 'Click to show stack trace' : 'Click to hide stack trace';
            };
        }
        
        // Insert content after the error line
        this.lastErrorLine.parentNode.insertBefore(content, this.lastErrorLine.nextSibling);
        this.scrollToBottom();
        this.lastErrorLine = null;
    }
    
    showHelp() {
        const helpText = `
Tcl Terminal Help
=================

Commands are sent directly to the active Tcl interpreter.

Interpreters:
  /           - List available interpreters
  /name       - Switch to interpreter 'name' (e.g. /ess, /dserv)
  /name cmd   - Run one command in 'name' without switching

Local Commands:
  help        - Show this help
  clear/cls   - Clear terminal

Keyboard Shortcuts:
  ↑/↓         - Navigate command history
  Tab         - Auto-complete commands (works after /name too)
  Ctrl+C      - Clear current line
  Ctrl+L      - Clear terminal
  Enter       - Execute command
        `.trim();
        
        this.showInfo(helpText);
    }
    
    clear() {
        this.container.innerHTML = '';
        if (this.options.showWelcome) {
            this.showInfo(this.options.welcomeMessage);
        }
        this.createInputLine();
    }
    
    scrollToBottom() {
        this.container.scrollTop = this.container.scrollHeight;
    }
    
    focus() {
        if (this.inputElement) {
            this.inputElement.focus();
        }
    }
    
    // Public API methods
    write(text, type = 'result') {
        switch (type) {
            case 'command': this.showCommand(text); break;
            case 'error': this.showError(text); break;
            case 'info': this.showInfo(text); break;
            case 'warning': this.showWarning(text); break;
            default: this.showResult(text);
        }
    }
    
    setInterpreter(name) {
        this.options.interpreter = name;
        this.currentProxy = null;
        this.completionPending = false;
        this.completionMatches = [];
        this.completionIndex = -1;
        const promptElement = this.container.querySelector('.tcl-terminal-prompt');
        if (promptElement) {
            promptElement.textContent = `${name}> `;
        }
    }
}

// Export for use
if (typeof window !== 'undefined') {
    window.TclTerminal = TclTerminal;
}
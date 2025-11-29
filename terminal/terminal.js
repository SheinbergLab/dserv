// ============================================
// Terminal Module
// ============================================

class Terminal {
    constructor(wsManager) {
        this.ws = wsManager;
        this.container = document.getElementById('terminal');
        this.inputElement = null;
        this.history = JSON.parse(localStorage.getItem('dserv-history') || '[]');
        this.historyIndex = this.history.length;
        
        // Interpreter management
        this.currentInterp = 'dserv';
        this.availableInterps = ['dserv'];
        this.interpSelectorElement = null;
        
        // Tab completion state
        this.completionMatches = [];
        this.completionIndex = -1;
        this.completionOriginal = '';
        this.completionPending = false;
        this.waitingForCompletion = false;

        // Listen for WebSocket events
        this.ws.on('connected', () => this.handleConnected());
        this.ws.on('disconnected', () => this.handleDisconnected());
        this.ws.on('terminal:response', (data) => this.handleResponse(data));
        this.ws.on('datapoint:update', (data) => this.handleDatapointUpdate(data));

        // Initialize
        this.init();
    }

    init() {
        this.showInfo('dserv Web Console - Type commands or "help" for assistance');
        this.showInfo('Connecting to WebSocket...');

        // Initialize interpreter selector
        this.interpSelectorElement = document.getElementById('interp-selector');
        if (this.interpSelectorElement) {
            this.interpSelectorElement.addEventListener('change', (e) => {
                this.switchInterpreter(e.target.value);
            });
        }

        // Clear terminal button
        document.getElementById('clear-terminal').addEventListener('click', () => {
            this.clearTerminal();
        });

        // Keyboard shortcuts
        document.addEventListener('keydown', (e) => {
            if (e.ctrlKey && e.key === 'l') {
                e.preventDefault();
                this.clearTerminal();
            }
        });
    }

    handleConnected() {
        this.showInfo('Connected to dserv');
        
        // Subscribe to interpreter list
        this.ws.send({
            cmd: 'subscribe',
            match: 'dserv/interps',
            every: 1
        });
        
        // Get current interpreter list
        this.ws.send({
            cmd: 'get',
            name: 'dserv/interps'
        });
        
        this.createInputLine();
    }

    handleDisconnected() {
        this.showInfo('Disconnected from server. Reconnecting...');
        if (this.inputElement) {
            this.inputElement.disabled = true;
        }
    }

    handleDatapointUpdate(data) {
        // Check if this is the dserv/interps datapoint
        if (data.name === 'dserv/interps') {
            const interpsData = data.data !== undefined ? data.data : data.value;
            if (typeof interpsData === 'string') {
                // Parse space-separated list of interpreters
                const interps = interpsData.trim().split(/\s+/).filter(i => i.length > 0);
                this.updateAvailableInterpreters(interps);
            } else if (Array.isArray(interpsData)) {
                this.updateAvailableInterpreters(interpsData);
            }
        }
    }

    updateAvailableInterpreters(interps) {
	// Always include 'dserv' as the main interpreter
	this.availableInterps = ['dserv', ...interps.filter(i => i !== 'dserv')];
	
	console.log('Available interpreters:', this.availableInterps);
	
	// Check if current interpreter still exists
	if (!this.availableInterps.includes(this.currentInterp)) {
            this.currentInterp = 'dserv';
            this.updatePrompt();
	}
	
	// Update selector dropdown
	if (this.interpSelectorElement) {
            const currentValue = this.interpSelectorElement.value;
            this.interpSelectorElement.innerHTML = this.availableInterps
		.map(interp => `<option value="${interp}">${interp}</option>`)
		.join('');
            
            // Set selector to match current interpreter
            this.interpSelectorElement.value = this.currentInterp;
	}
    }
    
    switchInterpreter(interpName) {
        // Validate interpreter exists
        if (!this.availableInterps.includes(interpName)) {
            this.showError(`Unknown interpreter: ${interpName}. Available: ${this.availableInterps.join(', ')}`);
            return false;
        }
        
        this.currentInterp = interpName;
        
        // Update dropdown if it exists
        if (this.interpSelectorElement) {
            this.interpSelectorElement.value = interpName;
        }
        
        // Update prompt
        this.updatePrompt();
        
        this.showInfo(`Switched to interpreter: ${interpName}`);
        return true;
    }

    updatePrompt() {
        // Update the prompt in existing input line if it exists
        const promptElement = this.container.querySelector('.prompt');
        if (promptElement) {
            promptElement.textContent = `${this.currentInterp}> `;
        }
    }

    createInputLine() {
        // Remove existing input if any
        const existingInput = this.container.querySelector('.input-container');
        if (existingInput) {
            existingInput.remove();
        }

        const container = document.createElement('div');
        container.className = 'input-container';

        const prompt = document.createElement('span');
        prompt.className = 'prompt';
        prompt.textContent = `${this.currentInterp}> `;

        this.inputElement = document.createElement('input');
        this.inputElement.type = 'text';
        this.inputElement.autocomplete = 'off';
        this.inputElement.spellcheck = false;

        container.appendChild(prompt);
        container.appendChild(this.inputElement);
        this.container.appendChild(container);

        this.inputElement.focus();
        this.scrollToBottom();

        // Event handlers
        this.inputElement.addEventListener('keydown', (e) => this.handleKeyDown(e));
        
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
    }

    handleKeyDown(e) {
        // Handle Tab for completion
        if (e.key === 'Tab') {
            e.preventDefault();
            this.handleTab();
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
        }
    }

    handleTab() {
        if (this.completionPending) {
            // Already have matches, cycle through them
            this.cycleCompletion();
        } else {
            // Start new completion
            this.startCompletion();
        }
    }

    startCompletion() {
        const input = this.inputElement.value;
        
        if (!input.trim()) {
            return; // Don't complete empty input
        }
        
        // Build completion script
        let completeScript;
        
        if (this.currentInterp === 'dserv') {
            completeScript = `complete {${input}}`;
        } else {
            completeScript = `send ${this.currentInterp} {complete {${input}}}`;
        }
        
        // Save original input
        this.completionOriginal = input;
        
        // Mark that we're waiting for completion response
        this.waitingForCompletion = true;
        
        // Send completion request
        this.ws.send({
            cmd: 'eval',
            script: completeScript
        });
    }

    cycleCompletion() {
        if (this.completionMatches.length === 0) return;
        
        // Move to next match
        this.completionIndex = (this.completionIndex + 1) % this.completionMatches.length;
        const match = this.completionMatches[this.completionIndex];
        
        // Update input
        this.inputElement.value = match;
        this.inputElement.setSelectionRange(match.length, match.length);
    }

    parseTclList(str) {
        // Simple parser good enough for most completion results
        if (!str || str.trim() === '') return [];
        
        // For simple space-separated words (99% of cases)
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
        if (this.history.length === 0) return;

        this.historyIndex += direction;
        this.historyIndex = Math.max(0, Math.min(this.history.length, this.historyIndex));

        if (this.historyIndex === this.history.length) {
            this.inputElement.value = '';
        } else {
            this.inputElement.value = this.history[this.historyIndex];
        }
    }

    submitCommand() {
        const command = this.inputElement.value.trim();
        
        // Handle empty command - just show prompt and create new line
        if (!command) {
            this.showCommand('');
            
            // Remove input line
            const inputContainer = this.inputElement.parentElement;
            if (inputContainer) {
                inputContainer.remove();
            }
            
            this.createInputLine();
            return;
        }

        // Save to history
        if (command !== this.history[this.history.length - 1]) {
            this.history.push(command);
            if (this.history.length > 100) this.history.shift();
            localStorage.setItem('dserv-history', JSON.stringify(this.history));
        }
        this.historyIndex = this.history.length;

        // Display command with current prompt
        this.showCommand(command);

        // Remove input line
        const inputContainer = this.inputElement.parentElement;
        if (inputContainer) {
            inputContainer.remove();
        }

        // Check for blocked commands
        if (this.isBlockedCommand(command)) {
            this.showWarning('No need to type exit, just close your browser window when done');
            this.createInputLine();
            return;
        }

        // Check for slash commands (interpreter switching or one-off)
        if (command.startsWith('/')) {
            this.handleSlashCommand(command);
            return;
        }

        // Check for local commands
        if (command === 'help') {
            this.showHelp();
            this.createInputLine();
            return;
        }

        if (command === 'clear' || command === 'cls') {
            this.clearTerminal();
            this.createInputLine();
            return;
        }

        // Send command to current interpreter
        this.sendToInterpreter(this.currentInterp, command, true);
    }

    handleSlashCommand(command) {
        // Parse: /interp [command...]
        const parts = command.slice(1).split(/\s+/);
        const targetInterp = parts[0];
        const oneOffCommand = parts.slice(1).join(' ');

        if (!targetInterp) {
            this.showError('Usage: /interp [command] or just /interp to switch');
            this.createInputLine();
            return;
        }

        // Validate interpreter exists
        if (!this.availableInterps.includes(targetInterp)) {
            this.showError(`Unknown interpreter: ${targetInterp}. Available: ${this.availableInterps.join(', ')}`);
            this.createInputLine();
            return;
        }

        if (!oneOffCommand) {
            // Just "/ess" - switch interpreter
            this.switchInterpreter(targetInterp);
            this.createInputLine();
        } else {
            // "/ess pwd" - one-off command, don't switch
            // Build the command to send
            let scriptToSend;
            
            if (this.currentInterp === 'dserv') {
                // From dserv, can directly route to target
                if (targetInterp === 'dserv') {
                    scriptToSend = oneOffCommand;
                } else {
                    scriptToSend = `send ${targetInterp} {${oneOffCommand}}`;
                }
            } else {
                // From subprocess, must route through current subprocess
                if (targetInterp === 'dserv') {
                    // One-off to dserv from subprocess - just send command to dserv directly
                    scriptToSend = oneOffCommand;
                } else if (targetInterp === this.currentInterp) {
                    // Same interpreter - just wrap once
                    scriptToSend = `send ${targetInterp} {${oneOffCommand}}`;
                } else {
                    // Different subprocess - double wrap through current interp
                    scriptToSend = `send ${this.currentInterp} {send ${targetInterp} {${oneOffCommand}}}`;
                }
            }
            
            this.ws.send({
                cmd: 'eval',
                script: scriptToSend
            });
        }
    }

    sendToInterpreter(interpName, command, createNewInput) {
        if (!this.ws.connected) {
            this.showError('Not connected to server');
            if (createNewInput) {
                this.createInputLine();
            }
            return;
        }

        // Wrap command if not targeting main dserv process
        let scriptToSend;
        if (interpName === 'dserv') {
            scriptToSend = command;
        } else {
            // Escape the command for send - need to handle Tcl braces
            // For safety, we'll wrap in braces
            scriptToSend = `send ${interpName} {${command}}`;
        }

        this.ws.send({
            cmd: 'eval',
            script: scriptToSend
        });
    }

    isBlockedCommand(command) {
        const normalized = command.trim().toLowerCase();
	// once blocked 'exit' but no process that on backend
	return false;
    }

    handleResponse(data) {
        // Check if this is a completion response
        if (this.waitingForCompletion) {
            this.waitingForCompletion = false;
            
            if (data.status === 'ok' && data.result) {
                // Parse Tcl list
                const matches = this.parseTclList(data.result);
                
                if (matches.length === 0) {
                    // No matches - do nothing
                    this.completionPending = false;
                } else if (matches.length === 1) {
                    // Single match - auto-complete immediately
                    this.inputElement.value = matches[0];
                    this.inputElement.setSelectionRange(matches[0].length, matches[0].length);
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
            } else {
                // Error in completion
                this.completionPending = false;
            }
            
            return; // Don't create input line for completion
        }
        
        // Regular command response
        if (data.status === 'error' || (data.result && data.result.startsWith('!TCL_ERROR'))) {
            this.showError(data.result || data.error || 'Unknown error');
        } else if (data.result !== undefined) {
            this.showResult(data.result);
        } else {
            this.showResult(JSON.stringify(data));
        }
        
        // Always create new input line on current interpreter
        this.createInputLine();
    }

    showCommand(text) {
        const line = document.createElement('div');
        line.className = 'output-line command-line';
        line.textContent = `${this.currentInterp}> ${text}`;
        this.container.appendChild(line);
        this.scrollToBottom();
    }

    showResult(text) {
        const line = document.createElement('div');
        line.className = 'output-line result-line';
        line.textContent = text;
        this.container.appendChild(line);
        this.scrollToBottom();
    }

    showError(text) {
        const line = document.createElement('div');
        line.className = 'output-line error-line';
        line.textContent = 'Error: ' + text;
        this.container.appendChild(line);
        this.scrollToBottom();
    }

    showInfo(text) {
        const line = document.createElement('div');
        line.className = 'output-line info-line';
        line.textContent = text;
        this.container.appendChild(line);
        this.scrollToBottom();
    }

    showWarning(text) {
        const line = document.createElement('div');
        line.className = 'output-line warning-line';
        line.textContent = text;
        this.container.appendChild(line);
        this.scrollToBottom();
    }

    showHelp() {
        const helpText = `
dserv Web Console Help
======================

Commands are sent directly to the active interpreter.

Interpreter Switching:
  /interp         - Switch active interpreter to 'interp'
  /interp cmd     - Execute 'cmd' on 'interp' without switching
  
  Examples:
    /ess          - Switch to 'ess' interpreter
    /ess pwd      - Run 'pwd' on 'ess' (stays on current interp)

Local Commands:
  help        - Show this help
  clear/cls   - Clear terminal

Keyboard Shortcuts:
  ↑/↓         - Navigate command history
  Tab         - Auto-complete commands, procs, variables, namespaces
  Ctrl+C      - Clear current line
  Ctrl+L      - Clear terminal
  Enter       - Execute command

Tab Completion:
  Press Tab to complete commands, procedures, variables, and more.
  Context-aware: completes variable names after 'set', namespace
  names after 'namespace eval', etc. Press Tab multiple times to
  cycle through matches.

The terminal connects to dserv via WebSocket and evaluates
Tcl commands in the active interpreter. Use the dropdown or
slash commands to switch between subprocesses.
        `.trim();

        this.showInfo(helpText);
    }

    clearTerminal() {
        this.container.innerHTML = '';
        this.showInfo('Terminal cleared');
        this.createInputLine();
    }

    scrollToBottom() {
        this.container.scrollTop = this.container.scrollHeight;
    }

    focusInput() {
        if (this.inputElement) {
            this.inputElement.focus();
        }
    }
}

// ============================================
// Initialize terminal when DOM is ready
// ============================================

let terminal;

document.addEventListener('DOMContentLoaded', () => {
    terminal = new Terminal(wsManager);

    // Track if user is selecting text
    let isSelecting = false;
    const terminalEl = document.getElementById('terminal');
    
    terminalEl.addEventListener('mousedown', () => {
        isSelecting = false;
    });
    
    terminalEl.addEventListener('mousemove', (e) => {
        if (e.buttons === 1) { // Left mouse button is down
            isSelecting = true;
        }
    });
    
    terminalEl.addEventListener('mouseup', () => {
        // If there's a text selection, don't clear it
        if (window.getSelection().toString().length > 0) {
            isSelecting = true;
        }
    });

    // Focus input when clicking in terminal area (but not if selecting text)
    terminalEl.addEventListener('click', (e) => {
        // Don't focus if user just finished selecting text
        if (isSelecting) {
            isSelecting = false;
            return;
        }
        
        // Only focus if clicking on the container itself or input area
        if (e.target.id === 'terminal' || e.target.classList.contains('input-container')) {
            terminal.focusInput();
        }
    });
    
    // Clean up copied text to remove extra blank lines
    terminalEl.addEventListener('copy', (e) => {
        const selection = window.getSelection();
        if (selection.rangeCount > 0) {
            const selectedText = selection.toString();
            // Remove multiple consecutive newlines (keep at most one blank line)
            const cleanedText = selectedText.replace(/\n\n+/g, '\n');
            e.clipboardData.setData('text/plain', cleanedText);
            e.preventDefault();
        }
    });
});

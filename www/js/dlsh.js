/**
 * dlsh.js - Interactive Tcl Workbench
 * 
 * Manages the unified interface with:
 * - TclEditor (top-left)
 * - TclTerminal (bottom-left)  
 * - GraphicsRenderer (top-right)
 * - DGTableViewer (bottom-right)
 * 
 * All components share a single DservConnection.
 */

class DlshWorkbench {
    constructor(options = {}) {
        this.options = {
            dservHost: 'localhost:2565',
            forceSecure: true,
            subprocess: 'dlsh',
            graphicsStream: 'graphics/demo',
            ...options
        };
        
        // Components
        this.conn = null;
        this.tclEditor = null;
        this.terminal = null;
        this.renderer = null;
        this.dpManager = null;
        this.tableViewer = null;
        
        // Private channel prefix (set after connection)
        this.privatePrefix = null;
        
        // Table history for pushed tables
        this.tableHistory = [];
        this.currentTableIndex = -1;
        
        // DOM elements
        this.elements = {
            statusIndicator: document.getElementById('status-indicator'),
            statusText: document.getElementById('status-text'),
            editorContainer: document.getElementById('editor-container'),
            terminalContainer: document.getElementById('terminal-container'),
            graphicsCanvas: document.getElementById('graphics-canvas'),
            graphicsStats: document.getElementById('graphics-stats'),
            tableContainer: document.getElementById('table-container'),
            dgNameInput: document.getElementById('dg-name'),
            errorContainer: document.getElementById('error-container'),
            consoleOutput: document.getElementById('console-output'),
            errorCount: document.getElementById('error-count')
        };
        
        // Error tracking
        this.errorCount = 0;
        this.lastErrorInfo = null;
        
        // Bind methods
        this.runCode = this.runCode.bind(this);
        this.loadTable = this.loadTable.bind(this);
        this.handleResize = this.handleResize.bind(this);
    }
    
    // ============================================================
    // Initialization
    // ============================================================
    
    async init() {
        this.updateStatus('connecting', 'Connecting to dserv...');
        
        try {
            await this.initConnection();
            this.initEditor();
            this.initTerminal();
            this.initGraphics();
            this.initTables();
            this.initErrorMonitor();
            this.initEventListeners();
            
            this.updateStatus('connected', 'Connected');
        } catch (e) {
            this.updateStatus('error', 'Connection failed');
            this.showError(`Failed to initialize: ${e.message}`);
        }
    }
    
    async initConnection() {
        this.conn = new DservConnection({
            subprocess: this.options.subprocess,
            createLinkedSubprocess: true,
            dservHost: this.options.dservHost,
            forceSecure: this.options.forceSecure,
            onStatus: (status, msg) => this.updateStatus(status, msg),
            onError: (err) => this.showError(err)
        });
        
        await this.conn.connect();
        
        // Get the private prefix from linked subprocess name
        this.privatePrefix = this.conn.getLinkedSubprocess();
        
        // Initialize linked subprocess with dlsh package and error monitoring
        if (this.privatePrefix) {
            await this.conn.sendToLinked('package require dlsh; errormon enable');
            
            // Create flushwin proc that writes to private graphics channel
            await this.conn.sendToLinked(
                `proc flushwin {{target main}} { dservSet ${this.privatePrefix}/graphics/$target [dumpwin json] }`
            );
            
            // Create dg_view proc that writes to private tables channel
            await this.conn.sendToLinked(
                `proc dg_view {dg {name ""}} {
                    if {$name eq ""} {
                        set name $dg
                    }
                    dservSet ${this.privatePrefix}/tables/$name [dg_toHybridJSON $dg]
                }`
            );
            
            console.log('Private prefix:', this.privatePrefix);
        }
    }
    
    initEditor() {
        this.tclEditor = new TclEditor('editor-container', {
            theme: 'dark',
            fontSize: '11px',
            tabSize: 4
        });
        
        this.elements.editorContainer.addEventListener('editor-ready', () => {
            this.tclEditor.setWebSocket(this.conn);
            this.tclEditor.setOnExecute((code) => this.executeSnippet(code));
            this.loadExample();
        }, { once: true });
    }
    
    initTerminal() {
        this.terminal = new TclTerminal('terminal-container', this.conn, {
            interpreter: 'dserv',
            useLinkedSubprocess: true,
            welcomeMessage: 'dlsh terminal ready',
            tabCompletion: true
        });
    }
    
    initGraphics() {
        // DatapointManager for subscriptions
        this.dpManager = new DatapointManager(this.conn, {
            autoGetKeys: false
        });
        
        // Initial canvas sizing
        this.resizeCanvas();
        
        // Determine graphics stream - use private channel if available
        const graphicsStream = this.privatePrefix 
            ? `${this.privatePrefix}/graphics/main`
            : this.options.graphicsStream;
        
        // GraphicsRenderer with auto-subscription
        this.renderer = new GraphicsRenderer('graphics-canvas', {
            datapointManager: this.dpManager,
            streamId: graphicsStream,
            width: this.elements.graphicsCanvas.width,
            height: this.elements.graphicsCanvas.height,
            onStats: (stats) => {
                this.elements.graphicsStats.textContent = 
                    `${stats.commandCount} cmds • ${stats.time}`;
            }
        });
    }
    
    initTables() {
        if (!this.privatePrefix || !this.dpManager) return;
        
        // Subscribe to tables using DatapointManager (same as GraphicsRenderer)
        const tablesPattern = `${this.privatePrefix}/tables/*`;
        console.log('Subscribing to tables pattern:', tablesPattern);
        
        // Use DatapointManager's subscribe method
        this.dpManager.subscribe(tablesPattern, (dp) => {
            console.log('Table update received:', dp.name);
            this.handleTablePush(dp);
        });
    }
    
    handleTablePush(datapoint) {
        // Extract table name from datapoint path
        const tableName = datapoint.name.split('/').pop();
        
        // Parse the data
        let data;
        try {
            data = typeof datapoint.data === 'string' 
                ? JSON.parse(datapoint.data) 
                : datapoint.data;
        } catch (e) {
            this.showError(`Invalid table data: ${e.message}`);
            return;
        }
        
        // Add to history
        const tableEntry = {
            name: tableName,
            data: data,
            timestamp: Date.now()
        };
        
        // Check if this table name already exists in history
        const existingIndex = this.tableHistory.findIndex(t => t.name === tableName);
        if (existingIndex >= 0) {
            // Update existing entry
            this.tableHistory[existingIndex] = tableEntry;
            if (this.currentTableIndex === existingIndex) {
                // Re-render if it's the currently displayed table
                this.renderTable(data);
            }
        } else {
            // Add new entry
            this.tableHistory.push(tableEntry);
            this.currentTableIndex = this.tableHistory.length - 1;
            this.renderTable(data);
        }
        
        // Update table selector UI
        this.updateTableSelector();
    }
    
    renderTable(data) {
        this.tableViewer = new DGTableViewer('table-container', data, {
            pageSize: 50,
            maxHeight: '100%',
            theme: 'dark'
        });
        this.tableViewer.render();
    }
    
    updateTableSelector() {
        // Update the dg-name input to show current table
        if (this.tableHistory.length > 0 && this.currentTableIndex >= 0) {
            const current = this.tableHistory[this.currentTableIndex];
            this.elements.dgNameInput.value = current.name;
        }
    }
    
    initErrorMonitor() {
        if (!this.privatePrefix || !this.dpManager) return;
        
        // Subscribe to error datapoint for this subprocess
        // Errors show up as error/PRIVATE_LINKED_NAME
        const errorPattern = `error/${this.privatePrefix}`;
        console.log('Subscribing to errors:', errorPattern);
        
        this.dpManager.subscribe(errorPattern, (dp) => {
            console.log('Error received:', dp);
            this.handleError(dp);
        });
    }
    
    handleError(datapoint) {
        const value = datapoint.data !== undefined ? datapoint.data : datapoint.value;
        
        // Skip if empty/null
        if (!value) return;
        
        // Parse error data
        let errorInfo;
        if (typeof value === 'string') {
            try {
                const parsed = JSON.parse(value);
                errorInfo = parsed.errorInfo || parsed.message || value;
            } catch {
                errorInfo = value;
            }
        } else if (typeof value === 'object') {
            errorInfo = value.errorInfo || value.message || JSON.stringify(value);
        } else {
            return;
        }
        
        // Check for duplicates - don't add if last error is identical
        if (this.lastErrorInfo === errorInfo) {
            console.log('Duplicate error ignored');
            return;
        }
        this.lastErrorInfo = errorInfo;
        
        // Log to console panel
        this.logToConsole(errorInfo, 'error');
    }
    
    logToConsole(message, type = 'error') {
        const output = this.elements.consoleOutput;
        if (!output) return;
        
        // Create entry
        const entry = document.createElement('div');
        entry.className = 'dlsh-console-entry collapsed';
        
        // Header line with time, expander, and preview
        const header = document.createElement('div');
        header.className = 'dlsh-console-header-line';
        
        const time = document.createElement('span');
        time.className = 'dlsh-console-time';
        time.textContent = new Date().toLocaleTimeString();
        
        // Get first line for preview
        const firstLine = message.split('\n')[0];
        const hasMore = message.includes('\n') || firstLine.length > 60;
        const preview = firstLine.length > 60 ? firstLine.substring(0, 57) + '...' : firstLine;
        
        const expander = document.createElement('span');
        expander.className = 'dlsh-console-expander';
        expander.textContent = hasMore ? '▶' : '•';
        
        const msg = document.createElement('span');
        msg.className = `dlsh-console-message ${type}`;
        msg.textContent = preview;
        
        header.appendChild(time);
        header.appendChild(expander);
        header.appendChild(msg);
        entry.appendChild(header);
        
        // Add expandable detail if message has more content
        if (hasMore) {
            const detail = document.createElement('div');
            detail.className = 'dlsh-console-detail';
            
            const pre = document.createElement('pre');
            pre.textContent = message;
            detail.appendChild(pre);
            
            entry.appendChild(detail);
            
            // Toggle on click
            header.style.cursor = 'pointer';
            header.addEventListener('click', () => {
                const isExpanded = entry.classList.toggle('collapsed');
                expander.textContent = isExpanded ? '▶' : '▼';
            });
        }
        
        output.appendChild(entry);
        
        // Auto-scroll to bottom
        output.scrollTop = output.scrollHeight;
        
        // Update error count
        if (type === 'error') {
            this.errorCount++;
            this.elements.errorCount.textContent = `${this.errorCount} error${this.errorCount > 1 ? 's' : ''}`;
        }
    }
    
    clearConsole() {
        if (this.elements.consoleOutput) {
            this.elements.consoleOutput.innerHTML = '';
        }
        this.errorCount = 0;
        if (this.elements.errorCount) {
            this.elements.errorCount.textContent = '';
        }
    }
    
    initEventListeners() {
        // Window resize
        window.addEventListener('resize', this.handleResize);
        
        // Enter key in dg name input
        this.elements.dgNameInput.addEventListener('keypress', (e) => {
            if (e.key === 'Enter') this.loadTable();
        });
    }
    
    // ============================================================
    // Editor Actions
    // ============================================================
    
    async runCode() {
        if (!this.tclEditor || !this.conn || !this.conn.connected) {
            this.showError('Not connected');
            return;
        }
        
        const code = this.tclEditor.getValue().trim();
        if (!code) return;
        
        try {
            const response = await this.conn.sendToLinked(code);
            
            // If response looks like a group name, auto-populate table input
            if (response && response.match(/^group\d+$/)) {
                this.elements.dgNameInput.value = response;
                this.loadTable();
            }
        } catch (e) {
            this.showError('Execution error: ' + e.message);
        }
    }
    
    async executeSnippet(code) {
        // Execute a snippet (from Shift-Enter) and show result in terminal
        if (!this.conn || !this.conn.connected) {
            this.showError('Not connected');
            return;
        }
        
        if (!code.trim()) return;
        
        try {
            // Show the command in terminal
            if (this.terminal) {
                this.terminal.showCommand(code.split('\n')[0] + (code.includes('\n') ? ' ...' : ''));
            }
            
            const response = await this.conn.sendToLinked(code);
            
            // Show result in terminal
            if (this.terminal && response) {
                this.terminal.showResult(response);
            }
            
            // Auto-populate table if result looks like a group name
            if (response && response.match(/^group\d+$/)) {
                this.elements.dgNameInput.value = response;
            }
        } catch (e) {
            if (this.terminal) {
                this.terminal.showError(e.message);
            }
        }
    }
    
    clearEditor() {
        if (this.tclEditor) {
            this.tclEditor.setValue('');
        }
    }
    
    loadExample() {
        if (!this.tclEditor) return;
        
        const code = `# Create a dynamic group
set g [dg_create]

# Add some lists
set n 100
dl_set $g:trial [dl_fromto 0 $n]
dl_set $g:condition [dl_repeat [dl_slist A B] [expr {$n/2}]]
dl_set $g:rt [dl_add 600 [dl_mult [dl_zrand $n] 100]]
dl_set $g:accuracy [dl_irand $n 2]

# Draw tick marks to graphics
clearwin
setwindow 0 0 1 1
dlg_markers [dl_fromto 0 1 0.1] 0.5 vtick -size 1y
dlg_markers 0.5 [dl_fromto 0 1 0.1] htick -size 1x
flushwin

# Push table to viewer
dg_view $g

return $g`;
        
        this.tclEditor.setValue(code);
    }
    
    // ============================================================
    // Terminal Actions
    // ============================================================
    
    clearTerminal() {
        if (this.terminal && this.terminal.clear) {
            this.terminal.clear();
        }
    }
    
    // ============================================================
    // Graphics Actions
    // ============================================================
    
    clearCanvas() {
        if (this.renderer) {
            this.renderer.clear();
        }
    }
    
    resizeCanvas() {
        const canvas = this.elements.graphicsCanvas;
        const container = canvas.parentElement;
        
        canvas.width = container.clientWidth;
        canvas.height = container.clientHeight;
        
        if (this.renderer && this.renderer.resize) {
            this.renderer.resize(canvas.width, canvas.height);
        }
    }
    
    handleResize() {
        this.resizeCanvas();
    }
    
    // ============================================================
    // Table Actions
    // ============================================================
    
    async loadTable() {
        const dgName = this.elements.dgNameInput.value.trim();
        if (!dgName) {
            this.showError('Enter a group name');
            return;
        }
        
        if (!this.conn || !this.conn.connected) {
            this.showError('Not connected');
            return;
        }
        
        try {
            const command = `dg_toHybridJSON ${dgName}`;
            const response = await this.conn.sendToLinked(command);
            
            let data;
            try {
                data = JSON.parse(response);
            } catch (e) {
                throw new Error(`Invalid JSON: ${response.substring(0, 100)}`);
            }
            
            this.renderTable(data);
            
        } catch (e) {
            this.showError(`Load failed: ${e.message}`);
        }
    }
    
    loadDemoData() {
        const data = {
            name: "Demo Data",
            rows: [
                {trial: 1, condition: "A", rt: 234.5, accuracy: 1},
                {trial: 2, condition: "B", rt: 456.2, accuracy: 1},
                {trial: 3, condition: "A", rt: 189.7, accuracy: 0},
                {trial: 4, condition: "C", rt: 567.3, accuracy: 1},
                {trial: 5, condition: "B", rt: 234.1, accuracy: 1},
                {trial: 6, condition: "A", rt: 678.9, accuracy: 0},
                {trial: 7, condition: "C", rt: 345.6, accuracy: 1},
                {trial: 8, condition: "B", rt: 456.7, accuracy: 1},
            ],
            arrays: {
                fixations: [
                    [0.5, 1.2, 2.3], [0.8, 1.9], [1.1, 2.2, 3.3, 4.4],
                    [0.3, 0.9, 1.5], [1.0, 2.0], [0.7, 1.4, 2.1, 2.8],
                    [0.4, 1.2], [0.6, 1.8, 2.4]
                ]
            }
        };
        
        this.renderTable(data);
    }
    
    // ============================================================
    // Connection Actions
    // ============================================================
    
    async reconnect() {
        if (this.conn) {
            this.conn.disconnect();
        }
        await this.init();
    }
    
    // ============================================================
    // UI Helpers
    // ============================================================
    
    updateStatus(status, message) {
        const indicator = this.elements.statusIndicator;
        const text = this.elements.statusText;
        
        indicator.className = 'dlsh-status-indicator';
        if (status === 'connected') {
            indicator.classList.add('connected');
        } else if (status === 'connecting') {
            indicator.classList.add('connecting');
        } else if (status === 'error') {
            indicator.classList.add('error');
        }
        
        text.textContent = message;
    }
    
    showError(message) {
        const container = this.elements.errorContainer;
        const toast = document.createElement('div');
        toast.className = 'dlsh-error-toast';
        toast.textContent = message;
        container.appendChild(toast);
        
        setTimeout(() => {
            toast.remove();
        }, 4000);
    }
}

// ============================================================
// Global instance and helper functions
// ============================================================

let workbench = null;

// These functions are called from HTML onclick handlers
function runCode() { workbench?.runCode(); }
function clearEditor() { workbench?.clearEditor(); }
function loadExample() { workbench?.loadExample(); }
function clearTerminal() { workbench?.clearTerminal(); }
function clearCanvas() { workbench?.clearCanvas(); }
function loadTable() { workbench?.loadTable(); }
function loadDemoData() { workbench?.loadDemoData(); }
function reconnect() { workbench?.reconnect(); }
function clearConsole() { workbench?.clearConsole(); }

// Initialize on DOM ready
document.addEventListener('DOMContentLoaded', () => {
    workbench = new DlshWorkbench();
    workbench.init();
});

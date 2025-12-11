/**
 * MeshDropdown - Reusable mesh systems dropdown component
 * 
 * Shows connected mesh peers in a dropdown menu, allowing quick navigation
 * between systems in a dserv mesh network.
 * 
 * Usage:
 *   const meshDropdown = new MeshDropdown(container, connection, options);
 * 
 * @param {HTMLElement|string} container - Container element or selector
 * @param {Object} connection - WebSocket connection with send() and on() methods
 * @param {Object} options - Configuration options
 */
class MeshDropdown {
    constructor(container, connection, options = {}) {
        this.container = typeof container === 'string' 
            ? document.querySelector(container) 
            : container;
        this.connection = connection;
        
        this.options = {
            pollInterval: options.pollInterval || 5000,
            guiPath: options.guiPath || '/essgui/',  // Path to append for GUI links
            showLocalBadge: options.showLocalBadge !== false,
            showSSLBadge: options.showSSLBadge !== false,
            showSubject: options.showSubject !== false,
            openInNewTab: options.openInNewTab !== false,  // For remote systems
            ...options
        };
        
        this.meshSystems = new Map();
        this.pollIntervalId = null;
        this.requestId = 'mesh-peers-' + Math.random().toString(36).substr(2, 9);
        
        this.render();
        this.setupEventListeners();
        this.startPolling();
    }
    
    /**
     * Render the dropdown HTML structure
     */
    render() {
        this.container.innerHTML = `
            <div class="mesh-dropdown">
                <button class="mesh-dropdown-btn" title="Connected Systems">
                    <span class="mesh-dropdown-icon">⬡</span>
                    <span class="mesh-dropdown-label">Local</span>
                    <span class="mesh-dropdown-arrow">▾</span>
                </button>
                <div class="mesh-dropdown-menu"></div>
            </div>
        `;
        
        this.elements = {
            dropdown: this.container.querySelector('.mesh-dropdown'),
            btn: this.container.querySelector('.mesh-dropdown-btn'),
            label: this.container.querySelector('.mesh-dropdown-label'),
            menu: this.container.querySelector('.mesh-dropdown-menu')
        };
    }
    
    /**
     * Setup event listeners
     */
    setupEventListeners() {
        // Toggle dropdown on button click
        this.elements.btn.addEventListener('click', (e) => {
            e.stopPropagation();
            this.elements.dropdown.classList.toggle('open');
        });
        
        // Close dropdown on outside click
        this.closeHandler = () => {
            this.elements.dropdown.classList.remove('open');
        };
        document.addEventListener('click', this.closeHandler);
        
        // Listen for mesh response from connection
        this.messageHandler = (data) => {
            if (data.requestId === this.requestId && data.status === 'ok') {
                this.handleMeshPeers(data.result);
            }
        };
        this.connection.on('message', this.messageHandler);
    }
    
    /**
     * Start polling for mesh peers
     */
    startPolling() {
        // Initial fetch
        this.fetchMeshPeers();
        
        // Poll at interval
        this.pollIntervalId = setInterval(() => this.fetchMeshPeers(), this.options.pollInterval);
    }
    
    /**
     * Stop polling
     */
    stopPolling() {
        if (this.pollIntervalId) {
            clearInterval(this.pollIntervalId);
            this.pollIntervalId = null;
        }
    }
    
    /**
     * Fetch mesh peers from server
     */
    fetchMeshPeers() {
        if (this.connection.connected) {
            this.connection.send({
                cmd: 'eval',
                script: 'send mesh {meshGetPeers}',
                requestId: this.requestId
            });
        }
    }
    
    /**
     * Parse Tcl list of dicts: {key1 val1 key2 val2} {key1 val1 ...}
     */
    parseTclList(str) {
        const results = [];
        let depth = 0;
        let current = '';
        
        for (let i = 0; i < str.length; i++) {
            const char = str[i];
            if (char === '{') {
                if (depth === 0) {
                    current = '';
                } else {
                    current += char;
                }
                depth++;
            } else if (char === '}') {
                depth--;
                if (depth === 0) {
                    results.push(current.trim());
                    current = '';
                } else {
                    current += char;
                }
            } else if (depth > 0) {
                current += char;
            }
        }
        return results;
    }
    
    /**
     * Parse Tcl dict: key1 val1 key2 {val with spaces} key3 val3
     */
    parseTclDict(str) {
        const dict = {};
        const tokens = [];
        let current = '';
        let depth = 0;
        
        for (let i = 0; i < str.length; i++) {
            const char = str[i];
            if (char === '{') {
                if (depth > 0) current += char;
                depth++;
            } else if (char === '}') {
                depth--;
                if (depth > 0) current += char;
            } else if (char === ' ' && depth === 0) {
                if (current) {
                    tokens.push(current);
                    current = '';
                }
            } else {
                current += char;
            }
        }
        if (current) tokens.push(current);
        
        for (let i = 0; i < tokens.length - 1; i += 2) {
            dict[tokens[i]] = tokens[i + 1];
        }
        return dict;
    }
    
    /**
     * Handle mesh peers response
     */
    handleMeshPeers(result) {
        if (!result) return;
        
        try {
            const peerStrings = this.parseTclList(result);
            
            this.meshSystems.clear();
            peerStrings.forEach(peerStr => {
                const peer = this.parseTclDict(peerStr);
                if (peer.id) {
                    peer.ssl = peer.ssl === '1' || peer.ssl === 1;
                    peer.isLocal = peer.isLocal === '1' || peer.isLocal === 1;
                    this.meshSystems.set(peer.id, peer);
                }
            });
            
            this.renderMenu();
        } catch (e) {
            console.error('MeshDropdown: Failed to parse mesh peers:', e);
        }
    }
    
    /**
     * Get URL for a system
     */
    getSystemUrl(sys) {
        if (sys.isLocal) {
            return window.location.href;
        }
        const protocol = sys.ssl ? 'https' : 'http';
        const port = sys.webPort || 2565;
        return `${protocol}://${sys.ip}:${port}${this.options.guiPath}`;
    }
    
    /**
     * Render the dropdown menu items
     */
    renderMenu() {
        if (this.meshSystems.size === 0) {
            this.elements.menu.innerHTML = '<div class="mesh-dropdown-empty">No mesh systems found</div>';
            return;
        }
        
        // Sort: local first, then by name
        const sorted = Array.from(this.meshSystems.values()).sort((a, b) => {
            if (a.isLocal && !b.isLocal) return -1;
            if (!a.isLocal && b.isLocal) return 1;
            return (a.name || a.id).localeCompare(b.name || b.id);
        });
        
        // Update label with local system name
        const local = sorted.find(s => s.isLocal);
        if (local) {
            const displayName = this.formatName(local.name || local.id);
            this.elements.label.textContent = displayName;
        }
        
        // Render menu items
        this.elements.menu.innerHTML = sorted.map(sys => {
            const url = this.getSystemUrl(sys);
            const status = sys.status || 'idle';
            const displayName = this.formatName(sys.name || sys.id);
            const target = sys.isLocal ? '_self' : (this.options.openInNewTab ? '_blank' : '_self');
            
            let badges = '';
            if (sys.isLocal && this.options.showLocalBadge) {
                badges += '<span class="mesh-dropdown-badge local">Local</span>';
            }
            if (sys.ssl && this.options.showSSLBadge) {
                badges += '<span class="mesh-dropdown-badge ssl">SSL</span>';
            }
            
            let subject = '';
            if (this.options.showSubject && sys.subject && sys.subject !== 'human') {
                subject = `<span class="mesh-dropdown-subject">${sys.subject}</span>`;
            }
            
            return `
                <a href="${url}" class="mesh-dropdown-item ${sys.isLocal ? 'current' : ''}" target="${target}">
                    <span class="mesh-dropdown-status ${status}"></span>
                    <div class="mesh-dropdown-info">
                        <div class="mesh-dropdown-name">${displayName}</div>
                        <div class="mesh-dropdown-details">
                            <span>${sys.ip || 'local'}</span>
                            ${subject}
                        </div>
                    </div>
                    ${badges}
                </a>
            `;
        }).join('');
    }
    
    /**
     * Format system name for display (remove common prefixes)
     */
    formatName(name) {
        return name.replace('Lab Station ', '');
    }
    
    /**
     * Get current mesh systems
     */
    getSystems() {
        return Array.from(this.meshSystems.values());
    }
    
    /**
     * Get local system info
     */
    getLocalSystem() {
        return Array.from(this.meshSystems.values()).find(s => s.isLocal);
    }
    
    /**
     * Manually refresh mesh peers
     */
    refresh() {
        this.fetchMeshPeers();
    }
    
    /**
     * Destroy the component and cleanup
     */
    destroy() {
        this.stopPolling();
        document.removeEventListener('click', this.closeHandler);
        // Note: connection.off() would be needed if ws_manager supports it
        this.container.innerHTML = '';
    }
}

// Export for module usage
if (typeof module !== 'undefined' && module.exports) {
    module.exports = MeshDropdown;
}

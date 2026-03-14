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
        
        if (!this.container) {
            console.warn('MeshDropdown: Container not found');
            this.initialized = false;
            return;
        }
        
        this.connection = connection;
        this.dpManager = options.dpManager || null;
        this.initialized = true;
        
        this.options = {
            guiPath: options.guiPath || '/ess_control',  // Path to append for GUI links
            showLocalBadge: options.showLocalBadge !== false,
            showSSLBadge: options.showSSLBadge !== false,
            showSubject: options.showSubject !== false,
            openInNewTab: options.openInNewTab !== false,  // For remote systems
            ...options
        };
        
        this.meshSystems = new Map();
        
        this.render();
        this.setupEventListeners();
        this.setupSubscription();
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
        if (!this.elements || !this.elements.btn) {
            console.error('MeshDropdown: Elements not initialized');
            return;
        }
        
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
    }
    
    /**
     * Subscribe to mesh/peers datapoint via DatapointManager
     */
    setupSubscription() {
        if (!this.dpManager) {
            console.warn('MeshDropdown: No DatapointManager, mesh peers will not update');
            return;
        }
        
        this.dpManager.subscribe('mesh/peers', (data) => {
            this.handleMeshPeers(data.value);
        });
    }
    
    /**
     * Handle mesh peers data from mesh/peers datapoint (JSON array)
     * Each peer has: hostname, ip, port, ssl, isLocal, status, workgroup, customFields
     */
    handleMeshPeers(result) {
        if (!result) return;
        
        try {
            // mesh/peers is published as JSON by meshconf.tcl
            let peers = result;
            if (typeof result === 'string') {
                peers = JSON.parse(result);
            }
            
            if (!Array.isArray(peers)) return;
            
            this.meshSystems.clear();
            peers.forEach(peer => {
                // Normalize field names for renderMenu
                const key = peer.hostname || peer.ip || 'unknown';
                const custom = peer.customFields || {};
                
                this.meshSystems.set(key, {
                    hostname: peer.hostname || '',
                    ip: peer.ip || '',
                    port: peer.port || 2565,
                    ssl: !!peer.ssl,
                    isLocal: !!peer.isLocal,
                    status: peer.status || 'idle',
                    workgroup: peer.workgroup || '',
                    subject: custom.subject || '',
                    system: custom.system || '',
                    protocol: custom.protocol || '',
                    lastSeenAgo: peer.lastSeenAgo || 0
                });
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
        const port = sys.port || 2565;
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
        
        // Sort: local first, then by hostname
        const sorted = Array.from(this.meshSystems.values()).sort((a, b) => {
            if (a.isLocal && !b.isLocal) return -1;
            if (!a.isLocal && b.isLocal) return 1;
            return (a.hostname).localeCompare(b.hostname);
        });
        
        // Update button label with local system name
        const local = sorted.find(s => s.isLocal);
        if (local) {
            this.elements.label.textContent = this.formatName(local.hostname);
        }
        
        // Render menu items
        this.elements.menu.innerHTML = sorted.map(sys => {
            const url = this.getSystemUrl(sys);
            const status = sys.status || 'idle';
            const displayName = this.formatName(sys.hostname);
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
     * Manually refresh mesh peers (touch the datapoint)
     */
    refresh() {
        if (this.connection?.connected && this.connection?.ws) {
            this.connection.ws.send(JSON.stringify({
                cmd: 'eval',
                script: 'catch {dservTouch mesh/peers}'
            }));
        }
    }
    
    /**
     * Destroy the component and cleanup
     */
    destroy() {
        document.removeEventListener('click', this.closeHandler);
        this.container.innerHTML = '';
    }
}

// Export for module usage
if (typeof module !== 'undefined' && module.exports) {
    module.exports = MeshDropdown;
}

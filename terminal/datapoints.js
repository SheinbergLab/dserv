// ============================================
// Datapoint Explorer Module
// ============================================

class DatapointExplorer {
    constructor(wsManager) {
        this.ws = wsManager;
        this.datapoints = new Map(); // name -> { value, timestamp, dtype, history }
        this.subscribedPoints = new Set();
        this.allKeys = [];
        this.selectedDatapoint = null;

        // DOM elements
        this.listElement = document.getElementById('datapoint-list');
        this.detailElement = document.getElementById('datapoint-detail');
        this.searchInput = document.getElementById('search-datapoints');
        this.updateCountElement = document.getElementById('update-count');

        // Listen for WebSocket events
        this.ws.on('connected', () => this.handleConnected());
        this.ws.on('datapoint:update', (data) => this.handleDatapointUpdate(data));
        this.ws.on('datapoint:keys', (data) => this.handleKeysUpdate(data));

        this.init();
    }

    init() {
        // Search functionality
        this.searchInput.addEventListener('input', (e) => {
            this.filterDatapoints(e.target.value);
        });

        this.searchInput.addEventListener('keydown', (e) => {
            if (e.key === 'Escape') {
                this.clearSearch();
            }
        });

        document.getElementById('clear-search').addEventListener('click', () => {
            this.clearSearch();
        });

        // Buttons
        document.getElementById('refresh-keys').addEventListener('click', () => {
            this.refreshKeys();
        });

        document.getElementById('unsubscribe-all').addEventListener('click', () => {
            this.unsubscribeAll();
        });
    }

    handleConnected() {
        this.showPlaceholder('Connected. Loading datapoints...');
        this.refreshKeys();
    }

    refreshKeys() {
        // Subscribe to @keys to get list of all datapoints
        this.ws.send({
            cmd: 'subscribe',
            match: '@keys',
            every: 1
        });
        // Also get current value (like datapoint_explorer does)
        this.ws.send({
            cmd: 'get',
            name: '@keys'
        });
    }

    handleKeysUpdate(data) {
        console.log('handleKeysUpdate called with:', data);
        if (data.name === '@keys' && (data.value || data.data)) {
            try {
                // dserv sends space-separated string (Tcl list): "dserv ess msg ..."
                const keysData = data.data !== undefined ? data.data : data.value;
                console.log('Keys data:', keysData, 'Type:', typeof keysData);
                
                let keys;
                if (typeof keysData === 'string') {
                    // Split on whitespace, like datapoint_explorer does
                    keys = keysData.trim().split(/\s+/).filter(k => k.length > 0);
                } else if (Array.isArray(keysData)) {
                    // Already an array
                    keys = keysData;
                } else {
                    console.error('Unexpected keys format:', keysData);
                    return;
                }
                
                console.log('Parsed keys:', keys);
                this.allKeys = keys;
                this.allKeys.sort();
                console.log('Rendering datapoint list with', this.allKeys.length, 'keys');
                this.renderDatapointList();
            } catch (e) {
                console.error('Failed to parse keys:', e);
            }
        } else {
            console.log('Keys update rejected - missing data. name:', data.name, 'has data/value:', !!(data.data || data.value));
        }
    }

    handleDatapointUpdate(data) {
        const name = data.name;
        if (!name) return;

        // dserv sends 'data' field, not 'value'
        const value = data.data !== undefined ? data.data : data.value;

        // Update datapoint data
        let dpData = this.datapoints.get(name);
        if (!dpData) {
            dpData = {
                value: null,
                timestamp: null,
                dtype: null,
                history: []
            };
            this.datapoints.set(name, dpData);
        }

        dpData.value = value;
        dpData.timestamp = data.timestamp;
        dpData.dtype = data.dtype;
        dpData.history.push({
            time: new Date(),
            value: value,
            timestamp: data.timestamp
        });

        // Keep history limited
        if (dpData.history.length > 100) {
            dpData.history.shift();
        }

        // Update UI if this is the selected datapoint
        if (this.selectedDatapoint === name) {
            this.renderDatapointDetail(name);
        }

        // Update subscribed count
        this.updateSubscribedCount();
    }

    renderDatapointList(filter = '') {
        const filtered = filter 
            ? this.allKeys.filter(key => key.toLowerCase().includes(filter.toLowerCase()))
            : this.allKeys;

        if (filtered.length === 0) {
            this.showPlaceholder(filter ? 'No matching datapoints' : 'No datapoints available');
            return;
        }

        this.listElement.innerHTML = '';

        filtered.forEach(name => {
            const item = document.createElement('div');
            item.className = 'datapoint-item';
            item.id = `dp-${name}`;
            item.textContent = name;

            if (this.subscribedPoints.has(name)) {
                item.classList.add('subscribed');
            }

            if (this.selectedDatapoint === name) {
                item.classList.add('selected');
            }

            // Click to select/subscribe
            item.addEventListener('click', (e) => {
                // Check if clicking on the unsubscribe X
                const rect = item.getBoundingClientRect();
                const clickX = e.clientX - rect.left;
                
                if (this.subscribedPoints.has(name) && clickX > rect.width - 35) {
                    // Clicked on X - unsubscribe
                    this.unsubscribe(name);
                } else {
                    // Normal click - select and subscribe if not already
                    this.selectDatapoint(name);
                    if (!this.subscribedPoints.has(name)) {
                        this.subscribe(name);
                    }
                }
            });

            this.listElement.appendChild(item);
        });
    }

    selectDatapoint(name) {
        // Update selection
        this.selectedDatapoint = name;

        // Update UI
        const items = this.listElement.querySelectorAll('.datapoint-item');
        items.forEach(item => item.classList.remove('selected'));

        const selectedItem = document.getElementById(`dp-${name}`);
        if (selectedItem) {
            selectedItem.classList.add('selected');
        }

        // If we don't have data yet, get it (like datapoint_explorer does)
        if (!this.datapoints.has(name)) {
            console.log(`Getting initial value for: ${name}`);
            this.ws.send({
                cmd: 'get',
                name: name
            });
        }

        this.renderDatapointDetail(name);
    }

    renderDatapointDetail(name) {
        const dpData = this.datapoints.get(name);

        if (!dpData || dpData.value === null) {
            this.detailElement.innerHTML = `
                <div class="placeholder">Waiting for data from: ${name}</div>
            `;
            return;
        }

        const history = dpData.history || [];
        const historyHtml = history.slice(-20).reverse().map(update => `
            <div class="update-entry">
                <div class="update-time">${update.time.toLocaleTimeString()}</div>
                <div>${this.formatValue(update.value)}</div>
            </div>
        `).join('');

        this.detailElement.innerHTML = `
            <div class="detail-row">
                <div class="detail-label">Name:</div>
                <div class="detail-value">${name}</div>
            </div>
            <div class="detail-row">
                <div class="detail-label">Datatype:</div>
                <div class="detail-value">${this.getDatatypeName(dpData.dtype)}</div>
            </div>
            <div class="detail-row">
                <div class="detail-label">Timestamp:</div>
                <div class="detail-value">${dpData.timestamp || 'unknown'}</div>
            </div>
            <div class="detail-row">
                <div class="detail-label">Current Value:</div>
                <div class="detail-value">${this.formatValue(dpData.value)}</div>
            </div>
            <div class="detail-row">
                <div class="detail-label">Update History (last 20):</div>
                <div class="update-history">
                    ${historyHtml || '<div class="placeholder">No history</div>'}
                </div>
            </div>
        `;
    }

    formatValue(value) {
        if (value === null || value === undefined) {
            return '<null>';
        }
        if (typeof value === 'object') {
            return JSON.stringify(value, null, 2);
        }
        return String(value);
    }

    getDatatypeName(dtype) {
        const types = {
            0: 'BYTE',
            1: 'STRING',
            2: 'FLOAT',
            3: 'DOUBLE',
            4: 'SHORT',
            5: 'INT',
            6: 'DG',
            7: 'SCRIPT',
            8: 'TRIGGER_SCRIPT',
            9: 'EVENT',
            10: 'NONE',
            11: 'JSON',
            12: 'ARROW',
            13: 'MSGPACK',
            14: 'JPEG',
            15: 'PPM',
            16: 'INT64',
            17: 'UNKNOWN'
        };
        return types[dtype] || `Unknown (${dtype})`;
    }

    subscribe(pattern) {
        if (this.ws.send({
            cmd: 'subscribe',
            match: pattern,
            every: 1
        })) {
            this.subscribedPoints.add(pattern);
            
            const item = document.getElementById(`dp-${pattern}`);
            if (item) {
                item.classList.add('subscribed');
            }

            console.log(`Subscribed to: ${pattern}`);
            this.updateSubscribedCount();
        }
    }

    unsubscribe(pattern) {
        if (this.ws.send({
            cmd: 'unsubscribe',
            match: pattern
        })) {
            this.subscribedPoints.delete(pattern);
            
            const item = document.getElementById(`dp-${pattern}`);
            if (item) {
                item.classList.remove('subscribed');
            }

            console.log(`Unsubscribed from: ${pattern}`);
            this.updateSubscribedCount();
        }
    }

    unsubscribeAll() {
        const toUnsubscribe = Array.from(this.subscribedPoints);
        toUnsubscribe.forEach(pattern => {
            if (pattern !== '@keys') {
                this.unsubscribe(pattern);
            }
        });
    }

    updateSubscribedCount() {
        const count = this.subscribedPoints.size - (this.subscribedPoints.has('@keys') ? 1 : 0);
        this.updateCountElement.textContent = `${count} subscribed`;
    }

    filterDatapoints(filter) {
        this.renderDatapointList(filter);
        
        // Show/hide clear button
        const clearBtn = document.getElementById('clear-search');
        clearBtn.style.display = filter ? 'block' : 'none';
    }

    clearSearch() {
        this.searchInput.value = '';
        this.filterDatapoints('');
    }

    showPlaceholder(message) {
        this.listElement.innerHTML = `<div class="placeholder">${message}</div>`;
    }
}

// ============================================
// Initialize datapoint explorer when DOM is ready
// ============================================

let datapointExplorer;

document.addEventListener('DOMContentLoaded', () => {
    datapointExplorer = new DatapointExplorer(wsManager);
});

/**
 * EventViewer - Display and manage event log from dserv
 */

class EventViewer {
    constructor(containerId, options = {}) {
        this.container = document.getElementById(containerId);
        this.options = {
            autoScroll: options.autoScroll !== undefined ? options.autoScroll : true,
            onEventSelect: options.onEventSelect || null,
            ...options
        };
        
        // Event storage
        this.allEvents = [];           // All events across all observations
        this.observations = [];        // Array of observations with events
        this.currentObsIndex = -1;     // -1 = live mode
        this.currentObsNumber = -1;    // Current observation number
        this.obsStartTime = 0;         // Observation start timestamp
        
        // Event names
        this.eventTypeNames = new Array(256).fill(null);
        this.eventSubtypeNames = {};   // Maps "type:subtype" to name
        
        // System state tracking (from events 7 and 18)
        this.systemState = null;       // Current state name (from type 7)
        this.systemInfo = {
            system: null,
            protocol: null,
            variant: null
        };
        
        // Filtering
        this.typeFilter = '';
        this.searchFilter = '';
        
        // UI state
        this.selectedEventIndex = null;
        
        // Initialize event type names
        this.initializeEventNames();
    }
    
    /**
     * Initialize default event names
     */
    initializeEventNames() {
        // Reserved events (0-15)
        for (let i = 0; i < 16; i++) {
            this.eventTypeNames[i] = `Reserved${i}`;
        }
        
        // System events (16-127)
        for (let i = 16; i < 128; i++) {
            this.eventTypeNames[i] = `System${i}`;
        }
        
        // User events (128-255)
        for (let i = 128; i < 256; i++) {
            this.eventTypeNames[i] = `User${i}`;
        }
        
        // Common system event names
        this.eventTypeNames[1] = 'NAMESET';
        this.eventTypeNames[2] = 'FILEIO';
        this.eventTypeNames[3] = 'USER';
        this.eventTypeNames[6] = 'SUBTYPE_NAMES';
        this.eventTypeNames[18] = 'SYSTEM_CHANGES';
        this.eventTypeNames[19] = 'BEGINOBS';
        this.eventTypeNames[20] = 'ENDOBS';
    }
    
    /**
     * Check if an event should be displayed
     */
    shouldDisplayEvent(event) {
        // Filter out internal system events that don't need to be shown
        
        // Type 1 (NAMESET) - used internally for name mapping
        if (event.type === 1) return false;
        
        // Type 3 (USER_INTERACTION) - don't display
        if (event.type === 3) return false;
        
        // Type 5 (PARAM_SETTING) - don't display
        if (event.type === 5) return false;
        
        // Type 6 (SUBTYPE_NAMES) - used internally for subtype name mapping
        if (event.type === 6) return false;
        
        // Type 7 (STATE) - used for header, don't display in table
        if (event.type === 7) return false;
        
        // Type 18 (SYSTEM_CHANGES) - used for header, don't display in table
        if (event.type === 18) return false;
        
        // Add more filtered event types here as needed
        // Example: if (event.type === X && event.subtype === Y) return false;
        
        return true;
    }
    
    /**
     * Handle incoming event datapoint from dserv
     */
    handleEventDatapoint(eventValue, dpData) {
        try {
            const event = {
                type: eventValue.e_type !== undefined ? eventValue.e_type : eventValue.type,
                subtype: eventValue.e_subtype !== undefined ? eventValue.e_subtype : eventValue.subtype,
                timestamp: eventValue.timestamp,
                ptype: eventValue.e_dtype !== undefined ? eventValue.e_dtype : (eventValue.dtype !== undefined ? eventValue.dtype : eventValue.ptype),
                params: eventValue.e_params !== undefined ? eventValue.e_params : eventValue.params
            };
            
            // Handle params cleanup
            if (event.params) {
                // Convert to string if it's an array or object
                if (Array.isArray(event.params)) {
                    event.params = JSON.stringify(event.params);
                } else if (typeof event.params === 'object') {
                    event.params = JSON.stringify(event.params);
                }
                
                // Strip wrapping quotes, brackets, or braces
                if (typeof event.params === 'string' && event.params.length > 1) {
                    const first = event.params[0];
                    const last = event.params[event.params.length - 1];
                    if ((first === '"' && last === '"') || 
                        (first === '[' && last === ']') ||
                        (first === '{' && last === '}')) {
                        event.params = event.params.slice(1, -1);
                    }
                }
            }
            
            // Process special events (still process them for internal state)
            this.processSpecialEvent(event);
            
            // Check if this event should be displayed
            if (!this.shouldDisplayEvent(event)) {
                return;
            }
            
            // Add to current observation
            this.addEvent(event);
            
            // Re-render if viewing live
            if (this.currentObsIndex === -1) {
                this.render();
            }
        } catch (e) {
            console.error('Failed to handle event:', e, eventValue);
            console.error('Stack:', e.stack);
        }
    }
    
    /**
     * Process special event types (names, obs control, etc.)
     */
    processSpecialEvent(event) {
        switch (event.type) {
            case 1: // NAMESET
                this.eventTypeNames[event.subtype] = this.decodeParams(event);
                break;
                
            case 6: // SUBTYPE_NAMES
                try {
                    const paramsStr = this.decodeParams(event);
                    const parts = paramsStr.split(' ');
                    for (let i = 0; i < parts.length; i += 2) {
                        if (i + 1 < parts.length) {
                            const key = `${event.subtype}:${parts[i + 1]}`;
                            this.eventSubtypeNames[key] = parts[i];
                        }
                    }
                } catch (e) {
                    console.error('Failed to parse subtype names:', e);
                }
                break;
                
            case 7: // STATE
                // State is in the subtype (0=Stopped, 1=Running, 2=Inactive, 3=Loading)
                const stateNames = {
                    0: 'Stopped',
                    1: 'Running',
                    2: 'Inactive',
                    3: 'Loading'
                };
                this.systemState = stateNames[event.subtype] || `State${event.subtype}`;
                console.log('STATE event - subtype:', event.subtype, 'systemState set to:', this.systemState);
                this.updateHeader();
                break;
                
            case 18: // SYSTEM_CHANGES
                // Parse system/protocol/variant from params (colon-separated)
                try {
                    const paramsStr = this.decodeParams(event);
                    console.log('SYSTEM_CHANGES event - params:', paramsStr);
                    const parts = paramsStr.split(':');
                    
                    // Format is "system:protocol:variant"
                    this.systemInfo.system = parts[0] || null;
                    this.systemInfo.protocol = parts[1] || null;
                    this.systemInfo.variant = parts[2] || null;
                    
                    console.log('systemInfo set to:', this.systemInfo);
                    this.updateHeader();
                } catch (e) {
                    console.error('Failed to parse system changes:', e);
                }
                break;
                
            case 3: // USER event - system control
                switch (event.subtype) {
                    case 2: // Reset
                        this.obsReset();
                        break;
                }
                break;
                
            case 19: // BEGINOBS
                this.obsStart(event);
                break;
                
            case 20: // ENDOBS
                this.obsEnd(event);
                break;
        }
    }
    
    /**
     * Update header with system state information
     */
    updateHeader() {
        const stateEl = document.getElementById('system-state');
        const infoEl = document.getElementById('system-info');
        
        if (stateEl && this.systemState) {
            // Style the state badge based on the state
            const stateClass = this.systemState.toLowerCase();
            stateEl.className = `event-state-badge event-state-${stateClass}`;
            stateEl.textContent = this.systemState;
            stateEl.style.display = 'inline-block';
        }
        
        if (infoEl) {
            const parts = [];
            if (this.systemInfo.system) parts.push(this.systemInfo.system);
            if (this.systemInfo.protocol) parts.push(this.systemInfo.protocol);
            if (this.systemInfo.variant) parts.push(this.systemInfo.variant);
            
            if (parts.length > 0) {
                infoEl.textContent = parts.join(' / ');
            }
        }
    }
    
    /**
     * Reset observations
     */
    obsReset() {
        console.log('Observation reset');
        this.currentObsNumber = -1;
        this.observations = [];
        this.allEvents = [];
        this.currentObsIndex = -1;
        this.render();
    }
    
    /**
     * Start new observation
     */
    obsStart(event) {
        console.log('Observation start:', event.timestamp);
        
        // Save previous observation if it exists
        if (this.currentObsNumber >= 0 && this.allEvents.length > 0) {
            this.observations.push({
                obsNumber: this.currentObsNumber,
                events: [...this.allEvents],
                startTime: this.obsStartTime
            });
        }
        
        // Start new observation
        this.currentObsNumber++;
        this.obsStartTime = event.timestamp;
        this.allEvents = [];
        this.currentObsIndex = -1; // Return to live view
        
        // Update UI to reflect new observation state
        if (typeof updateObservationUI === 'function') {
            updateObservationUI();
        }
    }
    
    /**
     * End observation
     */
    obsEnd(event) {
        console.log('Observation end:', event.timestamp);
        // Events are already added to allEvents
    }
    
    /**
     * Add event to current observation
     */
    addEvent(event) {
        // Calculate relative timestamp if in observation
        let displayTimestamp = event.timestamp;
        if (this.currentObsNumber >= 0) {
            displayTimestamp = event.timestamp - this.obsStartTime;
        }
        
        // Create display event
        const displayEvent = {
            ...event,
            displayTimestamp: displayTimestamp,
            obsNumber: this.currentObsNumber
        };
        
        this.allEvents.push(displayEvent);
        
        // Auto-scroll if enabled and viewing live
        if (this.options.autoScroll && this.currentObsIndex === -1) {
            setTimeout(() => this.scrollToBottom(), 10);
        }
    }
    
    /**
     * Decode event parameters based on type
     */
    decodeParams(event) {
        if (!event.params) return '';
        
        try {
            let params = event.params;
            
            // Handle string params (often wrapped in quotes)
            if (typeof params === 'string') {
                // Remove leading/trailing quotes if present
                if (params.startsWith('"') && params.endsWith('"')) {
                    params = params.slice(1, -1);
                }
            }
            // Handle array params
            else if (Array.isArray(params)) {
                params = JSON.stringify(params);
            }
            // Handle object params
            else if (typeof params === 'object') {
                params = JSON.stringify(params);
            }
            else {
                params = String(params);
            }
            
            // Format floats/doubles to 2 decimal places
            if (event.ptype === 2 || event.ptype === 3) {
                // Handle comma-separated values
                if (params.includes(',')) {
                    const values = params.split(',').map(v => {
                        const trimmed = v.trim();
                        const num = parseFloat(trimmed);
                        return isNaN(num) ? trimmed : num.toFixed(2);
                    });
                    return values.join(', ');
                }
                // Handle space-separated values (from arrays)
                else if (params.includes(' ')) {
                    const values = params.split(/\s+/).map(v => {
                        const num = parseFloat(v);
                        return isNaN(num) ? v : num.toFixed(2);
                    });
                    return values.join(' ');
                }
                // Handle single value
                else {
                    const num = parseFloat(params);
                    if (!isNaN(num)) {
                        return num.toFixed(2);
                    }
                }
            }
            
            return params;
        } catch (e) {
            return String(event.params);
        }
    }
    
    /**
     * Get event type name
     */
    getEventTypeName(typeNum) {
        return this.eventTypeNames[typeNum] || `Type${typeNum}`;
    }
    
    /**
     * Get event subtype name
     */
    getEventSubtypeName(typeNum, subtypeNum) {
        const key = `${typeNum}:${subtypeNum}`;
        return this.eventSubtypeNames[key] || `${subtypeNum}`;
    }
    
    /**
     * Get filtered/searched events for current view
     */
    getDisplayedEvents() {
        let events;
        
        if (this.currentObsIndex === -1) {
            // Live view - current observation
            events = this.allEvents;
        } else {
            // Historical view
            events = this.observations[this.currentObsIndex]?.events || [];
        }
        
        // Apply type filter
        if (this.typeFilter) {
            const filterType = parseInt(this.typeFilter);
            events = events.filter(e => e.type === filterType);
        }
        
        // Apply search filter
        if (this.searchFilter) {
            const search = this.searchFilter.toLowerCase();
            events = events.filter(e => {
                const typeName = this.getEventTypeName(e.type).toLowerCase();
                const subtypeName = this.getEventSubtypeName(e.type, e.subtype).toLowerCase();
                const params = this.decodeParams(e).toLowerCase();
                return typeName.includes(search) || 
                       subtypeName.includes(search) || 
                       params.includes(search);
            });
        }
        
        return events;
    }
    
    /**
     * Render the event table
     */
    render() {
        const events = this.getDisplayedEvents();
        
        if (events.length === 0) {
            this.container.innerHTML = `
                <div class="event-empty-state">
                    <div class="event-empty-icon">📊</div>
                    <div class="event-empty-text">No events to display</div>
                    <div class="event-empty-hint">Events will appear here as they occur</div>
                </div>
            `;
            return;
        }
        
        // Build table
        const table = document.createElement('table');
        table.className = 'event-table';
        
        // Header
        const thead = document.createElement('thead');
        thead.innerHTML = `
            <tr>
                <th style="width: 80px;">Time (ms)</th>
                <th style="width: 60px;">Δt (ms)</th>
                <th style="width: 50px;">Type</th>
                <th style="width: 120px;">Type Name</th>
                <th style="width: 50px;">Sub</th>
                <th style="width: 120px;">Subtype Name</th>
                <th>Parameters</th>
            </tr>
        `;
        table.appendChild(thead);
        
        // Body
        const tbody = document.createElement('tbody');
        
        events.forEach((event, index) => {
            const row = this.createEventRow(event, index);
            tbody.appendChild(row);
        });
        
        table.appendChild(tbody);
        
        this.container.innerHTML = '';
        this.container.appendChild(table);
    }
    
    /**
     * Create table row for an event
     */
    createEventRow(event, index) {
        const row = document.createElement('tr');
        
        // Add special row classes
        if (event.type === 19) {
            row.classList.add('obs-start-row');
        } else if (event.type === 20) {
            row.classList.add('obs-end-row');
        } else if (event.type >= 128) {
            row.classList.add('user-event-row');
        }
        
        // Calculate elapsed time from previous event
        let elapsedTime = 0;
        const events = this.getDisplayedEvents();
        if (index > 0) {
            elapsedTime = event.displayTimestamp - events[index - 1].displayTimestamp;
        }
        
        // Timestamp (convert from microseconds to milliseconds)
        const timeCell = document.createElement('td');
        timeCell.className = 'event-cell-timestamp';
        timeCell.textContent = (event.displayTimestamp / 1000).toFixed(1);
        row.appendChild(timeCell);
        
        // Elapsed time (convert from microseconds to milliseconds)
        const elapsedCell = document.createElement('td');
        elapsedCell.className = 'event-cell-elapsed';
        elapsedCell.textContent = index > 0 ? (elapsedTime / 1000).toFixed(1) : '0.0';
        row.appendChild(elapsedCell);
        
        // Type number
        const typeCell = document.createElement('td');
        typeCell.className = 'event-cell-type';
        typeCell.textContent = event.type;
        row.appendChild(typeCell);
        
        // Type name with badge
        const typeNameCell = document.createElement('td');
        const typeBadge = document.createElement('span');
        typeBadge.className = 'event-type-badge';
        if (event.type < 16) {
            typeBadge.classList.add('event-type-reserved');
        } else if (event.type < 128) {
            typeBadge.classList.add('event-type-system');
        } else {
            typeBadge.classList.add('event-type-user');
        }
        typeBadge.textContent = this.getEventTypeName(event.type);
        typeNameCell.appendChild(typeBadge);
        row.appendChild(typeNameCell);
        
        // Subtype number
        const subtypeCell = document.createElement('td');
        subtypeCell.className = 'event-cell-subtype';
        subtypeCell.textContent = event.subtype;
        row.appendChild(subtypeCell);
        
        // Subtype name
        const subtypeNameCell = document.createElement('td');
        subtypeNameCell.textContent = this.getEventSubtypeName(event.type, event.subtype);
        row.appendChild(subtypeNameCell);
        
        // Parameters
        const paramsCell = document.createElement('td');
        paramsCell.className = 'event-cell-params';
        paramsCell.textContent = this.decodeParams(event);
        paramsCell.title = this.decodeParams(event);
        row.appendChild(paramsCell);
        
        // Click handler
        row.addEventListener('click', () => {
            this.selectEvent(index);
        });
        
        return row;
    }
    
    /**
     * Select an event
     */
    selectEvent(index) {
        // Clear previous selection
        const rows = this.container.querySelectorAll('tr');
        rows.forEach(r => r.classList.remove('selected'));
        
        // Select new event
        if (rows[index + 1]) { // +1 for header
            rows[index + 1].classList.add('selected');
        }
        
        this.selectedEventIndex = index;
        
        // Callback
        if (this.options.onEventSelect) {
            const events = this.getDisplayedEvents();
            this.options.onEventSelect(events[index], index);
        }
    }
    
    /**
     * Clear selection
     */
    clearSelection() {
        const rows = this.container.querySelectorAll('tr');
        rows.forEach(r => r.classList.remove('selected'));
        this.selectedEventIndex = null;
    }
    
    /**
     * Build inspector tree for selected event
     */
    buildInspectorTree(event) {
        const container = document.createElement('div');
        
        const fields = [
            { key: 'Type', value: `${event.type} (${this.getEventTypeName(event.type)})` },
            { key: 'Subtype', value: `${event.subtype} (${this.getEventSubtypeName(event.type, event.subtype)})` },
            { key: 'Timestamp', value: `${event.displayTimestamp} ms` },
            { key: 'Absolute Time', value: `${event.timestamp}` },
            { key: 'Observation', value: event.obsNumber >= 0 ? event.obsNumber : 'N/A' },
            { key: 'Param Type', value: event.ptype },
            { key: 'Parameters', value: this.decodeParams(event) }
        ];
        
        fields.forEach(({ key, value }) => {
            const item = document.createElement('div');
            item.className = 'event-inspector-item';
            
            const keyEl = document.createElement('div');
            keyEl.className = 'event-inspector-key';
            keyEl.textContent = key;
            item.appendChild(keyEl);
            
            const valueEl = document.createElement('div');
            valueEl.className = 'event-inspector-value';
            valueEl.textContent = value;
            item.appendChild(valueEl);
            
            container.appendChild(item);
        });
        
        // Add raw JSON view
        const jsonItem = document.createElement('div');
        jsonItem.className = 'event-inspector-item';
        
        const jsonKey = document.createElement('div');
        jsonKey.className = 'event-inspector-key';
        jsonKey.textContent = 'Raw Data';
        jsonItem.appendChild(jsonKey);
        
        const jsonContainer = document.createElement('div');
        jsonContainer.className = 'event-inspector-json';
        const jsonPre = document.createElement('pre');
        jsonPre.textContent = JSON.stringify(event, null, 2);
        jsonContainer.appendChild(jsonPre);
        jsonItem.appendChild(jsonContainer);
        
        container.appendChild(jsonItem);
        
        return container;
    }
    
    /**
     * Navigation methods
     */
    previousObservation() {
        if (this.currentObsIndex === -1) {
            // From live to last observation
            this.currentObsIndex = this.observations.length - 1;
        } else if (this.currentObsIndex > 0) {
            this.currentObsIndex--;
        }
        this.render();
        if (typeof updateObservationUI === 'function') {
            updateObservationUI();
        }
    }
    
    nextObservation() {
        if (this.currentObsIndex < this.observations.length - 1) {
            this.currentObsIndex++;
            this.render();
        } else if (this.currentObsIndex === this.observations.length - 1) {
            // Back to live
            this.currentObsIndex = -1;
            this.render();
        }
        if (typeof updateObservationUI === 'function') {
            updateObservationUI();
        }
    }
    
    jumpToLive() {
        this.currentObsIndex = -1;
        this.render();
        this.scrollToBottom();
        if (typeof updateObservationUI === 'function') {
            updateObservationUI();
        }
    }
    
    getViewInfo() {
        return {
            isLive: this.currentObsIndex === -1,
            currentObs: this.currentObsIndex >= 0 ? 
                this.observations[this.currentObsIndex].obsNumber : 
                this.currentObsNumber,
            canNavigateBack: this.currentObsIndex > 0 || 
                (this.currentObsIndex === -1 && this.observations.length > 0),
            canNavigateForward: this.currentObsIndex >= 0 && 
                this.currentObsIndex < this.observations.length - 1
        };
    }
    
    /**
     * Filter/search methods
     */
    setTypeFilter(type) {
        this.typeFilter = type;
        this.render();
    }
    
    setSearchFilter(search) {
        this.searchFilter = search;
        this.render();
    }
    
    setAutoScroll(enabled) {
        this.options.autoScroll = enabled;
    }
    
    /**
     * Utility methods
     */
    getCurrentEventCount() {
        return this.getDisplayedEvents().length;
    }
    
    scrollToBottom() {
        this.container.scrollTop = this.container.scrollHeight;
    }
    
    clearAllEvents() {
        this.obsReset();
    }
    
    /**
     * Export events to CSV
     */
    exportCSV() {
        const events = this.getDisplayedEvents();
        if (events.length === 0) return;
        
        // Build CSV
        const headers = ['Time_ms', 'Delta_ms', 'Type', 'TypeName', 'Subtype', 'SubtypeName', 'Parameters'];
        const rows = [headers.join(',')];
        
        events.forEach((event, index) => {
            let elapsedTime = 0;
            if (index > 0) {
                elapsedTime = event.displayTimestamp - events[index - 1].displayTimestamp;
            }
            
            const row = [
                event.displayTimestamp,
                index > 0 ? elapsedTime : 0,
                event.type,
                `"${this.getEventTypeName(event.type)}"`,
                event.subtype,
                `"${this.getEventSubtypeName(event.type, event.subtype)}"`,
                `"${this.decodeParams(event).replace(/"/g, '""')}"`
            ];
            rows.push(row.join(','));
        });
        
        const csv = rows.join('\n');
        
        // Download
        const blob = new Blob([csv], { type: 'text/csv' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        
        const viewType = this.currentObsIndex === -1 ? 'live' : `obs_${this.observations[this.currentObsIndex].obsNumber}`;
        a.download = `events_${viewType}_${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.csv`;
        
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
    }
    
    /**
     * Load demo data for testing
     */
    loadDemoData() {
        console.log('Loading demo data...');
        
        // Reset first
        this.obsReset();
        
        // Simulate observation start
        this.obsStart({ timestamp: Date.now(), type: 19, subtype: 0 });
        
        // Add some demo events
        const eventTypes = [
            { type: 128, subtype: 0, name: 'TRIAL_START' },
            { type: 129, subtype: 0, name: 'STIM_ON' },
            { type: 129, subtype: 1, name: 'STIM_OFF' },
            { type: 130, subtype: 0, name: 'RESPONSE' },
            { type: 131, subtype: 0, name: 'REWARD' },
        ];
        
        // Set event names
        eventTypes.forEach(evt => {
            this.eventTypeNames[evt.type] = evt.name;
        });
        
        // Generate events
        let time = this.obsStartTime;
        for (let i = 0; i < 50; i++) {
            const eventType = eventTypes[Math.floor(Math.random() * eventTypes.length)];
            time += Math.floor(Math.random() * 500) + 100;
            
            this.addEvent({
                type: eventType.type,
                subtype: eventType.subtype,
                timestamp: time,
                ptype: 1,
                params: `Trial ${i + 1} params`
            });
        }
        
        this.render();
    }
}
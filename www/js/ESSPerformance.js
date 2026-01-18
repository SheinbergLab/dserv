/**
 * ESSPerformance.js
 * 
 * Performance monitoring panel with sortby functionality.
 * Displays trial statistics grouped by stimulus variables.
 */

class ESSPerformance {
    constructor(dpManager, containerId = 'performance-container') {
        this.dpManager = dpManager;
        this.container = document.getElementById(containerId);
        
        // Current state
        this.blockId = null;
        this.sortby1 = '';
        this.sortby2 = '';
        this.sortableColumns = [];
        this.obsId = 0;
        this.obsTotal = 0;
        
        // DOM elements (will be created in init)
        this.elements = {};
        
        this.init();
        this.setupSubscriptions();
    }
    
    init() {
        // Build the UI
        this.container.innerHTML = `
            <div class="ess-perf-summary">
                <div class="ess-perf-stat">
                    <span class="ess-perf-label">Trial</span>
                    <span class="ess-perf-value" id="perf-trial">--/--</span>
                </div>
                <div class="ess-perf-stat">
                    <span class="ess-perf-label">% Correct</span>
                    <span class="ess-perf-value" id="perf-correct">--%</span>
                </div>
                <div class="ess-perf-stat">
                    <span class="ess-perf-label">% Complete</span>
                    <span class="ess-perf-value" id="perf-complete">--%</span>
                </div>
            </div>
            <div class="ess-perf-sortby">
                <label>Sort by:</label>
                <select id="perf-sortby1" class="ess-perf-select">
                    <option value="">--</option>
                </select>
                <select id="perf-sortby2" class="ess-perf-select">
                    <option value="">--</option>
                </select>
            </div>
            <div class="ess-perf-table-container">
                <table id="perf-table" class="ess-perf-table">
                    <thead>
                        <tr id="perf-table-header">
                            <th>Level</th>
                            <th>%Corr</th>
                            <th>RT</th>
                            <th>N</th>
                        </tr>
                    </thead>
                    <tbody id="perf-table-body">
                    </tbody>
                </table>
            </div>
        `;


        // Cache element references
        this.elements = {
            trial: document.getElementById('perf-trial'),
            correct: document.getElementById('perf-correct'),
            complete: document.getElementById('perf-complete'),
            sortby1: document.getElementById('perf-sortby1'),
            sortby2: document.getElementById('perf-sortby2'),
            tableHeader: document.getElementById('perf-table-header'),
            tableBody: document.getElementById('perf-table-body')
        };
        
        // Bind dropdown events
        this.elements.sortby1.addEventListener('change', (e) => {
            this.sortby1 = e.target.value;
	    console.log('sortby1 changed to:', e.target.value);
            this.requestSortedPerf();
        });
        
        this.elements.sortby2.addEventListener('change', (e) => {
            this.sortby2 = e.target.value;
	    console.log('sortby2 changed to:', e.target.value);
            this.requestSortedPerf();
        });
    }
    
    setupSubscriptions() {
        // Subscribe to sortable columns (published when stimdg changes)
        this.dpManager?.subscribe('ess/sortby_columns', (data) => {
            this.updateSortableColumns(data.value);
        });
        
        // Subscribe to block_id changes
        this.dpManager?.subscribe('ess/block_id', (data) => {
            this.blockId = parseInt(data.value) || 0;
            this.clearTable();
        });
        
        // Subscribe to trial completion to trigger refresh
        this.dpManager?.subscribe('ess/block_n_complete', (data) => {
            this.requestSortedPerf();
        });
        
        // Subscribe to overall performance stats
        this.dpManager?.subscribe('ess/block_pct_correct', (data) => {
            const pct = parseFloat(data.value);
            this.elements.correct.textContent = isNaN(pct) ? '--%' : `${(pct * 100).toFixed(0)}%`;
        });
        
        this.dpManager?.subscribe('ess/block_pct_complete', (data) => {
            const pct = parseFloat(data.value);
            this.elements.complete.textContent = isNaN(pct) ? '--%' : `${(pct * 100).toFixed(0)}%`;
        });
        
        // Track obs counts for trial display
        this.dpManager?.subscribe('ess/obs_id', (data) => {
            this.obsId = parseInt(data.value) || 0;
            this.updateTrialCount();
        });
        
        this.dpManager?.subscribe('ess/obs_total', (data) => {
            this.obsTotal = parseInt(data.value) || 0;
            this.updateTrialCount();
        });
    }
    
    updateTrialCount() {
        this.elements.trial.textContent = `${this.obsId + 1}/${this.obsTotal}`;
    }
    
    updateSortableColumns(jsonStr) {
        try {
            const columns = typeof jsonStr === 'string' ? JSON.parse(jsonStr) : jsonStr;
            this.sortableColumns = Array.isArray(columns) ? columns : [];
            
            // Preserve current selections if still valid
            const currentSort1 = this.sortby1;
            const currentSort2 = this.sortby2;
            
            // Rebuild dropdowns
            this.populateDropdown(this.elements.sortby1, this.sortableColumns, currentSort1);
            this.populateDropdown(this.elements.sortby2, this.sortableColumns, currentSort2);
            
            // Update state (may have been reset if selection no longer valid)
            this.sortby1 = this.elements.sortby1.value;
            this.sortby2 = this.elements.sortby2.value;
            
        } catch (e) {
            console.error('Error parsing sortby_columns:', e);
            this.sortableColumns = [];
        }
    }
    
    populateDropdown(select, columns, preserveValue = '') {
        // Clear existing options
        select.innerHTML = '<option value="">--</option>';
        
        // Add column options
        columns.forEach(col => {
            const option = document.createElement('option');
            option.value = col;
            option.textContent = col;
            select.appendChild(option);
        });
        
        // Restore selection if still valid
        if (preserveValue && columns.includes(preserveValue)) {
            select.value = preserveValue;
        }
    }
    
    async requestSortedPerf() {
 console.log('requestSortedPerf called:', {
        blockId: this.blockId,
        sortby1: this.sortby1,
        sortby2: this.sortby2
 });
        if (this.blockId === null) return;
        
        // Only request if at least one sortby is selected
        // (otherwise the summary stats are sufficient)
        if (!this.sortby1 && !this.sortby2) {
            this.clearTable();
            return;
        }
        
        try {
            const conn = this.dpManager?.connection;
            if (!conn || !conn.connected || !conn.ws) {
                return;
            }
            
            // Build the Tcl command - send to db subprocess
            const cmd = `send db {get_sorted_perf ${this.blockId} ${this.sortby1} ${this.sortby2}}`;
            const response = await conn.sendRaw(cmd);
            
            this.updateSortedPerf(response);
            
        } catch (error) {
            console.error('Failed to request sorted perf:', error);
        }
    }
    
    updateSortedPerf(jsonStr) {
        try {
            const data = typeof jsonStr === 'string' ? JSON.parse(jsonStr) : jsonStr;
            
            if (data.error) {
                this.clearTable();
                return;
            }
            
            this.renderTable(data);
            
        } catch (e) {
            console.error('Error parsing sorted perf:', e);
            this.clearTable();
        }
    }
    
    renderTable(data) {
        const { vars, levels, status, rt, count } = data;
        
        // Update header based on vars
        let headerHtml = '';
        if (vars.length === 0) {
            headerHtml = '<th>Overall</th>';
        } else {
            vars.forEach(v => {
                headerHtml += `<th>${this.escapeHtml(v)}</th>`;
            });
        }
        headerHtml += '<th>%Corr</th><th>RT</th><th>N</th>';
        this.elements.tableHeader.innerHTML = headerHtml;
        
        // Build rows
        let bodyHtml = '';
        const nRows = status.length;
        
        for (let i = 0; i < nRows; i++) {
            // Skip rows with 0 trials
            if (count[i] === 0) continue;
            
            bodyHtml += '<tr>';
            
            // Level columns
            if (vars.length === 0) {
                bodyHtml += '<td>All</td>';
            } else {
                const levelTuple = levels[i] || [];
                levelTuple.forEach(val => {
                    bodyHtml += `<td>${this.escapeHtml(String(val))}</td>`;
                });
            }
            
            // Stats columns
            const pctCorrect = (status[i] * 100).toFixed(0);
            const meanRt = rt[i].toFixed(0);
            const n = count[i];
            
            bodyHtml += `<td>${pctCorrect}%</td>`;
            bodyHtml += `<td>${meanRt}</td>`;
            bodyHtml += `<td>${n}</td>`;
            
            bodyHtml += '</tr>';
        }
        
        this.elements.tableBody.innerHTML = bodyHtml;
    }
    
    clearTable() {
        this.elements.tableBody.innerHTML = '';
    }
    
    escapeHtml(str) {
        const div = document.createElement('div');
        div.textContent = str;
        return div.innerHTML;
    }
}

// Export
if (typeof window !== 'undefined') {
    window.ESSPerformance = ESSPerformance;
}

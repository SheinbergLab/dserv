/**
 * StimDGViewer.js
 * Specialized viewer for stimulus dynamic group (stimdg) data
 * Designed to integrate with dserv/DatapointManager for live updates
 */

class StimDGViewer {
    constructor(containerId, data, options = {}) {
        this.container = document.getElementById(containerId);
        if (!this.container) {
            throw new Error(`Container element with id "${containerId}" not found`);
        }
        
        this.data = data;
        this.options = {
            pageSize: options.pageSize || 50,
            onRowSelect: options.onRowSelect || null,
            onArrayClick: options.onArrayClick || null,
            outcomeColumn: options.outcomeColumn || 'outcome',
            ...options
        };
        
        this.currentPage = 0;
        this.sortColumn = null;
        this.sortDirection = 'asc';
        this.selectedRowIndex = null;
        
        this.processData();
    }
    
    /**
     * Process hybrid JSON format into rows
     */
    processData() {
        if (!this.data) {
            this.processedRows = [];
            this.arrayColumns = [];
            this.name = 'No Data';
            this.rowCount = 0;
            return;
        }
        
        // Handle hybrid format (rows + arrays + name)
        if (this.data.rows && this.data.arrays) {
            this.name = this.data.name || 'stimdg';
            this.arrayColumns = Object.keys(this.data.arrays);
            
            this.processedRows = this.data.rows.map((row, index) => {
                const enhancedRow = { _row_index: index, ...row };
                
                // Replace array indices with actual array data
                this.arrayColumns.forEach(fieldName => {
                    if (fieldName in row && typeof row[fieldName] === 'number') {
                        const arrayIndex = row[fieldName];
                        if (this.data.arrays[fieldName] && this.data.arrays[fieldName][arrayIndex]) {
                            enhancedRow[fieldName] = this.data.arrays[fieldName][arrayIndex];
                        }
                    }
                });
                
                return enhancedRow;
            });
        }
        // Handle direct rows array
        else if (Array.isArray(this.data)) {
            this.name = 'stimdg';
            this.arrayColumns = [];
            this.processedRows = this.data.map((row, index) => ({
                _row_index: index,
                ...row
            }));
        }
        // Fallback
        else {
            this.name = 'stimdg';
            this.arrayColumns = [];
            this.processedRows = [];
        }
        
        // Extract column names from first row
        if (this.processedRows.length > 0) {
            this.columns = Object.keys(this.processedRows[0]).filter(key => key !== '_row_index');
        } else {
            this.columns = [];
        }
        
        this.rowCount = this.processedRows.length;
    }
    
    /**
     * Update data (for live subscription updates)
     */
    updateData(newData) {
        this.data = newData;
        this.processData();
        this.render();
    }
    
    /**
     * Append a single row (for incremental updates)
     */
    appendRow(row) {
        const index = this.processedRows.length;
        const enhancedRow = { _row_index: index, ...row };
        
        // Process array columns
        this.arrayColumns.forEach(fieldName => {
            if (fieldName in row && typeof row[fieldName] === 'number') {
                const arrayIndex = row[fieldName];
                if (this.data.arrays && this.data.arrays[fieldName] && this.data.arrays[fieldName][arrayIndex]) {
                    enhancedRow[fieldName] = this.data.arrays[fieldName][arrayIndex];
                }
            }
        });
        
        this.processedRows.push(enhancedRow);
        this.rowCount = this.processedRows.length;
        
        // Re-render if on last page or auto-scroll enabled
        const totalPages = Math.ceil(this.rowCount / this.options.pageSize);
        if (this.currentPage >= totalPages - 1) {
            this.render();
        }
    }
    
    /**
     * Main render function
     */
    render() {
        this.container.innerHTML = '';
        
        if (this.processedRows.length === 0) {
            this.container.innerHTML = `
                <div class="stimdg-empty-state">
                    <div class="stimdg-empty-icon">📋</div>
                    <div class="stimdg-empty-text">No data loaded</div>
                    <div class="stimdg-empty-hint">Connect to dserv to load stimulus data</div>
                </div>
            `;
            return;
        }
        
        const table = this.createTable();
        this.container.appendChild(table);
    }
    
    /**
     * Create table element
     */
    createTable() {
        const table = document.createElement('table');
        table.className = 'stimdg-table';
        
        // Header
        const thead = document.createElement('thead');
        const headerRow = document.createElement('tr');
        
        this.columns.forEach(col => {
            const th = document.createElement('th');
            th.textContent = col;
            
            // Add sorting for non-array columns
            const isArrayCol = this.arrayColumns.includes(col);
            if (!isArrayCol) {
                th.className = 'sortable';
                if (this.sortColumn === col) {
                    th.classList.add('sorted');
                    th.classList.add(this.sortDirection);
                }
                th.onclick = () => this.sortByColumn(col);
            }
            
            headerRow.appendChild(th);
        });
        
        thead.appendChild(headerRow);
        table.appendChild(thead);
        
        // Body
        const tbody = document.createElement('tbody');
        const start = this.currentPage * this.options.pageSize;
        const end = Math.min(start + this.options.pageSize, this.processedRows.length);
        
        for (let i = start; i < end; i++) {
            const row = this.processedRows[i];
            const tr = this.createRow(row);
            tbody.appendChild(tr);
        }
        
        table.appendChild(tbody);
        return table;
    }
    
    /**
     * Create a table row
     */
    createRow(row) {
        const tr = document.createElement('tr');
        
        if (row._row_index === this.selectedRowIndex) {
            tr.classList.add('selected');
        }
        
        tr.onclick = (e) => {
            if (e.target.classList.contains('stimdg-cell-array')) return;
            this.selectRow(row, row._row_index);
        };
        
        this.columns.forEach(col => {
            const td = document.createElement('td');
            const value = row[col];
            
            if (Array.isArray(value)) {
                // Array cell
                td.className = 'stimdg-cell-array';
                td.textContent = `[${value.length}]`;
                td.onclick = (e) => {
                    e.stopPropagation();
                    if (this.options.onArrayClick) {
                        this.options.onArrayClick(value, col, row._row_index);
                    }
                };
            } else if (col === this.options.outcomeColumn && value) {
                // Outcome badge
                const badge = document.createElement('span');
                badge.className = `stimdg-outcome ${value}`;
                badge.textContent = value;
                td.appendChild(badge);
            } else if (typeof value === 'number') {
                // Number formatting
                td.className = 'stimdg-cell-number';
                if (Number.isInteger(value)) {
                    td.textContent = value;
                } else {
                    td.textContent = value.toFixed(2);
                }
            } else if (value === null || value === undefined) {
                td.className = 'stimdg-cell-null';
                td.textContent = '—';
            } else {
                td.textContent = value;
            }
            
            tr.appendChild(td);
        });
        
        return tr;
    }
    
    /**
     * Select a row
     */
    selectRow(row, rowIndex) {
        this.selectedRowIndex = rowIndex;
        
        // Update selection styling
        const rows = this.container.querySelectorAll('tbody tr');
        rows.forEach(tr => tr.classList.remove('selected'));
        
        const start = this.currentPage * this.options.pageSize;
        const tableIndex = rowIndex - start;
        if (tableIndex >= 0 && tableIndex < rows.length) {
            rows[tableIndex].classList.add('selected');
        }
        
        if (this.options.onRowSelect) {
            this.options.onRowSelect(row, rowIndex);
        }
    }
    
    /**
     * Clear row selection
     */
    clearSelection() {
        this.selectedRowIndex = null;
        const rows = this.container.querySelectorAll('tbody tr');
        rows.forEach(tr => tr.classList.remove('selected'));
    }
    
    /**
     * Sort by column
     */
    sortByColumn(column) {
        if (this.sortColumn === column) {
            this.sortDirection = this.sortDirection === 'asc' ? 'desc' : 'asc';
        } else {
            this.sortColumn = column;
            this.sortDirection = 'asc';
        }
        
        this.processedRows.sort((a, b) => {
            let aVal = a[column];
            let bVal = b[column];
            
            if (aVal === null || aVal === undefined) return 1;
            if (bVal === null || bVal === undefined) return -1;
            
            if (typeof aVal === 'number' && typeof bVal === 'number') {
                return this.sortDirection === 'asc' ? aVal - bVal : bVal - aVal;
            }
            
            const result = String(aVal).localeCompare(String(bVal));
            return this.sortDirection === 'asc' ? result : -result;
        });
        
        this.currentPage = 0;
        this.render();
    }
    
    /**
     * Pagination
     */
    getPageInfo() {
        const totalPages = Math.ceil(this.processedRows.length / this.options.pageSize);
        const start = this.currentPage * this.options.pageSize + 1;
        const end = Math.min((this.currentPage + 1) * this.options.pageSize, this.processedRows.length);
        
        return {
            page: this.currentPage,
            totalPages,
            start,
            end,
            total: this.processedRows.length,
            hasPrev: this.currentPage > 0,
            hasNext: this.currentPage < totalPages - 1
        };
    }
    
    prevPage() {
        if (this.currentPage > 0) {
            this.currentPage--;
            this.render();
        }
    }
    
    nextPage() {
        const totalPages = Math.ceil(this.processedRows.length / this.options.pageSize);
        if (this.currentPage < totalPages - 1) {
            this.currentPage++;
            this.render();
        }
    }
    
    goToPage(page) {
        const totalPages = Math.ceil(this.processedRows.length / this.options.pageSize);
        if (page >= 0 && page < totalPages) {
            this.currentPage = page;
            this.render();
        }
    }
    
    /**
     * Build inspector tree view for a row
     */
    buildInspectorTree(row) {
        const container = document.createElement('div');
        
        this.columns.forEach(col => {
            const value = row[col];
            const item = document.createElement('div');
            item.className = 'stimdg-inspector-item';
            
            const key = document.createElement('div');
            key.className = 'stimdg-inspector-key';
            key.textContent = col;
            item.appendChild(key);
            
            if (Array.isArray(value)) {
                const header = document.createElement('div');
                header.className = 'stimdg-inspector-array-header';
                header.textContent = `Array[${value.length}] ▼`;
                
                const content = document.createElement('div');
                content.className = 'stimdg-inspector-array-content';
                
                const isNested = value.length > 0 && Array.isArray(value[0]);
                
                if (isNested) {
                    // 2D array preview
                    const preview = value.slice(0, 5).map((subArr, idx) => {
                        const vals = Array.isArray(subArr) 
                            ? subArr.map(v => typeof v === 'number' ? v.toFixed(2) : v).join(', ')
                            : subArr;
                        return `[${idx}]: [${vals}]`;
                    }).join('\n');
                    content.textContent = preview + (value.length > 5 ? '\n...' : '');
                } else {
                    // 1D array preview
                    const preview = value.slice(0, 10).map(v => 
                        typeof v === 'number' ? v.toFixed(2) : v
                    ).join(', ');
                    content.textContent = preview + (value.length > 10 ? ', ...' : '');
                }
                
                header.onclick = () => {
                    content.classList.toggle('collapsed');
                    header.textContent = content.classList.contains('collapsed')
                        ? `Array[${value.length}] ▶`
                        : `Array[${value.length}] ▼`;
                };
                
                item.appendChild(header);
                item.appendChild(content);
            } else {
                const valueEl = document.createElement('div');
                valueEl.className = 'stimdg-inspector-value';
                
                if (value === null || value === undefined) {
                    valueEl.textContent = '—';
                    valueEl.style.color = 'var(--stimdg-text-muted)';
                } else if (typeof value === 'number' && !Number.isInteger(value)) {
                    valueEl.textContent = value.toFixed(4);
                } else {
                    valueEl.textContent = value;
                }
                
                item.appendChild(valueEl);
            }
            
            container.appendChild(item);
        });
        
        return container;
    }
    
    /**
     * Build array view for modal
     */
    buildArrayView(arrayData) {
        const container = document.createElement('div');
        
        const isNested = arrayData.length > 0 && Array.isArray(arrayData[0]);
        
        if (isNested) {
            // 2D array - table format
            const table = document.createElement('table');
            table.className = 'stimdg-array-table';
            
            // Determine max columns
            const maxCols = Math.max(...arrayData.map(row => Array.isArray(row) ? row.length : 0));
            
            // Header
            const thead = document.createElement('thead');
            const headerRow = document.createElement('tr');
            const thIdx = document.createElement('th');
            thIdx.textContent = '#';
            headerRow.appendChild(thIdx);
            
            for (let i = 0; i < maxCols; i++) {
                const th = document.createElement('th');
                th.textContent = `[${i}]`;
                headerRow.appendChild(th);
            }
            thead.appendChild(headerRow);
            table.appendChild(thead);
            
            // Body
            const tbody = document.createElement('tbody');
            arrayData.forEach((row, idx) => {
                const tr = document.createElement('tr');
                
                const tdIdx = document.createElement('td');
                tdIdx.className = 'index';
                tdIdx.textContent = idx;
                tr.appendChild(tdIdx);
                
                if (Array.isArray(row)) {
                    row.forEach(cell => {
                        const td = document.createElement('td');
                        td.textContent = typeof cell === 'number' && !Number.isInteger(cell)
                            ? cell.toFixed(3)
                            : cell;
                        tr.appendChild(td);
                    });
                    // Fill remaining cells if row is shorter
                    for (let i = row.length; i < maxCols; i++) {
                        const td = document.createElement('td');
                        td.textContent = '—';
                        td.style.color = 'var(--stimdg-text-muted)';
                        tr.appendChild(td);
                    }
                }
                
                tbody.appendChild(tr);
            });
            table.appendChild(tbody);
            container.appendChild(table);
        } else {
            // 1D array - list format
            const list = document.createElement('div');
            list.className = 'stimdg-array-list';
            
            arrayData.forEach((item, idx) => {
                const itemDiv = document.createElement('div');
                itemDiv.className = 'stimdg-array-list-item';
                
                const indexSpan = document.createElement('span');
                indexSpan.className = 'stimdg-array-list-index';
                indexSpan.textContent = `[${idx}]`;
                itemDiv.appendChild(indexSpan);
                
                const valueSpan = document.createElement('span');
                valueSpan.className = 'stimdg-array-list-value';
                valueSpan.textContent = typeof item === 'number' && !Number.isInteger(item)
                    ? item.toFixed(3)
                    : item;
                itemDiv.appendChild(valueSpan);
                
                list.appendChild(itemDiv);
            });
            
            container.appendChild(list);
        }
        
        return container;
    }
    
    /**
     * Export to CSV
     */
    exportCSV() {
        const headers = this.columns;
        const rows = [headers];
        
        this.processedRows.forEach(row => {
            const values = headers.map(header => {
                const value = row[header];
                if (Array.isArray(value)) {
                    return `"[Array:${value.length}]"`;
                }
                if (typeof value === 'string' && (value.includes(',') || value.includes('"'))) {
                    return `"${value.replace(/"/g, '""')}"`;
                }
                if (value === null || value === undefined) {
                    return '';
                }
                return value;
            });
            rows.push(values);
        });
        
        const csvContent = rows.map(row => row.join(',')).join('\n');
        const blob = new Blob([csvContent], { type: 'text/csv' });
        const url = window.URL.createObjectURL(blob);
        const link = document.createElement('a');
        link.href = url;
        link.download = `${this.name}_export.csv`;
        link.click();
        window.URL.revokeObjectURL(url);
    }
    
    /**
     * Get row by index
     */
    getRow(index) {
        return this.processedRows.find(row => row._row_index === index);
    }
    
    /**
     * Get all rows matching a filter
     */
    filterRows(predicate) {
        return this.processedRows.filter(predicate);
    }
}

// Export for use
if (typeof window !== 'undefined') {
    window.StimDGViewer = StimDGViewer;
}

if (typeof module !== 'undefined' && module.exports) {
    module.exports = StimDGViewer;
}
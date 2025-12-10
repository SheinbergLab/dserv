/**
 * DGTableViewer.js
 * Vanilla JavaScript table viewer for DYN_GROUP hybrid JSON format
 * 
 * Usage:
 *   const viewer = new DGTableViewer('container-id', data);
 *   viewer.render();
 */

class DGTableViewer {
  constructor(containerId, data, options = {}) {
    this.container = document.getElementById(containerId);
    if (!this.container) {
      throw new Error(`Container element with id "${containerId}" not found`);
    }
    
    this.data = data;
    this.options = {
      pageSize: options.pageSize || 50,
      maxHeight: options.maxHeight || '500px',
      showExport: options.showExport !== false,
      showRowCount: options.showRowCount !== false,
      compactMode: options.compactMode || false,
      theme: options.theme || 'light', // 'light' or 'dark'
      ...options
    };
    
    this.currentPage = 0;
    this.sortColumn = null;
    this.sortDirection = 'asc';
    this.selectedRowIndex = null;
    this.inspectorOpen = false;
    
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
      return;
    }
    
    // Handle hybrid format (rows + arrays + name)
    if (this.data.rows && this.data.arrays) {
      this.name = this.data.name || 'Data';
      this.arrayColumns = Object.keys(this.data.arrays);
      
      this.processedRows = this.data.rows.map((row, index) => {
        const enhancedRow = { _row_index: index, ...row };
        
        // Replace array indices with actual array data
        this.arrayColumns.forEach(fieldName => {
          if (fieldName in row && typeof row[fieldName] === 'number') {
            const arrayIndex = row[fieldName];
            const arrayData = this.data.arrays[fieldName][arrayIndex];
            enhancedRow[fieldName] = arrayData;
          }
        });
        
        return enhancedRow;
      });
    }
    // Handle direct rows array
    else if (Array.isArray(this.data)) {
      this.name = 'Data';
      this.arrayColumns = [];
      this.processedRows = this.data.map((row, index) => ({
        _row_index: index,
        ...row
      }));
    }
    // Fallback
    else {
      this.name = 'Data';
      this.arrayColumns = [];
      this.processedRows = [];
    }
    
    // Extract column names from first row
    if (this.processedRows.length > 0) {
      this.columns = Object.keys(this.processedRows[0]).filter(key => key !== '_row_index');
    } else {
      this.columns = [];
    }
  }
  
  /**
   * Main render function
   */
  render() {
    this.container.innerHTML = '';
    this.container.classList.add('dg-table-viewer');
    if (this.options.theme === 'dark') {
      this.container.classList.add('dg-dark');
      // Also add to body for modal/inspector which are appended to body
      document.body.classList.add('dg-dark-mode');
    }
    
    // Create header
    const header = this.createHeader();
    this.container.appendChild(header);
    
    // Create table
    const tableContainer = this.createTable();
    this.container.appendChild(tableContainer);
    
    // Create pagination
    if (this.processedRows.length > this.options.pageSize) {
      const pagination = this.createPagination();
      this.container.appendChild(pagination);
    }
    
    // Create modal container
    this.createModal();
    
    // Create inspector panel
    this.createInspector();
  }
  
  /**
   * Create header with info and buttons
   */
  createHeader() {
    const header = document.createElement('div');
    header.className = 'dg-header';
    
    const infoLeft = document.createElement('div');
    infoLeft.className = 'dg-info-left';
    
    const nameSpan = document.createElement('strong');
    nameSpan.textContent = this.name;
    infoLeft.appendChild(nameSpan);
    
    if (this.options.showRowCount) {
      const sep1 = document.createElement('span');
      sep1.className = 'dg-separator';
      sep1.textContent = '•';
      infoLeft.appendChild(sep1);
      
      const rowCount = document.createElement('span');
      rowCount.textContent = `${this.processedRows.length} rows`;
      infoLeft.appendChild(rowCount);
    }
    
    if (this.arrayColumns.length > 0) {
      const sep2 = document.createElement('span');
      sep2.className = 'dg-separator';
      sep2.textContent = '•';
      infoLeft.appendChild(sep2);
      
      const arrayInfo = document.createElement('span');
      arrayInfo.className = 'dg-array-info';
      arrayInfo.innerHTML = `<strong>Arrays:</strong> ${this.arrayColumns.join(', ')}`;
      infoLeft.appendChild(arrayInfo);
    }
    
    header.appendChild(infoLeft);
    
    // Right side buttons
    if (this.options.showExport) {
      const infoRight = document.createElement('div');
      infoRight.className = 'dg-info-right';
      
      const exportBtn = document.createElement('button');
      exportBtn.className = 'dg-button';
      exportBtn.textContent = '⬇ Export CSV';
      exportBtn.onclick = () => this.exportCSV();
      infoRight.appendChild(exportBtn);
      
      header.appendChild(infoRight);
    }
    
    return header;
  }
  
  /**
   * Create table with current page
   */
  createTable() {
    const container = document.createElement('div');
    container.className = 'dg-table-container';
    container.style.maxHeight = this.options.maxHeight;
    
    const table = document.createElement('table');
    table.className = 'dg-table';
    if (this.options.compactMode) {
      table.classList.add('dg-table-compact');
    }
    
    // Create header
    const thead = document.createElement('thead');
    const headerRow = document.createElement('tr');
    
    this.columns.forEach(col => {
      const th = document.createElement('th');
      th.textContent = col;
      
      // Add sort functionality for non-array columns
      const isArrayCol = this.arrayColumns.includes(col);
      if (!isArrayCol) {
        th.className = 'dg-sortable';
        if (this.sortColumn === col) {
          th.classList.add('dg-sorted');
          th.classList.add(this.sortDirection);
        }
        th.onclick = () => this.sortByColumn(col);
      }
      
      headerRow.appendChild(th);
    });
    
    thead.appendChild(headerRow);
    table.appendChild(thead);
    
    // Create body
    const tbody = document.createElement('tbody');
    const start = this.currentPage * this.options.pageSize;
    const end = Math.min(start + this.options.pageSize, this.processedRows.length);
    
    for (let i = start; i < end; i++) {
      const row = this.processedRows[i];
      const tr = document.createElement('tr');
      
      // Add click handler for row inspection
      tr.onclick = (e) => {
        // Don't trigger if clicking on array link
        if (e.target.classList.contains('dg-array-link')) {
          return;
        }
        this.showInspector(row, row._row_index);
      };
      
      this.columns.forEach(col => {
        const td = document.createElement('td');
        const value = row[col];
        
        if (Array.isArray(value)) {
          // Array cell - show as clickable link
          const link = document.createElement('span');
          link.className = 'dg-array-link';
          link.textContent = `array[${value.length}]`;
          link.onclick = () => this.showArrayModal(value, col, row._row_index);
          td.appendChild(link);
        } else if (typeof value === 'number' && !Number.isInteger(value)) {
          // Float - format to 3 decimals
          td.textContent = value.toFixed(3);
        } else if (value === null || value === undefined) {
          // Null/undefined
          td.textContent = '—';
          td.className = 'dg-null';
        } else {
          // Regular value
          td.textContent = value;
        }
        
        tr.appendChild(td);
      });
      
      tbody.appendChild(tr);
    }
    
    table.appendChild(tbody);
    container.appendChild(table);
    
    // Restore row highlighting if inspector is open
    if (this.inspectorOpen && this.selectedRowIndex !== null) {
      this.highlightRow(this.selectedRowIndex);
    }
    
    return container;
  }
  
  /**
   * Create pagination controls
   */
  createPagination() {
    const pagination = document.createElement('div');
    pagination.className = 'dg-pagination';
    
    const totalPages = Math.ceil(this.processedRows.length / this.options.pageSize);
    const start = this.currentPage * this.options.pageSize + 1;
    const end = Math.min((this.currentPage + 1) * this.options.pageSize, this.processedRows.length);
    
    // Previous button
    const prevBtn = document.createElement('button');
    prevBtn.className = 'dg-button';
    prevBtn.textContent = '← Prev';
    prevBtn.disabled = this.currentPage === 0;
    prevBtn.onclick = () => {
      this.currentPage--;
      this.render();
    };
    pagination.appendChild(prevBtn);
    
    // Page info
    const pageInfo = document.createElement('span');
    pageInfo.className = 'dg-page-info';
    pageInfo.textContent = `${start}-${end} of ${this.processedRows.length}`;
    pagination.appendChild(pageInfo);
    
    // Next button
    const nextBtn = document.createElement('button');
    nextBtn.className = 'dg-button';
    nextBtn.textContent = 'Next →';
    nextBtn.disabled = this.currentPage >= totalPages - 1;
    nextBtn.onclick = () => {
      this.currentPage++;
      this.render();
    };
    pagination.appendChild(nextBtn);
    
    return pagination;
  }
  
  /**
   * Sort table by column
   */
  sortByColumn(column) {
    if (this.sortColumn === column) {
      // Toggle direction
      this.sortDirection = this.sortDirection === 'asc' ? 'desc' : 'asc';
    } else {
      // New column
      this.sortColumn = column;
      this.sortDirection = 'asc';
    }
    
    this.processedRows.sort((a, b) => {
      let aVal = a[column];
      let bVal = b[column];
      
      // Handle nulls
      if (aVal === null || aVal === undefined) return 1;
      if (bVal === null || bVal === undefined) return -1;
      
      // Numeric comparison
      if (typeof aVal === 'number' && typeof bVal === 'number') {
        return this.sortDirection === 'asc' ? aVal - bVal : bVal - aVal;
      }
      
      // String comparison
      const result = String(aVal).localeCompare(String(bVal));
      return this.sortDirection === 'asc' ? result : -result;
    });
    
    this.currentPage = 0; // Reset to first page
    this.render();
  }
  
  /**
   * Create modal for array details
   */
  createModal() {
    // Remove existing modal if any
    const existing = document.getElementById('dg-modal');
    if (existing) existing.remove();
    
    const modal = document.createElement('div');
    modal.id = 'dg-modal';
    modal.className = 'dg-modal';
    modal.style.display = 'none';
    
    const modalContent = document.createElement('div');
    modalContent.className = 'dg-modal-content';
    
    const modalHeader = document.createElement('div');
    modalHeader.className = 'dg-modal-header';
    
    const modalTitle = document.createElement('h3');
    modalTitle.id = 'dg-modal-title';
    modalHeader.appendChild(modalTitle);
    
    const closeBtn = document.createElement('span');
    closeBtn.className = 'dg-modal-close';
    closeBtn.textContent = '×';
    closeBtn.onclick = () => this.hideArrayModal();
    modalHeader.appendChild(closeBtn);
    
    modalContent.appendChild(modalHeader);
    
    const modalBody = document.createElement('div');
    modalBody.className = 'dg-modal-body';
    modalBody.id = 'dg-modal-body';
    modalContent.appendChild(modalBody);
    
    modal.appendChild(modalContent);
    document.body.appendChild(modal);
    
    // Close on background click
    modal.onclick = (e) => {
      if (e.target === modal) {
        this.hideArrayModal();
      }
    };
  }
  
  /**
   * Show array in modal
   */
  showArrayModal(arrayData, columnName, rowIndex) {
    const modal = document.getElementById('dg-modal');
    const title = document.getElementById('dg-modal-title');
    const body = document.getElementById('dg-modal-body');
    
    title.textContent = `${columnName} - Row ${rowIndex}`;
    body.innerHTML = '';
    
    // Check if nested array (2D)
    const isNested = arrayData.length > 0 && Array.isArray(arrayData[0]);
    
    if (isNested) {
      // Create table for nested array
      const table = document.createElement('table');
      table.className = 'dg-table dg-modal-table';
      
      // Header
      const thead = document.createElement('thead');
      const headerRow = document.createElement('tr');
      const thRow = document.createElement('th');
      thRow.textContent = 'Row';
      headerRow.appendChild(thRow);
      
      const maxCols = Math.max(...arrayData.map(row => Array.isArray(row) ? row.length : 0));
      for (let i = 0; i < maxCols; i++) {
        const th = document.createElement('th');
        th.textContent = `Col ${i}`;
        headerRow.appendChild(th);
      }
      thead.appendChild(headerRow);
      table.appendChild(thead);
      
      // Body
      const tbody = document.createElement('tbody');
      arrayData.forEach((row, rowIdx) => {
        const tr = document.createElement('tr');
        
        const tdRow = document.createElement('td');
        tdRow.textContent = rowIdx;
        tdRow.className = 'dg-row-index';
        tr.appendChild(tdRow);
        
        if (Array.isArray(row)) {
          row.forEach(cell => {
            const td = document.createElement('td');
            td.textContent = typeof cell === 'number' && !Number.isInteger(cell) 
              ? cell.toFixed(3) 
              : cell;
            tr.appendChild(td);
          });
        }
        
        tbody.appendChild(tr);
      });
      table.appendChild(tbody);
      
      body.appendChild(table);
    } else {
      // Simple array - list format
      const list = document.createElement('div');
      list.className = 'dg-simple-array';
      
      arrayData.forEach((item, idx) => {
        const itemDiv = document.createElement('div');
        itemDiv.className = 'dg-array-item';
        
        const indexSpan = document.createElement('span');
        indexSpan.className = 'dg-array-index';
        indexSpan.textContent = `[${idx}]:`;
        itemDiv.appendChild(indexSpan);
        
        const valueSpan = document.createElement('span');
        valueSpan.className = 'dg-array-value';
        valueSpan.textContent = typeof item === 'number' && !Number.isInteger(item)
          ? item.toFixed(3)
          : item;
        itemDiv.appendChild(valueSpan);
        
        list.appendChild(itemDiv);
      });
      
      body.appendChild(list);
    }
    
    modal.style.display = 'flex';
  }
  
  /**
   * Hide array modal
   */
  hideArrayModal() {
    const modal = document.getElementById('dg-modal');
    if (modal) {
      modal.style.display = 'none';
    }
  }
  
  /**
   * Create inspector panel
   */
  createInspector() {
    // Remove existing inspector
    const existing = document.getElementById('dg-inspector');
    if (existing) existing.remove();
    
    const inspector = document.createElement('div');
    inspector.id = 'dg-inspector';
    inspector.className = 'dg-inspector-panel';
    
    // Header
    const header = document.createElement('div');
    header.className = 'dg-inspector-header';
    
    const title = document.createElement('h3');
    title.id = 'dg-inspector-title';
    title.textContent = 'Row Inspector';
    header.appendChild(title);
    
    const closeBtn = document.createElement('span');
    closeBtn.className = 'dg-inspector-close';
    closeBtn.textContent = '×';
    closeBtn.onclick = () => this.closeInspector();
    header.appendChild(closeBtn);
    
    inspector.appendChild(header);
    
    // Body
    const body = document.createElement('div');
    body.className = 'dg-inspector-body';
    body.id = 'dg-inspector-body';
    inspector.appendChild(body);
    
    document.body.appendChild(inspector);
  }
  
  /**
   * Show inspector for a row
   */
  showInspector(row, rowIndex) {
    const inspector = document.getElementById('dg-inspector');
    const title = document.getElementById('dg-inspector-title');
    const body = document.getElementById('dg-inspector-body');
    
    if (!inspector || !title || !body) return;
    
    this.selectedRowIndex = rowIndex;
    this.inspectorOpen = true;
    
    title.textContent = `Row ${rowIndex}`;
    body.innerHTML = '';
    
    // Build tree view
    const tree = this.buildTreeView(row);
    body.appendChild(tree);
    
    // Open panel
    inspector.classList.add('open');
    
    // Highlight row in table
    this.highlightRow(rowIndex);
  }
  
  /**
   * Close inspector
   */
  closeInspector() {
    const inspector = document.getElementById('dg-inspector');
    if (inspector) {
      inspector.classList.remove('open');
    }
    this.selectedRowIndex = null;
    this.inspectorOpen = false;
    this.highlightRow(null);
  }
  
  /**
   * Build tree view for row data
   */
  buildTreeView(row) {
    const container = document.createElement('div');
    
    this.columns.forEach(col => {
      const value = row[col];
      const item = document.createElement('div');
      item.className = 'dg-tree-item';
      
      if (Array.isArray(value)) {
        // Array field
        const isNested = value.length > 0 && Array.isArray(value[0]);
        
        const header = document.createElement('div');
        header.className = 'dg-tree-array-header';
        header.textContent = `${col}: array[${value.length}] ▼`;
        
        const content = document.createElement('div');
        content.className = 'dg-tree-array-content';
        
        if (isNested) {
          // 2D array - show as mini table
          const table = document.createElement('table');
          table.className = 'dg-table';
          table.style.fontSize = '11px';
          table.style.marginTop = '8px';
          
          const tbody = document.createElement('tbody');
          value.forEach((subArray, idx) => {
            const tr = document.createElement('tr');
            
            const tdIdx = document.createElement('td');
            tdIdx.textContent = `[${idx}]`;
            tdIdx.style.color = '#666';
            tdIdx.style.padding = '2px 6px';
            tr.appendChild(tdIdx);
            
            if (Array.isArray(subArray)) {
              subArray.forEach(cell => {
                const td = document.createElement('td');
                td.textContent = typeof cell === 'number' && !Number.isInteger(cell)
                  ? cell.toFixed(3)
                  : cell;
                td.style.padding = '2px 6px';
                tr.appendChild(td);
              });
            }
            
            tbody.appendChild(tr);
          });
          table.appendChild(tbody);
          content.appendChild(table);
        } else {
          // 1D array - show as list
          const arrayDiv = document.createElement('div');
          arrayDiv.className = 'dg-tree-array';
          value.forEach((val, idx) => {
            const valDiv = document.createElement('div');
            valDiv.innerHTML = `<span class="dg-tree-key">[${idx}]:</span> <span class="dg-tree-value">${typeof val === 'number' && !Number.isInteger(val) ? val.toFixed(3) : val}</span>`;
            arrayDiv.appendChild(valDiv);
          });
          content.appendChild(arrayDiv);
        }
        
        // Toggle collapse
        header.onclick = () => {
          content.classList.toggle('collapsed');
          header.textContent = content.classList.contains('collapsed')
            ? `${col}: array[${value.length}] ▶`
            : `${col}: array[${value.length}] ▼`;
        };
        
        item.appendChild(header);
        item.appendChild(content);
      } else {
        // Primitive field
        const displayValue = value === null || value === undefined
          ? '—'
          : (typeof value === 'number' && !Number.isInteger(value) ? value.toFixed(3) : value);
        
        item.innerHTML = `<span class="dg-tree-key">${col}:</span> <span class="dg-tree-value">${displayValue}</span>`;
      }
      
      container.appendChild(item);
    });
    
    return container;
  }
  
  /**
   * Highlight row in table
   */
  highlightRow(rowIndex) {
    const tbody = this.container.querySelector('tbody');
    if (!tbody) return;
    
    const rows = tbody.querySelectorAll('tr');
    rows.forEach(tr => {
      tr.classList.remove('dg-selected');
    });
    
    if (rowIndex !== null) {
      rows.forEach(tr => {
        // Check if this row matches the selected index
        // Need to find row by clicking and checking _row_index
        const cells = tr.querySelectorAll('td');
        if (cells.length > 0) {
          // Get the actual row data to check _row_index
          const start = this.currentPage * this.options.pageSize;
          const trIndex = Array.from(rows).indexOf(tr);
          const dataIndex = start + trIndex;
          
          if (dataIndex < this.processedRows.length && 
              this.processedRows[dataIndex]._row_index === rowIndex) {
            tr.classList.add('dg-selected');
          }
        }
      });
    }
  }
  
  /**
   * Export data as CSV
   */
  exportCSV() {
    const headers = this.columns;
    const rows = [headers];
    
    this.processedRows.forEach(row => {
      const values = headers.map(header => {
        const value = row[header];
        if (Array.isArray(value)) {
          return `"[Array of ${value.length}]"`;
        }
        if (typeof value === 'object' && value !== null) {
          return `"${JSON.stringify(value)}"`;
        }
        if (typeof value === 'string' && value.includes(',')) {
          return `"${value}"`;
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
}

// Make it available globally
if (typeof window !== 'undefined') {
  window.DGTableViewer = DGTableViewer;
}

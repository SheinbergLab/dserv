/**
 * data_manager.js
 * Simplified Data Manager - browse, stage, download/copy
 * 
 * Terminology:
 *   - obs: observation-period oriented data (sync-line bounded epochs)
 *   - trials: analysis-ready rectangular data (extracted trials)
 *   - export: copy files to destination
 */

// ============================================================================
// Global State
// ============================================================================

let connection = null;
let dpManager = null;

// File data
let allFiles = [];
let filteredFiles = [];
let selectedFiles = new Set();

// Filter options
let filterOptions = {
    subjects: [],
    systems: [],
    protocols: []
};

// Destination paths (configured via datapoint)
let exportDestination = '';
let exportEnabled = false;

// Preview
let previewViewer = null;

// UI state
let downloadMenuOpen = false;
let exportMenuOpen = false;

// Console
let errorCount = 0;

// ============================================================================
// Initialization
// ============================================================================

async function init() {
    log('Initializing Data Manager...', 'info');
    
    connection = new DservConnection({
        subprocess: 'df',
        autoReconnect: true,
        connectTimeout: 10000,
        onStatus: handleConnectionStatus,
        onError: handleConnectionError
    });
    
    dpManager = new DatapointManager(connection, {
        autoGetKeys: false
    });
    
    window.dpManager = dpManager;
    
    setupEventListeners();
    
    try {
        await connection.connect();
        log('Connected to dserv', 'info');
        
        await loadExportConfig();
        await loadFilterOptions();
        await loadStats();
        await loadFiles();
        
    } catch (e) {
        log(`Connection failed: ${e.message}`, 'error');
    }
    
    connection.on('connected', async () => {
        log('Reconnected - refreshing data...', 'info');
        await loadExportConfig();
        await loadFilterOptions();
        await loadStats();
        await loadFiles();
    });
}

function setupEventListeners() {
    // Filter changes
    document.getElementById('filter-subject').addEventListener('change', applyFilters);
    document.getElementById('filter-system').addEventListener('change', applyFilters);
    document.getElementById('filter-protocol').addEventListener('change', applyFilters);
    document.getElementById('filter-date').addEventListener('change', applyFilters);
    document.getElementById('filter-min-obs').addEventListener('input', debounce(applyFilters, 300));
    document.getElementById('filter-search').addEventListener('input', debounce(applyFilters, 300));
    
    // Close menus when clicking outside
    document.addEventListener('click', (e) => {
        if (!e.target.closest('#download-dropdown')) {
            closeDownloadMenu();
        }
        if (!e.target.closest('#export-dropdown')) {
            closeExportMenu();
        }
    });
}

// ============================================================================
// Data Loading
// ============================================================================

async function loadFilterOptions() {
    try {
        const result = await sendCommand('get_filter_options');
        filterOptions = JSON.parse(result);
        
        populateSelect('filter-subject', filterOptions.subjects);
        populateSelect('filter-system', filterOptions.systems);
        populateSelect('filter-protocol', filterOptions.protocols);
        
    } catch (e) {
        log(`Failed to load filter options: ${e.message}`, 'error');
    }
}

async function loadStats() {
    try {
        const result = await sendCommand('get_stats');
        const stats = JSON.parse(result);
        
        const totalObs = stats.total_obs || 0;
        const totalTrials = stats.total_trials || 0;
        document.getElementById('data-summary').textContent = 
            `${stats.total_files} files, ${totalObs.toLocaleString()} obs, ${totalTrials.toLocaleString()} trials`;
        
    } catch (e) {
        log(`Failed to load stats: ${e.message}`, 'error');
    }
}

async function loadFiles() {
    try {
        const filters = buildFilterDict();
        const filterStr = dictToTcl(filters);
        
        const result = await sendCommand(`list_datafiles {${filterStr}}`);
        allFiles = JSON.parse(result);
        filteredFiles = allFiles;
        
        renderFileTable();
        updateFileCount();
        
    } catch (e) {
        log(`Failed to load files: ${e.message}`, 'error');
    }
}

async function loadExportConfig() {
    try {
        const result = await sendCommand('get_export_config');
        const config = JSON.parse(result);
        
        exportDestination = config.destination || '';
        exportEnabled = config.enabled && exportDestination !== '';
        
        // Update UI
        const destDisplay = document.getElementById('export-destination-display');
        const exportBtn = document.getElementById('export-btn');
        
        if (exportEnabled) {
            destDisplay.textContent = exportDestination;
            destDisplay.classList.remove('not-configured');
            exportBtn.title = `Export to ${exportDestination}`;
        } else {
            destDisplay.textContent = 'Not configured';
            destDisplay.classList.add('not-configured');
            exportBtn.title = 'Configure df/export_destination to enable';
        }
        
        log(`Export config: ${exportEnabled ? exportDestination : 'not configured'}`, 'info');
        
    } catch (e) {
        log(`Failed to load export config: ${e.message}`, 'error');
        exportEnabled = false;
    }
}

// ============================================================================
// Filtering
// ============================================================================

function buildFilterDict() {
    const filters = {};
    
    const subject = document.getElementById('filter-subject').value;
    const system = document.getElementById('filter-system').value;
    const protocol = document.getElementById('filter-protocol').value;
    const dateFilter = document.getElementById('filter-date').value;
    const minObs = document.getElementById('filter-min-obs').value;
    
    if (subject) filters.subject = subject;
    if (system) filters.system = system;
    if (protocol) filters.protocol = protocol;
    if (minObs) filters.min_obs = parseInt(minObs);
    
    if (dateFilter) {
        const now = Math.floor(Date.now() / 1000);
        const day = 86400;
        
        switch (dateFilter) {
            case 'today':
                const todayStart = new Date();
                todayStart.setHours(0, 0, 0, 0);
                filters.after = Math.floor(todayStart.getTime() / 1000);
                break;
            case 'week':
                filters.after = now - (7 * day);
                break;
            case 'month':
                filters.after = now - (30 * day);
                break;
        }
    }
    
    return filters;
}

function applyFilters() {
    loadFiles();
}

function clearFilters() {
    document.getElementById('filter-subject').value = '';
    document.getElementById('filter-system').value = '';
    document.getElementById('filter-protocol').value = '';
    document.getElementById('filter-date').value = '';
    document.getElementById('filter-min-obs').value = '';
    document.getElementById('filter-search').value = '';
    
    loadFiles();
}

function filterBySearch(files) {
    const search = document.getElementById('filter-search').value.toLowerCase();
    if (!search) return files;
    
    return files.filter(f => 
        f.filename.toLowerCase().includes(search) ||
        (f.subject && f.subject.toLowerCase().includes(search))
    );
}

// ============================================================================
// File Table
// ============================================================================

function renderFileTable() {
    const tbody = document.getElementById('file-table-body');
    const displayFiles = filterBySearch(filteredFiles);
    
    tbody.innerHTML = displayFiles.map(file => {
        const isSelected = selectedFiles.has(file.filename);
        const statusClass = getStatusClass(file.status);
        const displayName = file.filename.replace(/\.ess$/, '');
        return `
            <tr class="${isSelected ? 'selected' : ''}" 
                data-filename="${escapeHtml(file.filename)}"
                onclick="handleRowClick(event, '${escapeJs(file.filename)}')">
                <td class="dm-col-check">
                    <input type="checkbox" 
                           ${isSelected ? 'checked' : ''} 
                           onclick="event.stopPropagation(); toggleFileSelection('${escapeJs(file.filename)}')">
                </td>
                <td class="dm-col-filename" title="${escapeHtml(file.filename)}">${escapeHtml(displayName)}</td>
                <td class="dm-col-subject">${escapeHtml(file.subject || '')}</td>
                <td class="dm-col-system">${escapeHtml(file.system || '')}</td>
                <td class="dm-col-protocol">${escapeHtml(file.protocol || '')}</td>
                <td class="dm-col-obs">${file.n_obs || 0}</td>
                <td class="dm-col-trials">${file.n_trials || 0}</td>
                <td class="dm-col-status ${statusClass}">${escapeHtml(file.status || '')}</td>
                <td class="dm-col-date">${escapeHtml(file.date || '')}</td>
                <td class="dm-col-time">${escapeHtml(file.time || '')}</td>
                <td class="dm-col-actions">
                    <button class="dm-preview-btn" onclick="event.stopPropagation(); previewFile('${escapeJs(file.filename)}')" title="Preview">👁</button>
                </td>
            </tr>
        `;
    }).join('');
    
    updateSelectAllCheckbox();
}

function getStatusClass(status) {
    switch (status) {
        case 'ok': return 'status-ok';
        case 'error': return 'status-error';
        case 'obs_only': return 'status-warning';
        case 'processing': return 'status-working';
        default: return '';
    }
}

function updateFileCount() {
    const displayFiles = filterBySearch(filteredFiles);
    document.getElementById('file-count').textContent = `${displayFiles.length} files`;
}

// ============================================================================
// Selection (Staging)
// ============================================================================

function handleRowClick(event, filename) {
    if (event.target.type === 'checkbox') return;
    toggleFileSelection(filename);
}

function toggleFileSelection(filename) {
    if (selectedFiles.has(filename)) {
        selectedFiles.delete(filename);
    } else {
        selectedFiles.add(filename);
    }
    updateSelectionUI();
}

function selectAll() {
    const displayFiles = filterBySearch(filteredFiles);
    displayFiles.forEach(f => selectedFiles.add(f.filename));
    updateSelectionUI();
}

function selectNone() {
    selectedFiles.clear();
    updateSelectionUI();
}

function toggleSelectAll(checkbox) {
    if (checkbox.checked) {
        selectAll();
    } else {
        selectNone();
    }
}

function updateSelectAllCheckbox() {
    const displayFiles = filterBySearch(filteredFiles);
    const checkbox = document.getElementById('select-all-check');
    
    if (displayFiles.length === 0) {
        checkbox.checked = false;
        checkbox.indeterminate = false;
    } else if (selectedFiles.size === 0) {
        checkbox.checked = false;
        checkbox.indeterminate = false;
    } else if (displayFiles.every(f => selectedFiles.has(f.filename))) {
        checkbox.checked = true;
        checkbox.indeterminate = false;
    } else {
        checkbox.checked = false;
        checkbox.indeterminate = true;
    }
}

function updateSelectionUI() {
    // Update table rows
    document.querySelectorAll('#file-table-body tr').forEach(row => {
        const filename = row.dataset.filename;
        const checkbox = row.querySelector('input[type="checkbox"]');
        if (selectedFiles.has(filename)) {
            row.classList.add('selected');
            if (checkbox) checkbox.checked = true;
        } else {
            row.classList.remove('selected');
            if (checkbox) checkbox.checked = false;
        }
    });
    
    // Update staging info
    const count = selectedFiles.size;
    let totalObs = 0;
    let totalTrials = 0;
    selectedFiles.forEach(filename => {
        const file = allFiles.find(f => f.filename === filename);
        if (file) {
            totalObs += file.n_obs || 0;
            totalTrials += file.n_trials || 0;
        }
    });
    
    document.getElementById('staged-count').textContent = `${count} file${count !== 1 ? 's' : ''}`;
    document.getElementById('staged-obs').textContent = `(${totalObs.toLocaleString()} obs, ${totalTrials.toLocaleString()} trials)`;
    
    // Enable/disable buttons
    document.getElementById('download-btn').disabled = count === 0;
    document.getElementById('export-btn').disabled = count === 0 || !exportEnabled;
    
    updateSelectAllCheckbox();
}

// ============================================================================
// Download Menu (browser-based)
// ============================================================================

function toggleDownloadMenu() {
    downloadMenuOpen = !downloadMenuOpen;
    document.getElementById('download-menu').classList.toggle('open', downloadMenuOpen);
    if (downloadMenuOpen) closeExportMenu();
}

function closeDownloadMenu() {
    downloadMenuOpen = false;
    document.getElementById('download-menu').classList.remove('open');
}

function getSelectedLevel() {
    // Default to trials for downloads
    return 'trials';
}

async function downloadAsZip() {
    closeDownloadMenu();
    
    if (selectedFiles.size === 0) return;
    
    const level = getSelectedLevel();
    const filenames = Array.from(selectedFiles);
    const filenameList = filenames.map(f => `{${f}}`).join(' ');
    
    const now = new Date();
    const bundleName = `export_${now.toISOString().slice(0,10)}`;
    
    showStatus(`Creating zip with ${filenames.length} files...`, 'working');
    log(`Creating zip bundle...`, 'info');
    
    try {
        const result = await sendCommand(`export_bundle {${filenameList}} ${level} ${bundleName}`);
        const response = JSON.parse(result);
        
        if (response.status === 'ok') {
            showStatus(`Created ${response.bundle} - downloading...`, 'success');
            log(`Bundle created: ${response.bundle} (${formatBytes(response.size)})`, 'success');
            
            // Trigger download
            const a = document.createElement('a');
            a.href = response.url;
            a.download = response.bundle;
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
        } else {
            showStatus(`Error: ${response.error}`, 'error');
            log(`Bundle failed: ${response.error}`, 'error');
        }
    } catch (e) {
        showStatus(`Error: ${e.message}`, 'error');
        log(`Bundle failed: ${e.message}`, 'error');
    }
}

// ============================================================================
// Export Menu (server-side to configured destination)
// ============================================================================

function toggleExportMenu() {
    if (!exportEnabled) {
        showStatus('Export not configured. Set df/export_destination datapoint.', 'error');
        return;
    }
    exportMenuOpen = !exportMenuOpen;
    document.getElementById('export-menu').classList.toggle('open', exportMenuOpen);
    if (exportMenuOpen) closeDownloadMenu();
}

function closeExportMenu() {
    exportMenuOpen = false;
    document.getElementById('export-menu').classList.remove('open');
}

async function exportFiles(mode) {
    closeExportMenu();
    
    if (selectedFiles.size === 0) return;
    if (!exportEnabled || !exportDestination) {
        showStatus('Export destination not configured', 'error');
        return;
    }
    
    const level = getSelectedLevel();
    const filenames = Array.from(selectedFiles);
    const filenameList = filenames.map(f => `{${f}}`).join(' ');
    
    let modeDesc;
    switch (mode) {
        case 'ess': modeDesc = '.ess files'; break;
        case 'dgz': modeDesc = '.dgz files'; break;
        default: modeDesc = '.ess + .dgz files'; mode = 'both';
    }
    
    showStatus(`Exporting ${filenames.length} ${modeDesc}...`, 'working');
    log(`Exporting to ${exportDestination} (${mode})...`, 'info');
    
    try {
        const result = await sendCommand(`export_files {${filenameList}} ${level} ${mode}`);
        const response = JSON.parse(result);
        
        if (response.status === 'ok') {
            const counts = [];
            if (response.ess_copied) counts.push(`${response.ess_copied} .ess`);
            if (response.obs_copied) counts.push(`${response.obs_copied} .obs.dgz`);
            if (response.trials_copied) counts.push(`${response.trials_copied} .trials.dgz`);
            
            showStatus(`Exported ${response.success} of ${response.total} files`, 'success');
            log(`Export complete: ${counts.join(', ') || 'no files'}`, 'success');
            
            // Refresh file list to show updated export status
            await loadFiles();
        } else {
            showStatus(`Error: ${response.error}`, 'error');
            log(`Export failed: ${response.error}`, 'error');
        }
    } catch (e) {
        showStatus(`Error: ${e.message}`, 'error');
        log(`Export failed: ${e.message}`, 'error');
    }
}

// ============================================================================
// Status Message
// ============================================================================

function showStatus(message, type = 'info') {
    const el = document.getElementById('status-message');
    el.textContent = message;
    el.className = 'dm-status-message ' + type;
    
    // Clear after delay (except for working status)
    if (type !== 'working') {
        setTimeout(() => {
            el.textContent = '';
            el.className = 'dm-status-message';
        }, 5000);
    }
}

// ============================================================================
// Preview Modal
// ============================================================================

async function previewFile(filename) {
    log(`Loading preview for ${filename}...`, 'info');
    
    const modal = document.getElementById('preview-modal');
    const container = document.getElementById('preview-table-container');
    
    document.getElementById('preview-title').textContent = `Preview: ${filename}`;
    container.innerHTML = '<div style="padding: 40px; text-align: center; color: #8b949e;">Loading...</div>';
    modal.classList.add('open');
    
    try {
        const infoResult = await sendCommand(`get_datafile_info {${filename}}`);
        const info = JSON.parse(infoResult);
        
        document.getElementById('preview-info').innerHTML = `
            <span><strong>Subject:</strong> ${escapeHtml(info.subject || '--')}</span>
            <span><strong>System:</strong> ${escapeHtml(info.system || '--')}</span>
            <span><strong>Protocol:</strong> ${escapeHtml(info.protocol || '--')}</span>
            <span><strong>Obs:</strong> ${info.n_obs || 0}</span>
            <span><strong>Trials:</strong> ${info.n_trials || 0}</span>
            <span><strong>Status:</strong> ${escapeHtml(info.status || '--')}</span>
            <span><strong>Size:</strong> ${formatBytes(info.file_size || 0)}</span>
        `;
        
        const level = getSelectedLevel();
        const previewResult = await sendCommand(`preview_datafile {${filename}} ${level} 100`);
        
        let data;
        try {
            data = JSON.parse(previewResult);
        } catch (e) {
            throw new Error(`Invalid preview data`);
        }
        
        container.innerHTML = '';
        container.id = 'preview-table-container';
        
        previewViewer = new DGTableViewer('preview-table-container', data, {
            pageSize: 50,
            maxHeight: '100%',
            showExport: true,
            compactMode: true
        });
        previewViewer.render();
        
        log(`Preview loaded (${data.rows?.length || 0} rows)`, 'info');
        
    } catch (e) {
        log(`Preview failed: ${e.message}`, 'error');
        container.innerHTML = `<div style="padding: 40px; text-align: center; color: #f85149;">Error: ${escapeHtml(e.message)}</div>`;
    }
}

function closePreview() {
    document.getElementById('preview-modal').classList.remove('open');
}

// ============================================================================
// Scan
// ============================================================================

async function scanDatafiles() {
    const btn = document.querySelector('.dm-scan-btn');
    btn.textContent = '⟳ Scanning...';
    btn.disabled = true;
    
    log('Scanning for datafiles...', 'info');
    
    try {
        await sendCommand('rescan');
        log('Scan complete', 'success');
        
        await loadFilterOptions();
        await loadStats();
        await loadFiles();
        
    } catch (e) {
        log(`Scan failed: ${e.message}`, 'error');
    }
    
    btn.textContent = '⟳ Scan';
    btn.disabled = false;
}

// ============================================================================
// Connection
// ============================================================================

function handleConnectionStatus(status, message) {
    const indicator = document.getElementById('status-indicator');
    const statusText = document.getElementById('status-text');
    
    indicator.className = 'dm-status-indicator';
    
    switch (status) {
        case 'connected':
            indicator.classList.add('connected');
            statusText.textContent = 'Connected';
            break;
        case 'connecting':
            indicator.classList.add('connecting');
            statusText.textContent = message || 'Connecting...';
            break;
        case 'disconnected':
            statusText.textContent = message || 'Disconnected';
            break;
    }
}

function handleConnectionError(error) {
    log(`Connection error: ${error}`, 'error');
}

async function sendCommand(cmd) {
    if (!connection || !connection.isReady()) {
        throw new Error('Not connected');
    }
    
    return new Promise((resolve, reject) => {
        const wrappedCmd = `send df {${cmd}}`;
        const message = { cmd: 'eval', script: wrappedCmd };
        
        const handler = (data) => {
            if (data.status === 'error') {
                reject(new Error(data.error || 'Unknown error'));
            } else {
                resolve(data.result || '');
            }
        };
        
        const unsubscribe = connection.on('terminal:response', (data) => {
            unsubscribe();
            handler(data);
        });
        
        connection.ws.send(JSON.stringify(message));
        
        setTimeout(() => {
            unsubscribe();
            reject(new Error('Command timeout'));
        }, 60000);  // Longer timeout for file operations
    });
}

// ============================================================================
// Console
// ============================================================================

function log(message, level = 'info') {
    const timestamp = new Date().toLocaleTimeString();
    
    if (level === 'error') {
        errorCount++;
        updateErrorCount();
    }
    
    const consoleBody = document.getElementById('console-output');
    if (consoleBody) {
        const div = document.createElement('div');
        div.className = `dm-console-entry ${level}`;
        div.innerHTML = `<span class="timestamp">${timestamp}</span>${escapeHtml(message)}`;
        consoleBody.appendChild(div);
        consoleBody.scrollTop = consoleBody.scrollHeight;
    }
    
    if (level === 'error') {
        console.error(`[DM] ${message}`);
    } else {
        console.log(`[DM] ${message}`);
    }
}

function updateErrorCount() {
    const el = document.getElementById('error-count');
    if (el) {
        el.textContent = errorCount > 0 ? `${errorCount} errors` : '';
    }
}

function clearConsole() {
    errorCount = 0;
    updateErrorCount();
    document.getElementById('console-output').innerHTML = '';
}

function toggleConsole() {
    const panel = document.getElementById('console-panel');
    const icon = document.getElementById('console-collapse-icon');
    panel.classList.toggle('collapsed');
    icon.textContent = panel.classList.contains('collapsed') ? '▶' : '▼';
}

// ============================================================================
// Utilities
// ============================================================================

function populateSelect(id, options) {
    const select = document.getElementById(id);
    const currentValue = select.value;
    
    select.innerHTML = '<option value="">All</option>';
    
    options.forEach(opt => {
        const option = document.createElement('option');
        option.value = opt;
        option.textContent = opt;
        select.appendChild(option);
    });
    
    if (options.includes(currentValue)) {
        select.value = currentValue;
    }
}

function dictToTcl(obj) {
    return Object.entries(obj)
        .map(([k, v]) => `${k} {${v}}`)
        .join(' ');
}

function escapeHtml(text) {
    if (!text) return '';
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

function escapeJs(text) {
    if (!text) return '';
    return text.replace(/\\/g, '\\\\').replace(/'/g, "\\'");
}

function formatBytes(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
}

function debounce(func, wait) {
    let timeout;
    return function(...args) {
        clearTimeout(timeout);
        timeout = setTimeout(() => func(...args), wait);
    };
}

// ============================================================================
// Initialize
// ============================================================================

document.addEventListener('DOMContentLoaded', init);

// Global exports for HTML handlers
window.scanDatafiles = scanDatafiles;
window.clearFilters = clearFilters;
window.selectAll = selectAll;
window.selectNone = selectNone;
window.toggleSelectAll = toggleSelectAll;
window.handleRowClick = handleRowClick;
window.toggleFileSelection = toggleFileSelection;
window.toggleDownloadMenu = toggleDownloadMenu;
window.downloadAsZip = downloadAsZip;
window.toggleExportMenu = toggleExportMenu;
window.exportFiles = exportFiles;
window.previewFile = previewFile;
window.closePreview = closePreview;
window.clearConsole = clearConsole;
window.toggleConsole = toggleConsole;

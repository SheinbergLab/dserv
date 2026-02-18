/**
 * ESS Workbench - Libs Integration
 *
 * Adds shared Tcl module (.tm) library management to the Scripts tab:
 *   - Lists local lib/*.tm files in the scripts sidebar
 *   - Loads lib content into the TclEditor
 *   - Save (local), Promote (overlay→base), Commit (base→registry)
 *   - Pull lib from registry
 *   - Sync status indicators per lib
 *
 * Depends on: ess_workbench.js, ess_registry_integration.js,
 *             ess_registry_integration_v2.js, ess_overlay_integration.js
 *
 * Backend commands used:
 *   ess::list_libs                    — returns list of {name version filename}
 *   ess::read_lib <filename>          — returns file content
 *   ess::save_lib <filename> {content} — saves to overlay or base
 *   ess::commit_lib <filename> {comment} — pushes base→registry
 *   ess::sync_libs                    — pulls all libs from registry
 */

(function() {
    'use strict';

    // ========================================================================
    // Patch init to add libs state
    // ========================================================================

    const _origInit = ESSWorkbench.prototype.init;
    ESSWorkbench.prototype.init = function() {
        // Libs state
        this.libsState = {
            libs: [],           // [{name, version, filename, checksum}, ...]
            currentLib: null,   // filename of currently selected lib
            modified: false,
            originalHash: null,
            syncStatus: {}      // {filename: 'synced'|'modified'|'outdated'|'unknown'}
        };

        _origInit.call(this);
        this.initLibsUI();
    };

    // ========================================================================
    // UI Initialization
    // ========================================================================

    ESSWorkbench.prototype.initLibsUI = function() {
        this.bindLibsEvents();
    };

    ESSWorkbench.prototype.bindLibsEvents = function() {
        // Delegate clicks on lib items in the sidebar
        const libsList = document.getElementById('libs-list');
        if (libsList) {
            libsList.addEventListener('click', (e) => {
                const item = e.target.closest('.script-item');
                if (item && item.dataset.libFilename) {
                    this.selectLib(item.dataset.libFilename);
                }
            });
        }
    };

    // ========================================================================
    // Load Libs List from Backend
    // ========================================================================

    /**
     * Fetch the list of .tm files from the local lib directory.
     * Called after snapshot updates (system_path available).
     */
    ESSWorkbench.prototype.loadLibsList = async function() {
        try {
            const result = await this.execRegistryCmd('ess::list_libs');

            // Parse result — could be Tcl list of dicts or JSON
            let libs = [];
            if (typeof result === 'string') {
                libs = this.parseLibsList(result);
            } else if (Array.isArray(result)) {
                libs = result;
            }

            this.libsState.libs = libs;
            this.renderLibsSidebar();

            // Check sync status for libs
            this.checkLibsSyncStatus();

        } catch (err) {
            console.warn('Could not load libs list:', err.message);
            // Not fatal — system might not have libs
        }
    };

    /**
     * Parse a Tcl-formatted libs list.
     * Expected format: list of dicts like {name planko version 3.2 filename planko-3.2.tm}
     * or space-separated filename entries.
     */
    ESSWorkbench.prototype.parseLibsList = function(raw) {
        if (!raw || raw.trim() === '') return [];

        // Try JSON first
        try {
            const parsed = JSON.parse(raw);
            if (Array.isArray(parsed)) return parsed;
        } catch(e) { /* not JSON, try Tcl */ }

        // Simple case: space-separated filenames
        const libs = [];
        const parts = raw.trim().split(/\s+/);

        for (const part of parts) {
            if (part.endsWith('.tm')) {
                const match = part.match(/^(.+)-(\d+[\.\d]*)\.tm$/);
                if (match) {
                    libs.push({
                        name: match[1],
                        version: match[2],
                        filename: part
                    });
                } else {
                    libs.push({ name: part, version: '', filename: part });
                }
            }
        }

        return libs;
    };

    // ========================================================================
    // Render Libs in Sidebar
    // ========================================================================

    ESSWorkbench.prototype.renderLibsSidebar = function() {
        const container = document.getElementById('libs-list');
        const divider = document.getElementById('libs-divider');
        if (!container) return;

        const libs = this.libsState.libs;

        if (libs.length === 0) {
            container.innerHTML = '';
            if (divider) divider.style.display = 'none';
            return;
        }

        // Show divider
        if (divider) divider.style.display = '';

        // Sort by name
        const sorted = [...libs].sort((a, b) => a.filename.localeCompare(b.filename));

        container.innerHTML = sorted.map(lib => {
            const isActive = this.libsState.currentLib === lib.filename;
            const syncStatus = this.libsState.syncStatus[lib.filename] || '';

            return `
                <button class="script-item ${isActive ? 'active' : ''}"
                        data-lib-filename="${this.escapeHtml(lib.filename)}"
                        title="${this.escapeHtml(lib.filename)}">
                    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                        <path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"></path>
                        <path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"></path>
                    </svg>
                    <span>${this.escapeHtml(lib.name || lib.filename)}</span>
                    <span class="script-status-dot ${syncStatus}" data-lib="${this.escapeHtml(lib.filename)}"></span>
                </button>
            `;
        }).join('');
    };

    // ========================================================================
    // Select and Load a Lib
    // ========================================================================

    ESSWorkbench.prototype.selectLib = async function(filename) {
        // Deselect any system script
        this.currentScript = null;
        document.getElementById('scripts-list')?.querySelectorAll('.script-item').forEach(
            item => item.classList.remove('active')
        );

        // Select this lib
        this.libsState.currentLib = filename;

        // Update sidebar active state
        document.getElementById('libs-list')?.querySelectorAll('.script-item').forEach(item => {
            item.classList.toggle('active', item.dataset.libFilename === filename);
        });

        // Load content
        await this.loadLibContent(filename);

        // Update sync status display
        const status = this.libsState.syncStatus[filename] || 'unknown';
        this.updateSingleSyncStatus('scripts-sync-status', filename);

        // Update editor header
        const filenameEl = document.getElementById('editor-filename');
        if (filenameEl) filenameEl.textContent = `lib/${filename}`;
    };

    ESSWorkbench.prototype.loadLibContent = async function(filename) {
        if (!this.editor) return;

        try {
            // Escape filename for Tcl
            const safeFilename = filename.replace(/[{}\\]/g, '');
            const content = await this.execRegistryCmd(`ess::read_lib {${safeFilename}}`);

            const text = (typeof content === 'string') ? content : '';
            this.editor.setValue(text);

            // Track original hash for change detection
            this.libsState.originalHash = await sha256(text);
            this.libsState.modified = false;

            const saveBtn = document.getElementById('script-save-btn');
            if (saveBtn) saveBtn.disabled = true;

            this.elements.editorStatus.textContent = '';

        } catch (err) {
            console.error('Failed to load lib:', err);
            this.editor.setValue(`# Error loading ${filename}: ${err.message}`);
        }
    };

    // ========================================================================
    // Save Lib (local — to overlay or base)
    // ========================================================================

    ESSWorkbench.prototype.saveCurrentLib = async function() {
        const filename = this.libsState.currentLib;
        if (!filename || !this.editor?.view) return;

        const content = this.editor.getValue();
        const btn = document.getElementById('script-save-btn');
        const originalText = btn?.textContent;

        try {
            if (btn) {
                btn.disabled = true;
                btn.textContent = 'Saving...';
            }

            const safeFilename = filename.replace(/[{}\\]/g, '');
            await this.execRegistryCmd(`ess::save_lib {${safeFilename}} {${content}}`);

            // Update hash
            this.libsState.originalHash = await sha256(content);
            this.libsState.modified = false;

            // Mark local modification
            this.libsState.syncStatus[filename] = 'modified';
            this.updateLibSyncDots();

            if (btn) {
                btn.textContent = 'Saved!';
                setTimeout(() => {
                    btn.textContent = originalText;
                    btn.disabled = true; // Re-disable until next change
                }, 1500);
            }

            this.showNotification?.(`Saved lib/${filename}`, 'success');

        } catch (err) {
            console.error('Failed to save lib:', err);
            this.showNotification?.(`Save failed: ${err.message}`, 'error');
            if (btn) {
                btn.textContent = originalText;
                btn.disabled = false;
            }
        }
    };

    // ========================================================================
    // Commit Lib (base → registry)
    // ========================================================================

    ESSWorkbench.prototype.commitCurrentLib = async function(comment) {
        const filename = this.libsState.currentLib;
        if (!filename) return;

        try {
            const safeFilename = filename.replace(/[{}\\]/g, '');
            const safeComment = (comment || '').replace(/[{}]/g, '');
            await this.execRegistryCmd(`ess::commit_lib {${safeFilename}} {${safeComment}}`);

            this.libsState.syncStatus[filename] = 'synced';
            this.updateLibSyncDots();

            this.showNotification?.(`Committed lib/${filename} to registry`, 'success');

        } catch (err) {
            console.error('Commit lib failed:', err);
            if (err.message?.includes('viewer')) {
                this.showNotification?.('Permission denied: viewers cannot commit.', 'error');
            } else {
                this.showNotification?.(`Commit failed: ${err.message}`, 'error');
            }
            throw err;
        }
    };

    // ========================================================================
    // Pull Libs (registry → local)
    // ========================================================================

    ESSWorkbench.prototype.pullCurrentLib = async function() {
        if (!confirm('Pull latest libs from registry? This will overwrite local base files.')) {
            return;
        }

        try {
            const result = await this.execRegistryCmd('ess::sync_libs');

            const pulled = typeof result === 'object' ? (result.pulled || 0) : 0;
            const unchanged = typeof result === 'object' ? (result.unchanged || 0) : 0;

            if (pulled > 0) {
                this.showNotification?.(`Libs synced: ${pulled} updated, ${unchanged} unchanged`, 'success');
            } else {
                this.showNotification?.('Libs already up to date', 'info');
            }

            // Reload current lib if it was updated
            if (this.libsState.currentLib) {
                await this.loadLibContent(this.libsState.currentLib);
            }

            // Refresh sync status
            await this.checkLibsSyncStatus();

        } catch (err) {
            console.error('Lib pull failed:', err);
            this.showNotification?.(`Pull failed: ${err.message}`, 'error');
        }
    };

    // ========================================================================
    // Sync Status for Libs
    // ========================================================================

    ESSWorkbench.prototype.checkLibsSyncStatus = async function() {
        try {
            const result = await this.execRegistryCmd('ess::lib_sync_status');

            if (typeof result === 'string') {
                // Parse Tcl dict: "planko-3.2.tm synced essutil-1.0.tm modified"
                const parts = result.split(/\s+/);
                for (let i = 0; i < parts.length - 1; i += 2) {
                    this.libsState.syncStatus[parts[i]] = parts[i + 1];
                }
            } else if (typeof result === 'object') {
                Object.assign(this.libsState.syncStatus, result);
            }

            this.updateLibSyncDots();

        } catch (err) {
            // lib_sync_status might not exist yet — non-fatal
            console.warn('Could not check lib sync status:', err.message);
        }
    };

    ESSWorkbench.prototype.updateLibSyncDots = function() {
        document.getElementById('libs-list')?.querySelectorAll('.script-status-dot[data-lib]').forEach(dot => {
            const filename = dot.dataset.lib;
            const status = this.libsState.syncStatus[filename] || '';
            dot.className = 'script-status-dot ' + status;
        });
    };

    // ========================================================================
    // Hook into existing save/pull/commit buttons
    // ========================================================================

    // Override saveCurrentScript to handle libs
    const _origSaveCurrentScript = ESSWorkbench.prototype.saveCurrentScript;
    ESSWorkbench.prototype.saveCurrentScript = async function() {
        if (this.libsState.currentLib) {
            return this.saveCurrentLib();
        }
        return _origSaveCurrentScript.call(this);
    };

    // Override pullScriptFromRegistry to handle libs
    const _origPullScript = ESSWorkbench.prototype.pullScriptFromRegistry;
    ESSWorkbench.prototype.pullScriptFromRegistry = async function() {
        if (this.libsState.currentLib) {
            return this.pullCurrentLib();
        }
        return _origPullScript.call(this);
    };

    // Override commitToRegistry to handle libs
    const _origCommitToRegistry = ESSWorkbench.prototype.commitToRegistry;
    ESSWorkbench.prototype.commitToRegistry = async function(scriptType, comment) {
        if (this.libsState.currentLib) {
            try {
                await this.commitCurrentLib(comment);
                const modal = document.getElementById('commit-modal');
                if (modal) modal.style.display = 'none';
            } catch (err) {
                console.error('Lib commit failed:', err);
            }
            return;
        }
        return _origCommitToRegistry.call(this, scriptType, comment);
    };

    // Override showCommitDialog to show lib filename
    const _origShowCommitDialog = ESSWorkbench.prototype.showCommitDialog;
    ESSWorkbench.prototype.showCommitDialog = function(scriptType) {
        if (this.libsState.currentLib) {
            if (!this.currentUser) {
                alert('Please select a user first');
                return;
            }
            const modal = document.getElementById('commit-modal');
            if (!modal) return;

            const fileEl = document.getElementById('commit-file');
            if (fileEl) fileEl.textContent = `File: lib/${this.libsState.currentLib}`;

            modal.dataset.scriptType = '__lib__';
            document.getElementById('commit-comment').value = '';
            modal.style.display = 'flex';
            return;
        }
        return _origShowCommitDialog.call(this, scriptType);
    };

    // Override selectScript to clear lib selection when picking a system script
    const _origSelectScript = ESSWorkbench.prototype.selectScript;
    ESSWorkbench.prototype.selectScript = async function(scriptName) {
        // Clear lib selection
        this.libsState.currentLib = null;
        document.getElementById('libs-list')?.querySelectorAll('.script-item').forEach(
            item => item.classList.remove('active')
        );

        return _origSelectScript.call(this, scriptName);
    };

    // ========================================================================
    // Hook into snapshot to refresh libs list
    // ========================================================================

    const _origHandleSnapshot = ESSWorkbench.prototype.handleSnapshot;
    ESSWorkbench.prototype.handleSnapshot = function(data) {
        _origHandleSnapshot.call(this, data);

        // Load libs list when we have a system path
        if (this.snapshot?.system_path) {
            this.loadLibsList();
        }
    };

    // ========================================================================
    // Hook into switchTab to update sync status when entering scripts tab
    // ========================================================================

    const _origSwitchTab = ESSWorkbench.prototype.switchTab;
    ESSWorkbench.prototype.switchTab = function(tabName) {
        _origSwitchTab.call(this, tabName);

        if (tabName === 'scripts' && this.libsState.libs.length > 0) {
            this.checkLibsSyncStatus();
        }
    };

    // ========================================================================
    // Hook into updateSingleSyncStatus to handle lib filenames
    // ========================================================================

    const _origUpdateSingleSync = ESSWorkbench.prototype.updateSingleSyncStatus;
    ESSWorkbench.prototype.updateSingleSyncStatus = function(elementId, scriptType) {
        // If it's a lib filename, use lib sync status
        if (scriptType && scriptType.endsWith('.tm')) {
            const el = document.getElementById(elementId);
            if (!el) return;

            const status = this.libsState.syncStatus[scriptType] || 'unknown';
            el.className = 'sync-status ' + status;

            const text = el.querySelector('.sync-text');
            if (text) {
                const labels = {
                    'synced': 'Synced',
                    'modified': 'Modified',
                    'outdated': 'Update Available',
                    'conflict': 'Conflict',
                    'unknown': '—'
                };
                text.textContent = labels[status] || status;
            }
            return;
        }

        return _origUpdateSingleSync.call(this, elementId, scriptType);
    };

})();

/**
 * ESS Libs Plugin
 *
 * Adds shared Tcl module (.tm) library management to the Scripts tab.
 * Lists, loads, saves, commits, and pulls lib files.
 *
 * Backend commands:
 *   ess::list_libs, ess::read_lib, ess::save_lib,
 *   ess::commit_lib, ess::sync_libs, ess::lib_sync_status
 */

const LibsPlugin = {

    // ==========================================
    // Lifecycle Hooks
    // ==========================================

    onInit(wb) {
        wb.libsState = {
            libs: [],
            currentLib: null,
            modified: false,
            originalHash: null,
            syncStatus: {}
        };

        this._bindLibsEvents(wb);
    },

    onSnapshot(wb, snapshot) {
        if (snapshot?.system_path) {
            this._loadLibsList(wb);
        }
    },

    onTabSwitch(wb, tabName) {
        if (tabName === 'scripts' && wb.libsState.libs.length > 0) {
            this._checkLibsSyncStatus(wb);
        }
    },

    onScriptSelect(wb, scriptName) {
        // Clear lib selection when picking a system script
        wb.libsState.currentLib = null;
        document.getElementById('libs-list')?.querySelectorAll('.script-item').forEach(
            item => item.classList.remove('active')
        );
    },

    // Intercept save if a lib is selected
    async onSaveScript(wb, scriptType, content) {
        if (wb.libsState.currentLib) {
            await this._saveCurrentLib(wb);
            return false; // handled
        }
        return true; // let default handle it
    },

    // Intercept pull if a lib is selected
    async onPullScript(wb, scriptType) {
        if (wb.libsState.currentLib) {
            await this._pullCurrentLib(wb);
            return false;
        }
        return true;
    },

    // Intercept commit if a lib is selected
    async onCommitScript(wb, scriptType, comment) {
        if (wb.libsState.currentLib) {
            try {
                await this._commitCurrentLib(wb, comment);
                const modal = document.getElementById('commit-modal');
                if (modal) modal.style.display = 'none';
            } catch (err) {
                console.error('Lib commit failed:', err);
            }
            return false;
        }
        return true;
    },

    // Intercept commit dialog if a lib is selected
    onShowCommitDialog(wb, scriptType) {
        if (wb.libsState.currentLib) {
            if (!wb.currentUser) {
                alert('Please select a user first');
                return false;
            }
            const modal = document.getElementById('commit-modal');
            if (!modal) return false;

            const fileEl = document.getElementById('commit-file');
            if (fileEl) fileEl.textContent = `File: lib/${wb.libsState.currentLib}`;

            modal.dataset.scriptType = '__lib__';
            document.getElementById('commit-comment').value = '';
            modal.style.display = 'flex';
            return false;
        }
        return true;
    },

    // ==========================================
    // Event Bindings
    // ==========================================

    _bindLibsEvents(wb) {
        const libsList = document.getElementById('libs-list');
        if (libsList) {
            libsList.addEventListener('click', (e) => {
                const item = e.target.closest('.script-item');
                if (item && item.dataset.libFilename) {
                    this._selectLib(wb, item.dataset.libFilename);
                }
            });
        }
    },

    // ==========================================
    // Load Libs List
    // ==========================================

    async _loadLibsList(wb) {
        try {
            const result = await wb.execTclCmd('ess::list_libs');

            let libs = [];
            if (typeof result === 'string') {
                libs = this._parseLibsList(result);
            } else if (Array.isArray(result)) {
                libs = result;
            }

            wb.libsState.libs = libs;
            this._renderLibsSidebar(wb);
            this._checkLibsSyncStatus(wb);

        } catch (err) {
            console.warn('Could not load libs list:', err.message);
        }
    },

    _parseLibsList(raw) {
        if (!raw || raw.trim() === '') return [];

        try {
            const parsed = JSON.parse(raw);
            if (Array.isArray(parsed)) return parsed;
        } catch (e) { /* not JSON */ }

        const libs = [];
        const parts = raw.trim().split(/\s+/);

        for (const part of parts) {
            if (part.endsWith('.tm')) {
                const match = part.match(/^(.+)-(\d+[\.\d]*)\.tm$/);
                if (match) {
                    libs.push({ name: match[1], version: match[2], filename: part });
                } else {
                    libs.push({ name: part, version: '', filename: part });
                }
            }
        }

        return libs;
    },

    // ==========================================
    // Render Libs in Sidebar
    // ==========================================

    _renderLibsSidebar(wb) {
        const container = document.getElementById('libs-list');
        const divider = document.getElementById('libs-divider');
        if (!container) return;

        const libs = wb.libsState.libs;

        if (libs.length === 0) {
            container.innerHTML = '';
            if (divider) divider.style.display = 'none';
            return;
        }

        if (divider) divider.style.display = '';

        const sorted = [...libs].sort((a, b) => a.filename.localeCompare(b.filename));

        container.innerHTML = sorted.map(lib => {
            const isActive = wb.libsState.currentLib === lib.filename;
            const syncStatus = wb.libsState.syncStatus[lib.filename] || '';

            return `
                <button class="script-item ${isActive ? 'active' : ''}"
                        data-lib-filename="${wb.escapeHtml(lib.filename)}"
                        title="${wb.escapeHtml(lib.filename)}">
                    <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                        <path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"></path>
                        <path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"></path>
                    </svg>
                    <span>${wb.escapeHtml(lib.name || lib.filename)}</span>
                    <span class="script-status-dot ${syncStatus}" data-lib="${wb.escapeHtml(lib.filename)}"></span>
                </button>
            `;
        }).join('');
    },

    // ==========================================
    // Select and Load a Lib
    // ==========================================

    async _selectLib(wb, filename) {
        // Deselect any system script
        wb.currentScript = null;
        document.getElementById('scripts-list')?.querySelectorAll('.script-item').forEach(
            item => item.classList.remove('active')
        );

        wb.libsState.currentLib = filename;

        document.getElementById('libs-list')?.querySelectorAll('.script-item').forEach(item => {
            item.classList.toggle('active', item.dataset.libFilename === filename);
        });

        await this._loadLibContent(wb, filename);

        // Update sync status display
        const el = document.getElementById('scripts-sync-status');
        if (el) {
            const status = wb.libsState.syncStatus[filename] || 'unknown';
            el.className = 'sync-status ' + status;
            const text = el.querySelector('.sync-text');
            if (text) {
                const labels = {
                    'synced': 'Synced', 'modified': 'Modified',
                    'outdated': 'Update Available', 'conflict': 'Conflict', 'unknown': '—'
                };
                text.textContent = labels[status] || status;
            }
        }

        const filenameEl = document.getElementById('editor-filename');
        if (filenameEl) filenameEl.textContent = `lib/${filename}`;
    },

    async _loadLibContent(wb, filename) {
        if (!wb.editor) return;

        try {
            const safeFilename = filename.replace(/[{}\\]/g, '');
            const content = await wb.execTclCmd(`ess::read_lib {${safeFilename}}`);

            const text = (typeof content === 'string') ? content : '';
            wb.editor.setValue(text);

            wb.libsState.originalHash = await sha256(text);
            wb.libsState.modified = false;

            const saveBtn = document.getElementById('script-save-btn');
            if (saveBtn) saveBtn.disabled = true;

            wb.elements.editorStatus.textContent = '';

        } catch (err) {
            console.error('Failed to load lib:', err);
            wb.editor.setValue(`# Error loading ${filename}: ${err.message}`);
        }
    },

    // ==========================================
    // Save / Commit / Pull
    // ==========================================

    async _saveCurrentLib(wb) {
        const filename = wb.libsState.currentLib;
        if (!filename || !wb.editor?.view) return;

        const content = wb.editor.getValue();
        const btn = document.getElementById('script-save-btn');
        const originalText = btn?.textContent;

        try {
            if (btn) { btn.disabled = true; btn.textContent = 'Saving...'; }

            const safeFilename = filename.replace(/[{}\\]/g, '');
            await wb.execTclCmd(`ess::save_lib {${safeFilename}} {${content}}`);

            wb.libsState.originalHash = await sha256(content);
            wb.libsState.modified = false;
            wb.libsState.syncStatus[filename] = 'modified';
            this._updateLibSyncDots(wb);

            if (btn) {
                btn.textContent = 'Saved!';
                setTimeout(() => { btn.textContent = originalText; btn.disabled = true; }, 1500);
            }

            wb.showNotification(`Saved lib/${filename}`, 'success');
        } catch (err) {
            console.error('Failed to save lib:', err);
            wb.showNotification(`Save failed: ${err.message}`, 'error');
            if (btn) { btn.textContent = originalText; btn.disabled = false; }
        }
    },

    async _commitCurrentLib(wb, comment) {
        const filename = wb.libsState.currentLib;
        if (!filename) return;

        try {
            const safeFilename = filename.replace(/[{}\\]/g, '');
            const safeComment = (comment || '').replace(/[{}]/g, '');
            await wb.execTclCmd(`ess::commit_lib {${safeFilename}} {${safeComment}}`);

            wb.libsState.syncStatus[filename] = 'synced';
            this._updateLibSyncDots(wb);

            wb.showNotification(`Committed lib/${filename} to registry`, 'success');
        } catch (err) {
            console.error('Commit lib failed:', err);
            if (err.message?.includes('viewer')) {
                wb.showNotification('Permission denied: viewers cannot commit.', 'error');
            } else {
                wb.showNotification(`Commit failed: ${err.message}`, 'error');
            }
            throw err;
        }
    },

    async _pullCurrentLib(wb) {
        if (!confirm('Pull latest libs from registry? This will overwrite local base files.')) {
            return;
        }

        try {
            const result = await wb.execTclCmd('ess::sync_libs');
            const pulled = typeof result === 'object' ? (result.pulled || 0) : 0;
            const unchanged = typeof result === 'object' ? (result.unchanged || 0) : 0;

            if (pulled > 0) {
                wb.showNotification(`Libs synced: ${pulled} updated, ${unchanged} unchanged`, 'success');
            } else {
                wb.showNotification('Libs already up to date', 'info');
            }

            if (wb.libsState.currentLib) {
                await this._loadLibContent(wb, wb.libsState.currentLib);
            }

            await this._checkLibsSyncStatus(wb);
        } catch (err) {
            console.error('Lib pull failed:', err);
            wb.showNotification(`Pull failed: ${err.message}`, 'error');
        }
    },

    // ==========================================
    // Sync Status
    // ==========================================

    async _checkLibsSyncStatus(wb) {
        try {
            const result = await wb.execTclCmd('ess::lib_sync_status');

            if (typeof result === 'string') {
                const parts = result.split(/\s+/);
                for (let i = 0; i < parts.length - 1; i += 2) {
                    wb.libsState.syncStatus[parts[i]] = parts[i + 1];
                }
            } else if (typeof result === 'object') {
                Object.assign(wb.libsState.syncStatus, result);
            }

            this._updateLibSyncDots(wb);
        } catch (err) {
            console.warn('Could not check lib sync status:', err.message);
        }
    },

    _updateLibSyncDots(wb) {
        document.getElementById('libs-list')?.querySelectorAll('.script-status-dot[data-lib]').forEach(dot => {
            const filename = dot.dataset.lib;
            const status = wb.libsState.syncStatus[filename] || '';
            dot.className = 'script-status-dot ' + status;
        });
    }
};

ESSWorkbench.registerPlugin(LibsPlugin);

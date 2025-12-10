/**
 * command_reference.js - Command reference viewer application
 */

class CommandReference {
    constructor() {
        this.conn = null;
        this.commands = [];
        this.filteredCommands = [];
        this.selectedCommand = null;
        this.editor = null;
        this.namespaces = new Set();
        
        this.init();
    }
    
    async init() {
        // Set up event listeners
        document.getElementById('search-input').addEventListener('input', (e) => this.onSearch(e.target.value));
        document.getElementById('namespace-filter').addEventListener('change', (e) => this.onNamespaceFilter(e.target.value));
        
        // Keyboard navigation
        document.addEventListener('keydown', (e) => this.onKeyDown(e));
        
        // Connect and load
        await this.connect();
        await this.loadCommands();
    }
    
    async connect() {
        this.conn = new DservConnection({
            subprocess: 'docs',
	    createLinkedSubprocess: true,
            forceSecure: true,
            onStatus: (status, msg) => this.onConnectionStatus(status, msg),
            onError: (err) => console.error('Connection error:', err),
        });
        
        try {
            await this.conn.connect();

	    // Include some initialization
	    if (this.conn.getLinkedSubprocess()) {
		await this.conn.sendToLinked('package require dlsh');
	    }
	    
        } catch (e) {
            console.error('Failed to connect:', e);
            document.getElementById('list-loading').textContent = 'Connection failed. Is dserv running?';
        }
    }
    
    onConnectionStatus(status, msg) {
        console.log('Connection:', status, msg);
    }
    
    async loadCommands() {
        if (!this.conn || !this.conn.connected) {
            document.getElementById('list-loading').textContent = 'Not connected';
            return;
        }
        
        try {
            const response = await this.conn.send('api_commands_list');
            const data = JSON.parse(response);
            
            if (data.commands && data.commands.length > 0) {
                this.commands = data.commands;
                this.filteredCommands = [...this.commands];
                
                // Extract namespaces
                this.namespaces.clear();
                this.commands.forEach(cmd => {
                    if (cmd.namespace) this.namespaces.add(cmd.namespace);
                });
                
                this.populateNamespaceFilter();
                this.renderCommandList();
                this.updateResultCount();
            } else {
                document.getElementById('list-loading').textContent = 'No commands found';
            }
        } catch (e) {
            console.error('Failed to load commands:', e);
            document.getElementById('list-loading').textContent = 'Failed to load commands';
        }
    }
    
    populateNamespaceFilter() {
        const select = document.getElementById('namespace-filter');
        const sorted = [...this.namespaces].sort();
        
        sorted.forEach(ns => {
            const option = document.createElement('option');
            option.value = ns;
            option.textContent = ns + '_';
            select.appendChild(option);
        });
    }
    
    renderCommandList() {
        const container = document.getElementById('command-list');
        container.innerHTML = '';
        
        // Group by namespace
        const groups = {};
        this.filteredCommands.forEach(cmd => {
            const ns = cmd.namespace || 'other';
            if (!groups[ns]) groups[ns] = [];
            groups[ns].push(cmd);
        });
        
        // Sort namespaces
        const sortedNs = Object.keys(groups).sort();
        
        sortedNs.forEach(ns => {
            const group = document.createElement('div');
            group.className = 'namespace-group';
            
            // Namespace header
            const header = document.createElement('div');
            header.className = 'namespace-header';
            header.innerHTML = `<span>${ns}_</span><span class="count">${groups[ns].length}</span>`;
            header.onclick = () => {
                const items = group.querySelector('.namespace-items');
                items.style.display = items.style.display === 'none' ? 'block' : 'none';
            };
            group.appendChild(header);
            
            // Command items
            const items = document.createElement('div');
            items.className = 'namespace-items';
            
            groups[ns].forEach(cmd => {
                const item = document.createElement('div');
                item.className = 'cmd-item';
                item.dataset.slug = cmd.slug;
                
                // Show full command name
                item.innerHTML = `<span class="name">${cmd.title}</span>`;
                
                item.onclick = () => this.selectCommand(cmd.slug);
                items.appendChild(item);
            });
            
            group.appendChild(items);
            container.appendChild(group);
        });
    }
    
    onSearch(query) {
        const nsFilter = document.getElementById('namespace-filter').value;
        this.filterCommands(query, nsFilter);
    }
    
    onNamespaceFilter(namespace) {
        const query = document.getElementById('search-input').value;
        this.filterCommands(query, namespace);
    }
    
    filterCommands(query, namespace) {
        query = query.toLowerCase().trim();
        
        this.filteredCommands = this.commands.filter(cmd => {
            // Namespace filter
            if (namespace && cmd.namespace !== namespace) return false;
            
            // Search query
            if (query) {
                const searchText = `${cmd.title} ${cmd.summary || ''}`.toLowerCase();
                return searchText.includes(query);
            }
            
            return true;
        });
        
        this.renderCommandList();
        this.updateResultCount();
        
        // Auto-select first result if searching
        if (query && this.filteredCommands.length > 0) {
            this.selectCommand(this.filteredCommands[0].slug, false);
        }
    }
    
    updateResultCount() {
        const count = document.getElementById('result-count');
        count.textContent = `${this.filteredCommands.length} of ${this.commands.length}`;
    }
    
    async selectCommand(slug, scrollIntoView = true) {
        // Update selection in list
        document.querySelectorAll('.cmd-item').forEach(el => {
            el.classList.toggle('selected', el.dataset.slug === slug);
        });
        
        if (scrollIntoView) {
            const selected = document.querySelector(`.cmd-item[data-slug="${slug}"]`);
            if (selected) selected.scrollIntoView({ block: 'nearest' });
        }
        
        // Load full command details
        try {
            const response = await this.conn.send(`api_command_get ${slug}`);
            const data = JSON.parse(response);
            
            if (data.command) {
                this.selectedCommand = data.command;
                this.renderCommandDetail(data.command);
            } else if (data.error) {
                console.error('API error:', data.error);
            }
        } catch (e) {
            console.error('Failed to load command:', e);
        }
    }
    
    renderCommandDetail(cmd) {
        const container = document.getElementById('command-detail');
        
        let html = `
            <div class="detail-header">
                <h2>${cmd.title}</h2>
                <div class="meta">
                    ${cmd.category?.name ? `<span>${cmd.category.name}</span>` : ''}
                    ${cmd.returnType ? `<span>Returns: ${cmd.returnType}</span>` : ''}
                </div>
            </div>
        `;
        
        // Summary
        if (cmd.summary) {
            html += `
                <div class="detail-section">
                    <p>${this.escapeHtml(cmd.summary)}</p>
                </div>
            `;
        }
        
        // Syntax
        if (cmd.syntax) {
            html += `
                <div class="detail-section">
                    <h3>Syntax</h3>
                    <div class="syntax-box">${this.escapeHtml(cmd.syntax)}</div>
                </div>
            `;
        }
        
        // Parameters
        if (cmd.parameters && cmd.parameters.length > 0) {
            html += `
                <div class="detail-section">
                    <h3>Parameters</h3>
                    <table class="params-table">
                        <thead>
                            <tr>
                                <th>Name</th>
                                <th>Description</th>
                            </tr>
                        </thead>
                        <tbody>
            `;
            
            cmd.parameters.forEach(p => {
                const typeStr = p.param_type ? `<span class="param-type">${p.param_type}</span>` : '';
                const optStr = p.is_optional ? '<span class="optional">(optional)</span>' : '';
                const defaultStr = p.default_value ? `<span class="optional">default: ${p.default_value}</span>` : '';
                
                html += `
                    <tr>
                        <td>
                            <span class="param-name">${this.escapeHtml(p.name)}</span>
                            ${typeStr}
                        </td>
                        <td>
                            <span class="param-desc">${this.escapeHtml(p.description)}</span>
                            ${optStr} ${defaultStr}
                        </td>
                    </tr>
                `;
            });
            
            html += `
                        </tbody>
                    </table>
                </div>
            `;
        }
        
        // Description/Details
        if (cmd.content && cmd.content !== cmd.summary) {
            html += `
                <div class="detail-section">
                    <h3>Details</h3>
                    <p>${this.formatContent(cmd.content)}</p>
                </div>
            `;
        }
        
        // Examples
        if (cmd.examples && cmd.examples.length > 0) {
            html += `<div class="detail-section"><h3>Example</h3>`;
            
            cmd.examples.forEach(ex => {
                html += `<div class="example-code">${this.escapeHtml(ex.code)}</div>`;
            });
            
            html += `</div>`;
        }
        
        // See Also
        if (cmd.see_also) {
            const links = cmd.see_also.split(',').map(s => s.trim()).filter(s => s);
            if (links.length > 0) {
                html += `
                    <div class="detail-section">
                        <h3>See Also</h3>
                        <div class="see-also-links">
                            ${links.map(link => `<a href="#" data-cmd="${link}">${link}</a>`).join('')}
                        </div>
                    </div>
                `;
            }
        }
        
        // Try It section
        html += `
            <div class="try-it-section">
                <h3>Try It</h3>
                <div class="try-it-editor" id="try-it-editor"></div>
                <div class="try-it-buttons">
                    <button class="primary" id="btn-run">Run</button>
                    <button id="btn-clear-output">Clear</button>
                </div>
                <div class="try-it-output" id="try-it-output"></div>
            </div>
        `;
        
        container.innerHTML = html;
        
        // Set up see-also links
        container.querySelectorAll('.see-also-links a').forEach(link => {
            link.onclick = (e) => {
                e.preventDefault();
                const cmdName = link.dataset.cmd;
                // Update search to find it
                document.getElementById('search-input').value = cmdName;
                this.filterCommands(cmdName, '');
                this.selectCommand(cmdName);
            };
        });
        
        // Initialize Try It editor
        this.initTryItEditor(cmd);
    }
    
    async initTryItEditor(cmd) {
        // Destroy old editor if exists
        if (this.editor) {
            this.editor.destroy();
        }
        
        // Create new editor
        this.editor = new TclEditor('try-it-editor', {
            theme: 'dark',
            fontSize: '12px',
            tabSize: 4,
        });
        
        // Wait for editor to be ready
        const editorEl = document.getElementById('try-it-editor');
        await new Promise(resolve => {
            editorEl.addEventListener('editor-ready', resolve, { once: true });
        });
        
        // Set initial content from example or syntax
        let initialCode = '';
        if (cmd.examples && cmd.examples.length > 0) {
            // Extract first line or simple usage from example
            const example = cmd.examples[0].code;
            const lines = example.split('\n').filter(l => l.trim().startsWith('%'));
            if (lines.length > 0) {
                initialCode = lines[0].replace(/^%\s*/, '');
            } else {
                initialCode = cmd.syntax || cmd.title;
            }
        } else if (cmd.syntax) {
            initialCode = cmd.syntax;
        } else {
            initialCode = cmd.title;
        }
        
        this.editor.setValue(initialCode);
        this.editor.setWebSocket(this.conn);
        
        // Set up buttons
        document.getElementById('btn-run').onclick = () => this.runCode();
        document.getElementById('btn-clear-output').onclick = () => {
            document.getElementById('try-it-output').textContent = '';
            document.getElementById('try-it-output').className = 'try-it-output';
        };
    }
    
    async runCode() {
        if (!this.editor || !this.conn || !this.conn.connected) return;
        
        const code = this.editor.getValue().trim();
        if (!code) return;
        
        const output = document.getElementById('try-it-output');
        output.textContent = 'Running...';
        output.className = 'try-it-output';
        
        try {
            // Send to main Tcl interpreter, not docs subprocess
	    const response = await this.conn.sendToLinked(code);
            output.textContent = response;
            output.className = 'try-it-output success';
        } catch (e) {
            output.textContent = e.message || 'Error';
            output.className = 'try-it-output error';
        }
    }

    async resetInterpreter() {
	try {
            const newName = await this.conn.resetLinkedSubprocess();
            // Re-run initialization
            await this.conn.sendToLinked('package require dlsh');
            
            // Clear output
            document.getElementById('try-it-output').textContent = 'Interpreter reset';
            document.getElementById('try-it-output').className = 'try-it-output success';
	} catch (e) {
            console.error('Failed to reset:', e);
            const output = document.getElementById('try-it-output');
            if (output) {
		output.textContent = 'Reset failed: ' + e.message;
		output.className = 'try-it-output error';
            }
	}
    }
    
    onKeyDown(e) {
        // Arrow key navigation in list
        if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
            const items = [...document.querySelectorAll('.cmd-item')];
            if (items.length === 0) return;
            
            const currentIdx = items.findIndex(el => el.classList.contains('selected'));
            let newIdx;
            
            if (e.key === 'ArrowDown') {
                newIdx = currentIdx < items.length - 1 ? currentIdx + 1 : 0;
            } else {
                newIdx = currentIdx > 0 ? currentIdx - 1 : items.length - 1;
            }
            
            const newSlug = items[newIdx].dataset.slug;
            this.selectCommand(newSlug);
            
            e.preventDefault();
        }
        
        // Enter to focus editor
        if (e.key === 'Enter' && e.target.id === 'search-input') {
            if (this.editor) {
                this.editor.focus();
            }
            e.preventDefault();
        }
        
        // Escape to focus search
        if (e.key === 'Escape') {
            document.getElementById('search-input').focus();
            document.getElementById('search-input').select();
        }
    }
    
    escapeHtml(text) {
        if (!text) return '';
        const div = document.createElement('div');
        div.textContent = text;
        return div.innerHTML;
    }
    
    formatContent(text) {
        if (!text) return '';
        // Basic formatting: preserve line breaks, escape HTML
        return this.escapeHtml(text).replace(/\n/g, '<br>');
    }
}

// Start the app
document.addEventListener('DOMContentLoaded', () => {
    window.app = new CommandReference();
});

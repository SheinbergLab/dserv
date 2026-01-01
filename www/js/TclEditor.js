/**
 * TclEditor.js - CodeMirror 6 based Tcl editor
 * 
 * Features:
 * - Tcl syntax highlighting
 * - Smart tab (re-indent or complete)
 * - Auto-indent on Enter
 * - Emacs or default keybindings
 * - Integration with TclFormatter and TclLinter
 * - WebSocket completion support
 */

class TclEditor {
  constructor(elementId, options = {}) {
    this.container = typeof elementId === 'string' 
      ? document.getElementById(elementId) 
      : elementId;
    
    if (!this.container) {
      throw new Error(`Element not found: ${elementId}`);
    }

    this.options = {
      theme: 'dark',
      fontSize: '14px',
      tabSize: 4,
      lineNumbers: true,
      lineWrapping: true,
      keybindings: options.keybindings || 'default', // 'default' or 'emacs'
      ...options
    };

    this.view = null;
    this.ws = null;
    this.pendingCompletions = null;

    this._initEditor();
  }

  async _initEditor() {
    // Import CodeMirror modules
    const { EditorState } = await import('https://esm.sh/@codemirror/state@6');
    const { EditorView, keymap, lineNumbers, highlightActiveLine, highlightSpecialChars } = await import('https://esm.sh/@codemirror/view@6');
    const { defaultKeymap, history, historyKeymap, indentWithTab } = await import('https://esm.sh/@codemirror/commands@6');
    const { syntaxHighlighting, defaultHighlightStyle, bracketMatching, StreamLanguage } = await import('https://esm.sh/@codemirror/language@6');
    const { dracula } = await import('https://esm.sh/@uiw/codemirror-theme-dracula@4');
    const { autocompletion, completionKeymap, acceptCompletion, completionStatus, startCompletion } = await import('https://esm.sh/@codemirror/autocomplete@6');
    const { tcl } = await import('https://esm.sh/@codemirror/legacy-modes@6/mode/tcl');
    const { searchKeymap, highlightSelectionMatches } = await import('https://esm.sh/@codemirror/search@6');

    // Store references for later use
    this._cm = { EditorState, EditorView, keymap, StreamLanguage, acceptCompletion, completionStatus, startCompletion };

    // Build extensions
    const extensions = [
      lineNumbers(),
      highlightActiveLine(),
      highlightSpecialChars(),
      history(),
      bracketMatching(),
      highlightSelectionMatches(),
      StreamLanguage.define(tcl),
      EditorState.tabSize.of(this.options.tabSize),
      EditorView.lineWrapping,
      dracula,
      autocompletion({
        override: [this._completionSource.bind(this)],
        defaultKeymap: false  // We'll handle Tab ourselves
      }),
    ];

    // Custom keybindings
    const customKeymap = [
      { key: 'Tab', run: this._handleTab.bind(this) },
      { key: 'Shift-Tab', run: this._handleShiftTab.bind(this) },
      { key: 'Enter', run: this._handleEnter.bind(this) },
      { key: 'Shift-Enter', run: this._handleShiftEnter.bind(this) },
      { key: 'Ctrl-/', run: this._toggleComment.bind(this) },
      { key: 'Mod-Shift-f', run: this._formatCode.bind(this) },
    ];

    // Emacs keybindings
    if (this.options.keybindings === 'emacs') {
      customKeymap.push(
        { key: 'Ctrl-a', run: (view) => { this._cursorLineStart(view); return true; } },
        { key: 'Ctrl-e', run: (view) => { this._cursorLineEnd(view); return true; } },
        { key: 'Ctrl-k', run: (view) => { this._killLine(view); return true; } },
        { key: 'Ctrl-y', run: (view) => { this._yank(view); return true; } },
        { key: 'Ctrl-w', run: (view) => { this._killRegion(view); return true; } },
        { key: 'Alt-w', run: (view) => { this._copyRegion(view); return true; } },
        { key: 'Ctrl-g', run: () => true }, // Cancel
        { key: 'Ctrl-Space', run: (view) => { this._setMark(view); return true; } },
      );
    }

    extensions.push(
      keymap.of([
        ...customKeymap,
        ...completionKeymap,  // Must come before defaultKeymap for arrow keys to work in popup
        ...historyKeymap,
        ...defaultKeymap,
        ...searchKeymap,
      ])
    );

    // Custom styling additions (on top of Dracula theme)
    extensions.push(EditorView.theme({
      '&': {
        height: '100%',
        fontSize: this.options.fontSize,
      },
      '.cm-scroller': {
        fontFamily: 'Monaco, Menlo, "Ubuntu Mono", "Cascadia Code", monospace',
      },
      '.cm-focused': {
        outline: 'none',
      },
      // Enhanced bracket matching
      '.cm-matchingBracket': {
        backgroundColor: 'rgba(255, 212, 59, 0.3) !important',
        outline: '1px solid #ffd43b',
      },
      '.cm-nonmatchingBracket': {
        backgroundColor: 'rgba(255, 107, 107, 0.3)',
        outline: '1px solid #ff6b6b',
      },
    }));

    // Create the editor
    const state = EditorState.create({
      doc: '',
      extensions,
    });

    this.view = new EditorView({
      state,
      parent: this.container,
    });

    // Setup context menu
    this._setupContextMenu();

    // Emit ready event
    this.container.dispatchEvent(new CustomEvent('editor-ready', { detail: this }));
  }

  // Context menu setup
  _setupContextMenu() {
    // Create menu element (hidden by default)
    this._contextMenu = document.createElement('div');
    this._contextMenu.className = 'tcl-context-menu';
    this._contextMenu.style.cssText = `
      display: none;
      position: fixed;
      background: #2d2d30;
      border: 1px solid #3e3e42;
      border-radius: 4px;
      box-shadow: 0 2px 8px rgba(0, 0, 0, 0.4);
      padding: 4px 0;
      z-index: 1000;
      min-width: 150px;
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
      font-size: 13px;
    `;
    document.body.appendChild(this._contextMenu);

    // Right-click handler
    this.container.addEventListener('contextmenu', async (e) => {
      e.preventDefault();
      
      const pos = this.view.posAtCoords({ x: e.clientX, y: e.clientY });
      if (pos === null) return;

      const { text } = this._getWordAt(this.view.state, pos);
      
      // Build menu items
      const items = await this._buildContextMenuItems(text, pos);
      
      // Render menu
      this._contextMenu.innerHTML = items.map(item => {
        if (item.separator) {
          return '<div class="tcl-context-menu-separator" style="height: 1px; background: #3e3e42; margin: 4px 0;"></div>';
        }
        const disabled = item.disabled ? 'opacity: 0.5; pointer-events: none;' : '';
        return `<div class="tcl-context-menu-item" data-action="${item.action}" style="padding: 6px 12px; cursor: pointer; color: #d4d4d4; ${disabled}">${item.label}</div>`;
      }).join('');

      // Add hover effect
      this._contextMenu.querySelectorAll('.tcl-context-menu-item').forEach(el => {
        el.addEventListener('mouseenter', () => {
          if (!el.style.pointerEvents) el.style.background = '#094771';
        });
        el.addEventListener('mouseleave', () => {
          el.style.background = '';
        });
        el.addEventListener('click', () => {
          this._handleContextMenuAction(el.dataset.action, text, pos);
          this._hideContextMenu();
        });
      });

      // Position and show menu
      this._contextMenu.style.left = e.clientX + 'px';
      this._contextMenu.style.top = e.clientY + 'px';
      this._contextMenu.style.display = 'block';

      // Adjust if menu goes off screen
      const rect = this._contextMenu.getBoundingClientRect();
      if (rect.right > window.innerWidth) {
        this._contextMenu.style.left = (window.innerWidth - rect.width - 5) + 'px';
      }
      if (rect.bottom > window.innerHeight) {
        this._contextMenu.style.top = (window.innerHeight - rect.height - 5) + 'px';
      }
    });

    // Hide menu on click elsewhere or escape
    document.addEventListener('click', (e) => {
      if (!this._contextMenu.contains(e.target)) {
        this._hideContextMenu();
      }
    });
    
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') {
        this._hideContextMenu();
      }
    });
  }

  _hideContextMenu() {
    this._contextMenu.style.display = 'none';
  }

  async _buildContextMenuItems(word, pos) {
    const items = [];
    
    // Check if there's a selection
    const selection = this.view.state.selection.main;
    const hasSelection = !selection.empty;

    // Execute section first - most useful
    items.push({
      label: '▶️ Execute Selection',
      action: 'executeSelection',
      disabled: !hasSelection
    });

    items.push({
      label: '▶️ Execute Line',
      action: 'executeLine'
    });

    items.push({ separator: true });

    if (word && word.length >= 2) {
      // Check if it's a proc we can show definition for
      let isProc = false;
      if (this.ws) {
        try {
          let result;
          const checkScript = `expr {[info procs {${word}}] ne ""}`;
          if (this.ws.sendToLinked && this.ws.getLinkedSubprocess && this.ws.getLinkedSubprocess()) {
            result = await this.ws.sendToLinked(checkScript);
          } else if (this.ws.send) {
            result = await this.ws.send(checkScript);
          } else if (this.ws.eval) {
            const evalResult = await this.ws.eval(checkScript);
            result = evalResult.result;
          }
          isProc = result && result.trim() === '1';
        } catch (e) {}
      }

      // Check if we can show help (ensemble or proc)
      let hasHelp = isProc;
      if (!hasHelp && this.ws) {
        try {
          let result;
          const checkScript = `namespace ensemble exists {${word}}`;
          if (this.ws.sendToLinked && this.ws.getLinkedSubprocess && this.ws.getLinkedSubprocess()) {
            result = await this.ws.sendToLinked(checkScript);
          } else if (this.ws.send) {
            result = await this.ws.send(checkScript);
          } else if (this.ws.eval) {
            const evalResult = await this.ws.eval(checkScript);
            result = evalResult.result;
          }
          hasHelp = result && result.trim() === '1';
        } catch (e) {}
      }

      items.push({
        label: '📖 Help',
        action: 'help',
        disabled: !hasHelp
      });

      items.push({
        label: '📄 Show Definition',
        action: 'definition',
        disabled: !isProc
      });

      items.push({ separator: true });
    }

    items.push({
      label: '⌨️ Complete',
      action: 'complete'
    });

    items.push({ separator: true });

    items.push({
      label: '📋 Copy',
      action: 'copy'
    });

    items.push({
      label: '✂️ Cut',
      action: 'cut'
    });

    items.push({
      label: '📥 Paste',
      action: 'paste'
    });

    return items;
  }

  async _handleContextMenuAction(action, word, pos) {
    switch (action) {
      case 'executeSelection': {
        const selection = this.view.state.selection.main;
        if (!selection.empty) {
          const code = this.view.state.doc.sliceString(selection.from, selection.to).trim();
          if (code && this.onExecute) {
            this.onExecute(code);
          }
        }
        break;
      }

      case 'executeLine': {
        const selection = this.view.state.selection.main;
        const line = this.view.state.doc.lineAt(selection.head);
        const code = line.text.trim();
        if (code && this.onExecute) {
          this.onExecute(code);
        }
        break;
      }

      case 'help':
        if (word && this.ws) {
          try {
            const infoScript = `
              set _cmd {${word}}
              set _result ""
              if {[namespace ensemble exists $_cmd]} {
                set _subs [namespace ensemble configure $_cmd -subcommands]
                if {$_subs ne ""} {
                  set _result "ensemble: [join [lsort $_subs] {, }]"
                } else {
                  set _map [namespace ensemble configure $_cmd -map]
                  if {$_map ne ""} {
                    set _result "ensemble: [join [lsort [dict keys $_map]] {, }]"
                  }
                }
              } elseif {[info procs $_cmd] ne ""} {
                set _args [info args $_cmd]
                set _arglist {}
                foreach _a $_args {
                  if {[info default $_cmd $_a _def]} {
                    lappend _arglist "?$_a?"
                  } else {
                    lappend _arglist $_a
                  }
                }
                set _result "proc: $_cmd [join $_arglist { }]"
              }
              set _result
            `;
            let result;
            if (this.ws.sendToLinked && this.ws.getLinkedSubprocess && this.ws.getLinkedSubprocess()) {
              result = await this.ws.sendToLinked(infoScript);
            } else if (this.ws.send) {
              result = await this.ws.send(infoScript);
            } else if (this.ws.eval) {
              const evalResult = await this.ws.eval(infoScript);
              result = evalResult.result;
            }
            if (result && result.trim()) {
              this._showHelpModal(word, result.trim());
            }
          } catch (e) {
            console.error('Failed to get help:', e);
          }
        }
        break;

      case 'definition':
        if (word && this.ws) {
          try {
            const defScript = `info body {${word}}`;
            let result;
            if (this.ws.sendToLinked && this.ws.getLinkedSubprocess && this.ws.getLinkedSubprocess()) {
              result = await this.ws.sendToLinked(defScript);
            } else if (this.ws.send) {
              result = await this.ws.send(defScript);
            } else if (this.ws.eval) {
              const evalResult = await this.ws.eval(defScript);
              result = evalResult.result;
            }
            if (result) {
              // Get args too
              const argsScript = `info args {${word}}`;
              let args;
              if (this.ws.sendToLinked && this.ws.getLinkedSubprocess && this.ws.getLinkedSubprocess()) {
                args = await this.ws.sendToLinked(argsScript);
              } else if (this.ws.send) {
                args = await this.ws.send(argsScript);
              } else if (this.ws.eval) {
                const evalResult = await this.ws.eval(argsScript);
                args = evalResult.result;
              }
              
              // Show in a modal or new editor tab
              const fullDef = `proc ${word} {${args || ''}} {${result}}`;
              this._showDefinitionModal(word, fullDef);
            }
          } catch (e) {
            console.error('Failed to get definition:', e);
          }
        }
        break;

      case 'complete':
        this.view.focus();
        this._cm.startCompletion(this.view);
        break;

      case 'copy':
        document.execCommand('copy');
        break;

      case 'cut':
        document.execCommand('cut');
        break;

      case 'paste':
        document.execCommand('paste');
        break;
    }
  }

  _showDefinitionModal(name, definition) {
    // Create modal overlay
    const overlay = document.createElement('div');
    overlay.style.cssText = `
      position: fixed;
      top: 0;
      left: 0;
      right: 0;
      bottom: 0;
      background: rgba(0, 0, 0, 0.5);
      z-index: 2000;
      display: flex;
      align-items: center;
      justify-content: center;
    `;

    const modal = document.createElement('div');
    modal.style.cssText = `
      background: #1e1e1e;
      border: 1px solid #3e3e42;
      border-radius: 6px;
      padding: 16px;
      max-width: 80%;
      max-height: 80%;
      overflow: auto;
      box-shadow: 0 4px 20px rgba(0, 0, 0, 0.5);
    `;

    const header = document.createElement('div');
    header.style.cssText = `
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 12px;
      padding-bottom: 8px;
      border-bottom: 1px solid #3e3e42;
    `;
    header.innerHTML = `
      <span style="color: #d4d4d4; font-weight: bold;">proc ${name}</span>
      <button style="background: none; border: none; color: #808080; cursor: pointer; font-size: 18px;">&times;</button>
    `;

    const pre = document.createElement('pre');
    pre.style.cssText = `
      margin: 0;
      color: #d4d4d4;
      font-family: Monaco, Menlo, "Ubuntu Mono", monospace;
      font-size: 13px;
      white-space: pre-wrap;
    `;
    pre.textContent = definition;

    modal.appendChild(header);
    modal.appendChild(pre);
    overlay.appendChild(modal);
    document.body.appendChild(overlay);

    // Close handlers
    const close = () => document.body.removeChild(overlay);
    header.querySelector('button').addEventListener('click', close);
    overlay.addEventListener('click', (e) => {
      if (e.target === overlay) close();
    });
    document.addEventListener('keydown', function escHandler(e) {
      if (e.key === 'Escape') {
        close();
        document.removeEventListener('keydown', escHandler);
      }
    });
  }

  _showHelpModal(name, info) {
    // Create modal overlay
    const overlay = document.createElement('div');
    overlay.style.cssText = `
      position: fixed;
      top: 0;
      left: 0;
      right: 0;
      bottom: 0;
      background: rgba(0, 0, 0, 0.5);
      z-index: 2000;
      display: flex;
      align-items: center;
      justify-content: center;
    `;

    const modal = document.createElement('div');
    modal.style.cssText = `
      background: #1e1e1e;
      border: 1px solid #3e3e42;
      border-radius: 6px;
      padding: 16px;
      min-width: 300px;
      max-width: 80%;
      max-height: 80%;
      overflow: auto;
      box-shadow: 0 4px 20px rgba(0, 0, 0, 0.5);
    `;

    const header = document.createElement('div');
    header.style.cssText = `
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 12px;
      padding-bottom: 8px;
      border-bottom: 1px solid #3e3e42;
    `;
    header.innerHTML = `
      <span style="color: #d4d4d4; font-weight: bold;">${name}</span>
      <button style="background: none; border: none; color: #808080; cursor: pointer; font-size: 18px;">&times;</button>
    `;

    const content = document.createElement('div');
    content.style.cssText = `
      color: #d4d4d4;
      font-family: Monaco, Menlo, "Ubuntu Mono", monospace;
      font-size: 13px;
      white-space: pre-wrap;
      word-break: break-word;
    `;
    content.textContent = info;

    modal.appendChild(header);
    modal.appendChild(content);
    overlay.appendChild(modal);
    document.body.appendChild(overlay);

    // Close handlers
    const close = () => document.body.removeChild(overlay);
    header.querySelector('button').addEventListener('click', close);
    overlay.addEventListener('click', (e) => {
      if (e.target === overlay) close();
    });
    document.addEventListener('keydown', function escHandler(e) {
      if (e.key === 'Escape') {
        close();
        document.removeEventListener('keydown', escHandler);
      }
    });
  }

  // Smart Tab handling
  _handleTab(view) {
    const state = view.state;
    const selection = state.selection.main;

    // Check if completion popup is open - if so, accept the selected completion
    if (this._cm.completionStatus(state) === 'active') {
      return this._cm.acceptCompletion(view);
    }

    // If there's a selection, indent it
    if (!selection.empty) {
      return this._indentSelection(view);
    }

    const line = state.doc.lineAt(selection.head);
    const textBeforeCursor = state.doc.sliceString(line.from, selection.head);

    // If at beginning of line or only whitespace, re-indent
    if (/^\s*$/.test(textBeforeCursor)) {
      this._reindentLine(view, line);
      return true;
    }

    // Otherwise trigger completion explicitly
    this._cm.startCompletion(view);
    return true;
  }

  _handleShiftTab(view) {
    return this._dedentSelection(view);
  }

  _handleEnter(view) {
    const state = view.state;
    
    // If completion popup is open, accept the selected completion
    if (this._cm.completionStatus(state) === 'active') {
      return this._cm.acceptCompletion(view);
    }
    
    const selection = state.selection.main;
    const line = state.doc.lineAt(selection.head);

    // Calculate proper indent for next line
    const allText = state.doc.toString();
    const lines = TclFormatter.splitLines(allText);
    const currentLineNum = line.number - 1;

    let nextIndent = 0;
    try {
      nextIndent = TclFormatter.calculateLineIndent(lines, currentLineNum + 1, this.options.tabSize);
    } catch (e) {
      // Fallback: use current line's indent
      const match = line.text.match(/^(\s*)/);
      nextIndent = match ? match[1].length : 0;
      // Add indent if line ends with {
      if (line.text.trim().endsWith('{')) {
        nextIndent += this.options.tabSize;
      }
    }

    const newIndent = ' '.repeat(nextIndent);
    const newline = '\n' + newIndent;

    view.dispatch({
      changes: { from: selection.head, insert: newline },
      selection: { anchor: selection.head + newline.length },
    });

    return true;
  }

  _handleShiftEnter(view) {
    const state = view.state;
    const selection = state.selection.main;
    
    let code;
    if (!selection.empty) {
      // Run selection
      code = state.doc.sliceString(selection.from, selection.to);
    } else {
      // Run current line
      const line = state.doc.lineAt(selection.head);
      code = line.text;
    }
    
    code = code.trim();
    if (!code) return true;
    
    // Call the onExecute callback if set
    if (this.onExecute) {
      this.onExecute(code);
    }
    
    return true;
  }

  _reindentLine(view, line) {
    const state = view.state;
    const allText = state.doc.toString();
    const lines = TclFormatter.splitLines(allText);
    const lineNum = line.number - 1;

    let targetIndent = 0;
    try {
      targetIndent = TclFormatter.calculateLineIndent(lines, lineNum, this.options.tabSize);
    } catch (e) {
      targetIndent = 0;
    }

    const currentIndent = line.text.match(/^(\s*)/)[0].length;
    const newIndent = ' '.repeat(targetIndent);

    view.dispatch({
      changes: { from: line.from, to: line.from + currentIndent, insert: newIndent },
      selection: { anchor: line.from + targetIndent },
    });
  }

  _indentSelection(view) {
    const state = view.state;
    const selection = state.selection.main;
    const startLine = state.doc.lineAt(selection.from);
    const endLine = state.doc.lineAt(selection.to);

    const changes = [];
    for (let i = startLine.number; i <= endLine.number; i++) {
      const line = state.doc.line(i);
      changes.push({ from: line.from, insert: '    ' });
    }

    view.dispatch({ changes });
    return true;
  }

  _dedentSelection(view) {
    const state = view.state;
    const selection = state.selection.main;
    const startLine = state.doc.lineAt(selection.from);
    const endLine = state.doc.lineAt(selection.to);

    const changes = [];
    for (let i = startLine.number; i <= endLine.number; i++) {
      const line = state.doc.line(i);
      const match = line.text.match(/^( {1,4}|\t)/);
      if (match) {
        changes.push({ from: line.from, to: line.from + match[0].length, insert: '' });
      }
    }

    if (changes.length) {
      view.dispatch({ changes });
    }
    return true;
  }

  _toggleComment(view) {
    const state = view.state;
    const selection = state.selection.main;
    const line = state.doc.lineAt(selection.head);

    if (line.text.trim().startsWith('#')) {
      // Uncomment
      const match = line.text.match(/^(\s*)#\s?/);
      if (match) {
        view.dispatch({
          changes: { from: line.from, to: line.from + match[0].length, insert: match[1] },
        });
      }
    } else {
      // Comment
      const indent = line.text.match(/^\s*/)[0];
      view.dispatch({
        changes: { from: line.from + indent.length, insert: '# ' },
      });
    }
    return true;
  }

  _formatCode(view) {
    const state = view.state;
    const code = state.doc.toString();
    const formatted = TclFormatter.formatTclCode(code, this.options.tabSize);

    view.dispatch({
      changes: { from: 0, to: state.doc.length, insert: formatted },
    });
    return true;
  }

  // Emacs commands
  _killRing = [];

  _cursorLineStart(view) {
    const selection = view.state.selection.main;
    const line = view.state.doc.lineAt(selection.head);
    view.dispatch({ selection: { anchor: line.from } });
  }

  _cursorLineEnd(view) {
    const selection = view.state.selection.main;
    const line = view.state.doc.lineAt(selection.head);
    view.dispatch({ selection: { anchor: line.to } });
  }

  _killLine(view) {
    const selection = view.state.selection.main;
    const line = view.state.doc.lineAt(selection.head);
    const textToKill = view.state.doc.sliceString(selection.head, line.to);
    
    this._killRing.push(textToKill || '\n');
    
    const to = textToKill ? line.to : Math.min(line.to + 1, view.state.doc.length);
    view.dispatch({
      changes: { from: selection.head, to },
    });
  }

  _yank(view) {
    if (this._killRing.length > 0) {
      const text = this._killRing[this._killRing.length - 1];
      const selection = view.state.selection.main;
      view.dispatch({
        changes: { from: selection.head, insert: text },
        selection: { anchor: selection.head + text.length },
      });
    }
  }

  _killRegion(view) {
    const selection = view.state.selection.main;
    if (!selection.empty) {
      const text = view.state.doc.sliceString(selection.from, selection.to);
      this._killRing.push(text);
      view.dispatch({
        changes: { from: selection.from, to: selection.to, insert: '' },
      });
    }
  }

  _copyRegion(view) {
    const selection = view.state.selection.main;
    if (!selection.empty) {
      const text = view.state.doc.sliceString(selection.from, selection.to);
      this._killRing.push(text);
    }
  }

  _setMark(view) {
    // Mark is handled by default selection behavior
  }

  // Completion source
  async _completionSource(context) {
    if (!this.ws) return null;

    const line = context.state.doc.lineAt(context.pos);
    const textBeforeCursor = context.state.doc.sliceString(line.from, context.pos);

    // Determine token boundaries and completion text
    let from;
    let completionText;

    // Check if we're inside a quoted string
    const quoteMatch = textBeforeCursor.match(/"([^"]*)$/);
    if (quoteMatch) {
      // Inside quotes - complete the quoted content
      const quotedPart = quoteMatch[1];
      from = context.pos - quotedPart.length;
      completionText = textBeforeCursor;
    } else {
      // Normal word matching - include path characters
      const wordMatch = textBeforeCursor.match(/[~.\w_:$/-]+$/);
      if (!wordMatch) return null;

      const word = wordMatch[0];
      from = context.pos - word.length;

      // Don't try to complete very short tokens (avoid noise)
      // But allow if there's context (space before the word means we're completing an argument)
      const hasContext = textBeforeCursor.length > word.length && 
                         textBeforeCursor[textBeforeCursor.length - word.length - 1] === ' ';
      if (word.length < 2 && !word.startsWith('$') && !word.startsWith('/') && !hasContext) return null;

      completionText = textBeforeCursor;
    }

    try {
      // Use sendToLinked if available, otherwise send
      // Use complete_token to get just the token completions
      let result;
      if (this.ws.sendToLinked && this.ws.getLinkedSubprocess && this.ws.getLinkedSubprocess()) {
        result = await this.ws.sendToLinked(`complete_token {${completionText}}`);
      } else if (this.ws.send) {
        result = await this.ws.send(`complete_token {${completionText}}`);
      } else if (this.ws.eval) {
        const evalResult = await this.ws.eval(`complete_token {${completionText}}`);
        result = evalResult.result;
      } else {
        return null;
      }
      
      if (result) {
        const tokens = this._parseTclList(result);
        if (tokens.length > 0) {
          return {
            from,
            options: tokens.map(token => ({
              label: token,
              type: completionText.includes('$') ? 'variable' : 
                    (completionText.includes('/') || completionText.includes('~')) ? 'text' : 'function',
            })),
          };
        }
      }
    } catch (e) {
      // Silently ignore completion errors - they're expected during editing
      // console.error('Completion error:', e);
    }

    return null;
  }

  _parseTclList(str) {
    if (!str || str.trim() === '') return [];
    if (!str.includes('{') && !str.includes('"')) {
      return str.trim().split(/\s+/);
    }

    const result = [];
    let current = '';
    let inBraces = 0;
    let inQuotes = false;

    for (let i = 0; i < str.length; i++) {
      const char = str[i];

      if (char === '{' && !inQuotes) {
        inBraces++;
        if (inBraces === 1) continue;
      } else if (char === '}' && !inQuotes) {
        inBraces--;
        if (inBraces === 0 && current) {
          result.push(current);
          current = '';
          continue;
        }
      } else if (char === '"' && inBraces === 0) {
        inQuotes = !inQuotes;
        continue;
      } else if (char === ' ' && inBraces === 0 && !inQuotes) {
        if (current) {
          result.push(current);
          current = '';
        }
        continue;
      }

      current += char;
    }

    if (current) result.push(current);
    return result;
  }

  // Helper to get word at position
  _getWordAt(state, pos) {
    const line = state.doc.lineAt(pos);
    const lineText = line.text;
    const linePos = pos - line.from;

    // Find word boundaries
    let start = linePos;
    let end = linePos;

    // Word characters for Tcl: alphanumeric, underscore, colons (for namespaces)
    const isWordChar = (ch) => /[\w:]/.test(ch);

    while (start > 0 && isWordChar(lineText[start - 1])) start--;
    while (end < lineText.length && isWordChar(lineText[end])) end++;

    const text = lineText.slice(start, end);
    return {
      from: line.from + start,
      to: line.from + end,
      text
    };
  }

  // Public API
  setWebSocket(ws) {
    this.ws = ws;
  }

  setOnExecute(callback) {
    this.onExecute = callback;
  }

  setValue(text, cursorPos = -1) {
    if (!this.view) return;
    this.view.dispatch({
      changes: { from: 0, to: this.view.state.doc.length, insert: text },
      selection: cursorPos === -1 ? { anchor: 0 } : undefined,
    });
  }

  getValue() {
    return this.view ? this.view.state.doc.toString() : '';
  }

  focus() {
    if (this.view) this.view.focus();
  }

  lint() {
    const code = this.getValue();
    return new TclLinter().lint(code);
  }

  format() {
    if (this.view) {
      this._formatCode(this.view);
    }
  }

  destroy() {
    if (this.view) {
      this.view.destroy();
      this.view = null;
    }
  }
}

// Export for browser
window.TclEditor = TclEditor;
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

    // Emit ready event
    this.container.dispatchEvent(new CustomEvent('editor-ready', { detail: this }));
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

    // Find word start - match word characters, colons (for namespaces), $ (for variables)
    const wordMatch = textBeforeCursor.match(/[\w_:$.-]+$/);
    if (!wordMatch) return null;

    const word = wordMatch[0];
    const from = context.pos - word.length;
    
    // Don't try to complete very short tokens (avoid noise)
    if (word.length < 2 && !word.startsWith('$')) return null;

    // Determine what to send for completion
    // For simple command completion (no special chars), just send the word
    // For variables ($) or namespaces (::), send more context
    let completionText;
    if (word.startsWith('$') || word.includes('::')) {
      // Send the word with some context
      completionText = word;
    } else {
      // For command completion, just send the token being typed
      // This avoids errors from incomplete preceding commands
      completionText = word;
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
              type: word.startsWith('$') ? 'variable' : 'function',
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

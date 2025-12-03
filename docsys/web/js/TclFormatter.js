/**
 * TclFormatter.js - Tcl code formatting with proper indentation and continuation handling
 * Adapted for vanilla JS (no ES6 module export for browser use)
 */

class TclFormatter {
  /**
   * Format Tcl code with proper indentation and continuation alignment
   */
  static formatTclCode(code, indentSize = 4) {
    const lines = this.splitLines(code);
    const formattedLines = [];
    let indentLevel = 0;
    let inContinuation = false;
    let continuationContext = null;

    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      const trimmed = this.trim(line);

      // Skip empty lines but preserve them
      if (trimmed.length === 0) {
        formattedLines.push('');
        continue;
      }

      // Handle comments
      if (trimmed[0] === '#') {
        let commentIndent;

        if (inContinuation) {
          commentIndent = this.calculateContinuationIndent(continuationContext, indentSize);
        } else {
          commentIndent = indentLevel * indentSize;
        }

        formattedLines.push(' '.repeat(commentIndent) + trimmed);

        const endsWithBackslash = trimmed.endsWith('\\');
        if (!inContinuation && endsWithBackslash) {
          inContinuation = true;
          continuationContext = this.createContinuationContext(trimmed, indentLevel, i);
        } else if (inContinuation && !endsWithBackslash) {
          inContinuation = false;
          continuationContext = null;
        }
        continue;
      }

      // Check for line continuation
      const endsWithBackslash = trimmed.endsWith('\\');

      // Count braces in current line
      const openBraces = this.countUnquotedChar(trimmed, '{');
      const closeBraces = this.countUnquotedChar(trimmed, '}');

      // Calculate current line indent
      let currentIndent;

      if (!inContinuation) {
        currentIndent = indentLevel * indentSize;

        if (trimmed[0] === '}') {
          currentIndent = Math.max(0, (indentLevel - 1) * indentSize);
        }

        if (this.startsWithKeyword(trimmed, ['else', 'elseif', 'catch'])) {
          currentIndent = Math.max(0, (indentLevel - 1) * indentSize);
        }
      } else {
        currentIndent = this.calculateContinuationIndent(continuationContext, indentSize, trimmed);
      }

      const formattedLine = this.formatLine(trimmed, currentIndent);
      formattedLines.push(formattedLine);

      // Update state
      if (!inContinuation && endsWithBackslash) {
        inContinuation = true;
        continuationContext = this.createContinuationContext(trimmed, indentLevel, i);
        indentLevel += (openBraces - closeBraces);
        if (indentLevel < 0) indentLevel = 0;
      } else if (inContinuation && endsWithBackslash) {
        this.updateContinuationContext(continuationContext, trimmed);
        indentLevel += (openBraces - closeBraces);
        if (indentLevel < 0) indentLevel = 0;
      } else if (inContinuation && !endsWithBackslash) {
        indentLevel += (openBraces - closeBraces);
        if (indentLevel < 0) indentLevel = 0;
        inContinuation = false;
        continuationContext = null;
      } else {
        indentLevel += (openBraces - closeBraces);
        if (indentLevel < 0) indentLevel = 0;
      }
    }

    return this.joinLines(formattedLines);
  }

  /**
   * Create a continuation context for tracking alignment
   */
  static createContinuationContext(firstLine, baseIndentLevel, lineIndex) {
    const context = {
      baseIndentLevel: baseIndentLevel,
      firstLine: firstLine,
      lineIndex: lineIndex,
      bracketDepth: 0,
      commandNesting: 0,
      alignmentColumn: null
    };

    context.alignmentColumn = this.findAlignmentColumn(firstLine);
    context.bracketDepth = this.countOpeningBrackets(firstLine) - this.countClosingBrackets(firstLine);

    return context;
  }

  /**
   * Update continuation context as we process more lines
   */
  static updateContinuationContext(context, line) {
    if (!context) return;

    const openBrackets = this.countOpeningBrackets(line);
    const closeBrackets = this.countClosingBrackets(line);
    context.bracketDepth += (openBrackets - closeBrackets);

    if (context.bracketDepth < 0) context.bracketDepth = 0;
  }

  /**
   * Calculate the proper indentation for a continuation line
   */
  static calculateContinuationIndent(context, indentSize, currentLine = '') {
    if (!context) return indentSize;

    if (context.alignmentColumn !== null) {
      return context.alignmentColumn;
    }

    const baseIndent = context.baseIndentLevel * indentSize;
    const continuationIndent = indentSize;
    const bracketIndent = Math.max(0, context.bracketDepth) * indentSize;
    return baseIndent + continuationIndent + bracketIndent;
  }

  /**
   * Find the alignment column for continuation lines
   */
  static findAlignmentColumn(line) {
    const trimmed = line.trim();
    const leadingSpaces = line.length - line.trimStart().length;

    // Pattern: dl_local var [command \
    const dlLocalMatch = trimmed.match(/^dl_local\s+\S+\s+\[/);
    if (dlLocalMatch) {
      const bracketPos = trimmed.indexOf('[');
      return leadingSpaces + bracketPos + 1;
    }

    // Pattern: set var [command \
    const setMatch = trimmed.match(/^set\s+\S+\s+\[/);
    if (setMatch) {
      const bracketPos = trimmed.indexOf('[');
      return leadingSpaces + bracketPos + 1;
    }

    // General pattern: look for opening bracket
    const bracketIndex = trimmed.indexOf('[');
    if (bracketIndex !== -1) {
      return leadingSpaces + bracketIndex + 1;
    }

    return null;
  }

  /**
   * Calculate line indentation in context
   */
  static calculateLineIndent(lines, currentLineIndex, indentSize = 4) {
    if (currentLineIndex < 0 || currentLineIndex >= lines.length) {
      return 0;
    }

    let indentLevel = 0;
    let inContinuation = false;
    let continuationContext = null;

    for (let i = 0; i < currentLineIndex; i++) {
      const line = lines[i];
      const trimmed = this.trim(line);

      if (trimmed.length === 0 || trimmed[0] === '#') {
        continue;
      }

      const endsWithBackslash = trimmed.endsWith('\\');
      const openBraces = this.countUnquotedChar(trimmed, '{');
      const closeBraces = this.countUnquotedChar(trimmed, '}');

      if (!inContinuation && endsWithBackslash) {
        inContinuation = true;
        continuationContext = this.createContinuationContext(trimmed, indentLevel, i);
        indentLevel += (openBraces - closeBraces);
      } else if (inContinuation && endsWithBackslash) {
        this.updateContinuationContext(continuationContext, trimmed);
        indentLevel += (openBraces - closeBraces);
      } else if (inContinuation && !endsWithBackslash) {
        indentLevel += (openBraces - closeBraces);
        inContinuation = false;
        continuationContext = null;
      } else {
        indentLevel += (openBraces - closeBraces);
      }

      if (indentLevel < 0) indentLevel = 0;
    }

    const currentTrimmed = this.trim(lines[currentLineIndex]);

    if (inContinuation) {
      return this.calculateContinuationIndent(continuationContext, indentSize, currentTrimmed);
    }

    let currentIndent = indentLevel;

    if (currentTrimmed.length > 0) {
      if (currentTrimmed[0] === '}') {
        currentIndent = Math.max(0, indentLevel - 1);
      }
      if (this.startsWithKeyword(currentTrimmed, ['else', 'elseif', 'catch'])) {
        currentIndent = Math.max(0, indentLevel - 1);
      }
    }

    return currentIndent * indentSize;
  }

  // Helper methods
  static countOpeningBrackets(line) {
    let count = 0;
    let inQuotes = false;
    let escaped = false;

    for (const c of line) {
      if (escaped) { escaped = false; continue; }
      if (c === '\\') { escaped = true; continue; }
      if (c === '"') { inQuotes = !inQuotes; continue; }
      if (!inQuotes && c === '[') count++;
    }
    return count;
  }

  static countClosingBrackets(line) {
    let count = 0;
    let inQuotes = false;
    let escaped = false;

    for (const c of line) {
      if (escaped) { escaped = false; continue; }
      if (c === '\\') { escaped = true; continue; }
      if (c === '"') { inQuotes = !inQuotes; continue; }
      if (!inQuotes && c === ']') count++;
    }
    return count;
  }

  static splitLines(text) {
    return text.split('\n');
  }

  static joinLines(lines) {
    return lines.join('\n');
  }

  static trim(str) {
    return str.trim();
  }

  static startsWithKeyword(line, keywords) {
    for (const keyword of keywords) {
      const pattern = new RegExp(`^\\s*${keyword}\\b`);
      if (pattern.test(line)) return true;
    }
    return false;
  }

  static countUnquotedChar(str, target) {
    let count = 0;
    let inQuotes = false;
    let inBraces = false;
    let braceDepth = 0;
    let escaped = false;

    for (const c of str) {
      if (escaped) { escaped = false; continue; }
      if (c === '\\') { escaped = true; continue; }
      if (c === '"' && !inBraces) { inQuotes = !inQuotes; continue; }

      if (!inQuotes) {
        if (c === '{') {
          braceDepth++;
          inBraces = (braceDepth > 0);
        } else if (c === '}') {
          braceDepth--;
          inBraces = (braceDepth > 0);
        }
      }

      if (!inQuotes && c === target) {
        if (target === '{' || target === '}' || !inBraces) {
          count++;
        }
      }
    }

    return count;
  }

  static formatLine(line, indentSpaces) {
    const indent = ' '.repeat(Math.max(0, indentSpaces));
    const endsWithContinuation = line.trimEnd().endsWith('\\');

    let normalized = '';
    let inQuotes = false;
    let escaped = false;
    let prevChar = '';

    for (const c of line) {
      if (escaped) {
        normalized += c;
        escaped = false;
        prevChar = c;
        continue;
      }

      if (c === '\\') {
        normalized += c;
        escaped = true;
        prevChar = c;
        continue;
      }

      if (c === '"') {
        inQuotes = !inQuotes;
        normalized += c;
        prevChar = c;
        continue;
      }

      if (inQuotes) {
        normalized += c;
        prevChar = c;
      } else {
        if (/\s/.test(c)) {
          if (prevChar !== ' ' && prevChar !== '') {
            normalized += ' ';
            prevChar = ' ';
          }
        } else {
          normalized += c;
          prevChar = c;
        }
      }
    }

    if (endsWithContinuation) {
      normalized = normalized.replace(/\s+\\$/, ' \\');
      normalized = normalized.replace(/\s{2,}\\$/, ' \\');
    } else {
      normalized = normalized.replace(/\s+$/, '');
    }

    return indent + normalized;
  }
}

// Export for browser
window.TclFormatter = TclFormatter;
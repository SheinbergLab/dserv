/**
 * Enhanced TclFormatter - Improved handling of continuations and command substitutions
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
    let continuationContext = null; // Track continuation context for alignment

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
		// If we're in a continuation, comments should align with the continuation
		commentIndent = this.calculateContinuationIndent(continuationContext, indentSize);
	    } else {
		// Normal comment - just use the current block indent level
		commentIndent = indentLevel * indentSize;
	    }

	    formattedLines.push(' '.repeat(commentIndent) + trimmed);

	    // Comments don't affect continuation state unless they end with backslash
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

	// Count braces and brackets in current line
	const openBraces = this.countUnquotedChar(trimmed, '{');
	const closeBraces = this.countUnquotedChar(trimmed, '}');

	// Calculate current line indent
	let currentIndent;

	if (!inContinuation) {
	    // Normal line - convert indent LEVEL to SPACES
	    currentIndent = indentLevel * indentSize;  // ← Convert to spaces

	    // Adjust for closing braces at start of line
	    if (trimmed[0] === '}') {
		currentIndent = Math.max(0, (indentLevel - 1) * indentSize);  // ← Convert to spaces
	    }

	    // Handle special keywords
	    if (this.startsWithKeyword(trimmed, ['else', 'elseif', 'catch'])) {
		currentIndent = Math.max(0, (indentLevel - 1) * indentSize);  // ← Convert to spaces
	    }
	} else {
	    // Continuation line - this already returns SPACES
	    currentIndent = this.calculateContinuationIndent(continuationContext, indentSize, trimmed);
	}

      // Format the line with current indentation
      const formattedLine = this.formatLine(trimmed, currentIndent);
      formattedLines.push(formattedLine);

      // Update state based on line type
      if (!inContinuation && endsWithBackslash) {
        // Starting a new continuation
        inContinuation = true;
        continuationContext = this.createContinuationContext(trimmed, indentLevel, i);
        // Count braces for overall indent level
        indentLevel += (openBraces - closeBraces);
        if (indentLevel < 0) indentLevel = 0;
      } else if (inContinuation && endsWithBackslash) {
        // Continuing a continuation - update context
        this.updateContinuationContext(continuationContext, trimmed);
        indentLevel += (openBraces - closeBraces);
        if (indentLevel < 0) indentLevel = 0;
      } else if (inContinuation && !endsWithBackslash) {
        // Ending continuation
        indentLevel += (openBraces - closeBraces);
        if (indentLevel < 0) indentLevel = 0;
        inContinuation = false;
        continuationContext = null;
      } else {
        // Normal line (not in continuation)
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

    // Analyze the first line to determine alignment strategy
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

    // Ensure bracket depth doesn't go negative
    if (context.bracketDepth < 0) context.bracketDepth = 0;
  }

  /**
   * Calculate the proper indentation for a continuation line
   */
static calculateContinuationIndent(context, indentSize, currentLine = '') {
  if (!context) return indentSize;

  // If we found a specific alignment point, use it with minimal extra indentation
  if (context.alignmentColumn !== null) {
    // Use simple alignment - just align to the position after the opening bracket
    const result = context.alignmentColumn;
    return result;
  }

  // Fallback: base + continuation + bracket nesting
  const baseIndent = context.baseIndentLevel * indentSize;
  const continuationIndent = indentSize;
  const bracketIndent = Math.max(0, context.bracketDepth) * indentSize;
  const result = baseIndent + continuationIndent + bracketIndent;
  return result;
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
   * Enhanced calculation for line indentation in context
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

    // Calculate indent for the current line
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

  // Keep all the existing helper methods
  static countOpeningBrackets(line) {
    let count = 0;
    let inQuotes = false;
    let escaped = false;

    for (const c of line) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (c === '\\') {
        escaped = true;
        continue;
      }
      if (c === '"') {
        inQuotes = !inQuotes;
        continue;
      }
      if (!inQuotes && c === '[') {
        count++;
      }
    }
    return count;
  }

  static countClosingBrackets(line) {
    let count = 0;
    let inQuotes = false;
    let escaped = false;

    for (const c of line) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (c === '\\') {
        escaped = true;
        continue;
      }
      if (c === '"') {
        inQuotes = !inQuotes;
        continue;
      }
      if (!inQuotes && c === ']') {
        count++;
      }
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
      if (pattern.test(line)) {
        return true;
      }
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
      if (escaped) {
        escaped = false;
        continue;
      }

      if (c === '\\') {
        escaped = true;
        continue;
      }

      if (c === '"' && !inBraces) {
        inQuotes = !inQuotes;
        continue;
      }

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
  // indentSpaces is already the total number of spaces needed
  const indent = ' '.repeat(Math.max(0, indentSpaces));

  // Check if line ends with continuation before processing
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

  // Remove trailing whitespace BUT preserve line continuation
  if (endsWithContinuation) {
    // Remove trailing whitespace except for the final backslash
    normalized = normalized.replace(/\s+\\$/, ' \\');
    // Handle case where there might be multiple spaces before backslash
    normalized = normalized.replace(/\s{2,}\\$/, ' \\');
  } else {
    // Normal case - remove all trailing whitespace
    normalized = normalized.replace(/\s+$/, '');
  }

  return indent + normalized;
}
}

export default TclFormatter;

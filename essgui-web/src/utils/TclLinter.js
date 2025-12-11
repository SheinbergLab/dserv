// TclLinter.js with comprehensive ESS knowledge

export class TclLinter {
  constructor() {
    this.errors = [];
    this.warnings = [];

    // Comprehensive ESS system methods
    this.essSystemMethods = new Set([
      'add_state', 'add_action', 'add_transition', 'add_param', 'add_variable', 'add_method',
      'set_start', 'set_end', 'get_params', 'set_param', 'get_states', 'status', 'set_status',
      'init', 'deinit', 'start', 'stop', 'reset', 'update', 'do_action', 'do_transition',
      'set_init_callback', 'set_deinit_callback', 'set_protocol_init_callback',
      'set_protocol_deinit_callback', 'set_final_init_callback',
      'set_start_callback', 'set_end_callback', 'set_reset_callback', 'set_quit_callback',
      'set_file_suggest_callback', 'set_file_open_callback', 'set_file_close_callback',
      'set_subject_callback', 'configure_stim', 'update_stimdg',
      'file_suggest', 'file_open', 'file_close', 'set_subject',
      'name', 'get_system', 'set_version', 'get_version', 'set_protocol', 'get_protocol',
      'set_variants', 'get_variants', 'set_variant', 'get_variant', 'add_variant',
      'reset_variant_args', 'set_variant_args', 'get_variant_args',
      'get_variable', 'set_variable', 'set_parameters', 'set_default_param_vals',
      'protocol_init', 'protocol_deinit', 'final_init'
    ]);

    // ESS event types
    this.essEventTypes = new Set([
      'MAGIC', 'NAME', 'FILE', 'USER', 'TRACE', 'PARAM', 'SUBTYPES', 'SYSTEM_STATE',
      'FSPIKE', 'HSPIKE', 'ID', 'BEGINOBS', 'ENDOBS', 'ISI', 'TRIALTYPE', 'OBSTYPE',
      'EMLOG', 'FIXSPOT', 'EMPARAMS', 'STIMULUS', 'PATTERN', 'STIMTYPE', 'SAMPLE',
      'PROBE', 'CUE', 'TARGET', 'DISTRACTOR', 'SOUND', 'CHOICES', 'FIXATE', 'RESP',
      'SACCADE', 'DECIDE', 'ENDTRIAL', 'ABORT', 'REWARD', 'DELAY', 'PUNISH', 'PHYS',
      'MRI', 'STIMULATOR', 'TOUCH', 'TARGNAME', 'SCENENAME', 'SACCADE_INFO',
      'STIM_TRIGGER', 'MOVIENAME', 'STIMULATION', 'SECOND_CHANCE', 'SECOND_RESP',
	'SWAPBUFFER', 'STIM_DATA', 'DIGITAL_LINES', 'STATE_DEBUG', 'FEEDBACK'
    ]);

    // ESS functions
    this.essFunctions = new Set([
      'timerTick', 'timerExpired', 'now', 'dservSet', 'dservGet', 'dservTouch', 'dservClear',
      'rmtSend', 'rmtOpen', 'rmtClose', 'rmtConnected', 'rmtName', 'soundPlay', 'soundReset',
      'soundSetVoice', 'soundVolume', 'evtPut', 'evt_put', 'begin_obs', 'end_obs',
      'em_region_on', 'em_region_off', 'em_fixwin_set', 'em_eye_in_region',
      'em_sampler_enable', 'em_sampler_start', 'em_sampler_status', 'em_sampler_vals',
      'touch_region_on', 'touch_region_off', 'touch_win_set', 'touch_in_region',
      'reward', 'juicer_init', 'print', 'my'
    ]);
  }

  lint(code) {
    this.errors = [];
    this.warnings = [];

    try {
      this.checkBasicSyntax(code);
      this.checkBraceBalance(code);
      this.checkQuoteBalance(code);
      this.checkEssPatterns(code);

      return {
        isValid: this.errors.length === 0,
        errors: this.errors,
        warnings: this.warnings,
        summary: this.getSummary()
      };
    } catch (error) {
      this.errors.push({
        line: 0,
        column: 0,
        message: `Linter error: ${error.message}`,
        severity: 'error'
      });

      return {
        isValid: false,
        errors: this.errors,
        warnings: this.warnings,
        summary: 'Linting failed'
      };
    }
  }

checkEssPatterns(code) {
  const lines = code.split('\n');
  let hasTimerTick = false;
  let hasTimerExpired = false;
  const timerTickLines = [];

  // First pass: collect timer usage
  lines.forEach((line, lineNum) => {
    if (line.includes('timerTick')) {
      hasTimerTick = true;
      timerTickLines.push(lineNum + 1);
    }
    if (line.includes('timerExpired')) {
      hasTimerExpired = true;
    }
  });

  // Second pass: analyze patterns
  lines.forEach((line, lineNum) => {
    const trimmed = line.trim();

    // Skip empty lines and comments
    if (!trimmed || trimmed.startsWith('#')) {
      return;
    }

    // Check for ESS system method calls
    const sysMethodMatch = line.match(/\$sys(?:tem)?\s+(\w+)/);
    if (sysMethodMatch) {
      const method = sysMethodMatch[1];

      if (!this.essSystemMethods.has(method)) {
        this.warnings.push({
          line: lineNum + 1,
          column: sysMethodMatch.index + 1,
          message: `Unknown ESS system method: ${method}`,
          severity: 'warning'
        });
      }
    }

    // Check for event types
    const eventMatch = line.match(/::ess::evt_put\s+(\w+)/);
    if (eventMatch) {
      const eventType = eventMatch[1];

      if (!this.essEventTypes.has(eventType)) {
        this.warnings.push({
          line: lineNum + 1,
          column: eventMatch.index + 1,
          message: `Unknown event type: ${eventType}`,
          severity: 'warning'
        });
      }
    }

    // FIXED: Check for state transitions
    const transitionMatch = line.match(/\$sys\s+add_transition\s+\w+/);
    if (transitionMatch) {
      // Check if this is a one-liner with inline return
      if (line.includes('return')) {
        // One-liner with return - this is fine, no warning needed
        return;
      }

      // Check if this starts a block with braces
      if (line.includes('{')) {
        // Look for return in the transition block
        let blockHasReturn = false;
        let braceCount = (line.match(/\{/g) || []).length - (line.match(/\}/g) || []).length;
        let searchLine = lineNum + 1;

        // Search through the block
        while (searchLine < lines.length && braceCount > 0) {
          const searchText = lines[searchLine];
          braceCount += (searchText.match(/\{/g) || []).length - (searchText.match(/\}/g) || []).length;

          if (searchText.includes('return')) {
            blockHasReturn = true;
            break;
          }
          searchLine++;
        }

        if (!blockHasReturn) {
          this.warnings.push({
            line: lineNum + 1,
            column: 1,
            message: 'State transition block should include return statement',
            severity: 'warning'
          });
        }
      } else {
        // No braces and no return on same line - check nearby lines
        let hasReturnNearby = false;
        for (let i = lineNum; i < Math.min(lineNum + 3, lines.length); i++) {
          if (lines[i].includes('return')) {
            hasReturnNearby = true;
            break;
          }
        }

        if (!hasReturnNearby) {
          this.warnings.push({
            line: lineNum + 1,
            column: 1,
            message: 'State transition should include return statement',
            severity: 'warning'
          });
        }
      }
    }
  });

  // Timer analysis: only warn if timerTick without timerExpired
  if (hasTimerTick && !hasTimerExpired && timerTickLines.length > 0) {
    this.warnings.push({
      line: timerTickLines[0],
      column: 1,
      message: 'timerTick used but no timerExpired check found in script',
      severity: 'warning'
    });
  }
}

  checkBasicSyntax(code) {
    const lines = code.split('\n');

    lines.forEach((line, lineNum) => {
      const trimmed = line.trim();

      // Skip empty lines and comments
      if (!trimmed || trimmed.startsWith('#')) {
        return;
      }

      // Check for unterminated strings on single line
      if (this.hasUnterminatedString(line)) {
        this.errors.push({
          line: lineNum + 1,
          column: line.length,
          message: 'Unterminated string',
          severity: 'error'
        });
      }
    });
  }

  checkBraceBalance(code) {
    let braceStack = [];
    let braceCount = 0;
    const lines = code.split('\n');

    lines.forEach((line, lineNum) => {
      let inString = false;
      let escapeNext = false;

      for (let i = 0; i < line.length; i++) {
        const char = line[i];
        const actualLineNum = lineNum + 1;

        if (escapeNext) {
          escapeNext = false;
          continue;
        }

        if (char === '\\') {
          escapeNext = true;
          continue;
        }

        if (char === '"' && !escapeNext) {
          inString = !inString;
          continue;
        }

        if (inString) {
          continue;
        }

        if (char === '{') {
          braceCount++;
          braceStack.push({ line: actualLineNum, column: i + 1, type: 'open' });
        } else if (char === '}') {
          braceCount--;
          if (braceCount < 0) {
            this.errors.push({
              line: actualLineNum,
              column: i + 1,
              message: 'Unmatched closing brace',
              severity: 'error'
            });
            braceCount = 0;
          } else {
            braceStack.pop();
          }
        }
      }
    });

    if (braceCount > 0) {
      const lastOpen = braceStack[braceStack.length - 1];
      this.errors.push({
        line: lastOpen?.line || lines.length,
        column: lastOpen?.column || 1,
        message: `${braceCount} unmatched opening brace${braceCount > 1 ? 's' : ''}`,
        severity: 'error'
      });
    }
  }

  checkQuoteBalance(code) {
    const lines = code.split('\n');
    let inMultiLineString = false;

    lines.forEach((line, lineNum) => {
      let quoteCount = 0;
      let escapeNext = false;

      for (let char of line) {
        if (escapeNext) {
          escapeNext = false;
          continue;
        }

        if (char === '\\') {
          escapeNext = true;
          continue;
        }

        if (char === '"') {
          quoteCount++;
        }
      }

      if (quoteCount % 2 !== 0) {
        if (!inMultiLineString) {
          inMultiLineString = true;
        } else {
          inMultiLineString = false;
        }
      }
    });

    if (inMultiLineString) {
      this.errors.push({
        line: lines.length,
        column: 1,
        message: 'Unterminated multi-line string',
        severity: 'error'
      });
    }
  }

  hasUnterminatedString(line) {
    let quoteCount = 0;
    let escapeNext = false;

    for (let char of line) {
      if (escapeNext) {
        escapeNext = false;
        continue;
      }

      if (char === '\\') {
        escapeNext = true;
        continue;
      }

      if (char === '"') {
        quoteCount++;
      }
    }

    return quoteCount % 2 !== 0;
  }

  getSummary() {
    const errorCount = this.errors.length;
    const warningCount = this.warnings.length;

    if (errorCount === 0 && warningCount === 0) {
      return 'No issues found';
    }

    const parts = [];
    if (errorCount > 0) {
      parts.push(`${errorCount} error${errorCount > 1 ? 's' : ''}`);
    }
    if (warningCount > 0) {
      parts.push(`${warningCount} warning${warningCount > 1 ? 's' : ''}`);
    }

    return parts.join(', ');
  }

  static quickValidate(code) {
    const linter = new TclLinter();
    const result = linter.lint(code);
    return result.isValid;
  }

  static getErrors(code) {
    const linter = new TclLinter();
    const result = linter.lint(code);
    return result.errors;
  }
}

export const validateTclCode = (code) => {
  return TclLinter.quickValidate(code);
};

export const getTclErrors = (code) => {
  return TclLinter.getErrors(code);
};

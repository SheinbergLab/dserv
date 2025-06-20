#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/fl_ask.H>
#include <cstring>
#include <cctype>
#include <vector>
#include <set>
#include <string>

#include "TclEditor.h"

// --- Style Definitions ---
enum TclStyleTypes {
    STYLE_NORMAL = 'A',
    STYLE_KEYWORD = 'B',
    STYLE_COMMENT = 'C',
    STYLE_STRING_DQ = 'D',     // Double-quoted string
    STYLE_VARIABLE = 'E',
    STYLE_COMMAND_SUBST = 'F', // Square bracket command substitution
    STYLE_NUMBER = 'G',
    // If specific styling for '{' and '}' characters is desired later,
    // a new style like STYLE_BRACE_DELIMITER could be added.
};

// Style table for syntax highlighting
Fl_Text_Display::Style_Table_Entry styletable[] = {
    // FONT COLOR      FONT FACE   FONT SIZE
    { FL_BLACK,        FL_COURIER, 14 },        // A - Plain
    { FL_BLUE,         FL_COURIER, 14 },        // B - Keywords  
    { FL_RED,          FL_COURIER_ITALIC, 14 }, // C - Comments
    { FL_DARK_GREEN,   FL_COURIER, 14 },        // D - Strings
    { FL_DARK_MAGENTA, FL_COURIER, 14 },        // E - Variables
    { FL_DARK_CYAN,    FL_COURIER, 14 },        // F - Command subst
    { FL_DARK_RED,     FL_COURIER, 14 },        // G - Numbers
    { FL_DARK_BLUE,    FL_COURIER_BOLD, 14 }    // H - Highlighting for search
    
};

// Tcl keywords
const std::set<std::string> tcl_keywords = {
    // Control flow
    "if", "then", "else", "elseif", "endif",
    "for", "foreach", "while", "break", "continue",
    "switch", "case", "default",
    
    // Procedure and namespace
    "proc", "return", "namespace", "variable", "global", "upvar",
    "uplevel", "apply", "coroutine", "yield", "yieldto",
    
    // Error handling
    "try", "trap", "finally", "throw", "catch", "error",
    
    // Variable and array operations
    "set", "unset", "array", "dict", "list", "lappend", "linsert",
    "lreplace", "lsearch", "lsort", "llength", "lindex", "lrange",
    "join", "split", "concat",
    
    // String operations
    "string", "regexp", "regsub", "scan", "format", "subst",
    
    // File and I/O
    "open", "close", "read", "write", "puts", "gets", "flush",
    "seek", "tell", "eof", "file", "glob", "pwd", "cd",
    "exec", "eval", "source", "load", "package",
    
    // Channel operations
    "chan", "socket", "fileevent", "fconfigure", "fcopy",
    
    // Time and events
    "after", "update", "vwait", "time", "clock",
    
    // Miscellaneous
    "expr", "incr", "append", "lset", "binary", "encoding",
    "exit", "rename", "info", "history", "unknown", "auto_load",
    "auto_import", "auto_qualify", "auto_mkindex",
    
    // Tcl 9 specific
    "tailcall", "nextto", "self", "my", "oo::class", "oo::object",
    "oo::define", "method", "constructor", "destructor", "filter",
    "mixin", "forward", "unexport", "export", "create", "new",
    "destroy", "copy", "configure", "cget"
};

// --- Tcl Syntax Parsing Function ---
// text: The input Tcl code.
// style_out: The output buffer where style characters will be written.
// length: The length of the text and style_out buffers.
void parse_tcl_syntax(const char* text, char* style_out, int length) {
    if (!text || !style_out || length <= 0) {
        if (length > 0 && style_out) style_out[0] = '\0'; // Ensure empty style if no text
        return;
    }

    int i = 0;
    while (i < length) {
        // Default style for the current character
        style_out[i] = STYLE_NORMAL;

        // 1. Comments: '#' to end of line
        if (text[i] == '#') {
            while (i < length && text[i] != '\n') {
                style_out[i] = STYLE_COMMENT;
                i++;
            }
            continue; 
        }

        // 2. Whitespace
        if (isspace(text[i])) {
            style_out[i] = STYLE_NORMAL; 
            i++;
            continue;
        }

        // 3. Strings (double-quoted: "...")
        if (text[i] == '"') {
            style_out[i++] = STYLE_STRING_DQ; 
            while (i < length) {
                style_out[i] = STYLE_STRING_DQ;
                if (text[i] == '\\' && i + 1 < length) { 
                    style_out[++i] = STYLE_STRING_DQ;    
                } else if (text[i] == '"') {
                    break; 
                }
                i++;
            }
            if (i < length) i++; 
            continue;
        }

        // 4. Brace-quoted strings/blocks: {...}
        // This rule has been modified. Previously, it would style the entire
        // content of a {...} block uniformly and skip internal parsing.
        // Now, '{' and '}' are treated as regular characters (typically styled STYLE_NORMAL
        // by the fall-through logic at the end of this loop), allowing the parser
        // to step into the block and apply other syntax rules to its content.
        // This is crucial for highlighting syntax inside procedure bodies, if/while blocks, etc.
        // No explicit code is needed here for '{' or '}' if they are to be STYLE_NORMAL,
        // as they will not match subsequent specific rules and fall to the default.
        // If specific styling for '{' or '}' themselves were desired (e.g. a STYLE_BRACE_DELIMITER),
        // explicit checks like `if (text[i] == '{')` would be added here or before other rules.

        // 5. Variables: $varName or ${varName}
        // (Note: Rule numbering shifted due to effective removal of old Rule 4 logic)
        if (text[i] == '$') {
            style_out[i++] = STYLE_VARIABLE; 
            if (i < length && text[i] == '{') { 
                style_out[i++] = STYLE_VARIABLE; 
                while (i < length && text[i] != '}') {
                    style_out[i++] = STYLE_VARIABLE; 
                }
                if (i < length && text[i] == '}') {
                    style_out[i++] = STYLE_VARIABLE; 
                }
            } else { 
                while (i < length && (isalnum(text[i]) || text[i] == '_' || text[i] == ':')) {
                    style_out[i++] = STYLE_VARIABLE;
                }
            }
            continue;
        }

        // 6. Command Substitution: [...]
        // Simplified: handles basic nesting.
        if (text[i] == '[') {
            int subst_start_idx = i;
            int bracket_level = 0;
            int current_scan_idx = i;

            while (current_scan_idx < length) {
                if (text[current_scan_idx] == '[') {
                    bracket_level++;
                } else if (text[current_scan_idx] == ']') {
                    bracket_level--;
                    if (bracket_level == 0) break; 
                }
                current_scan_idx++;
            }
            
            if (bracket_level == 0 && current_scan_idx < length) { 
                for (int k = subst_start_idx; k <= current_scan_idx; ++k) {
                    style_out[k] = STYLE_COMMAND_SUBST;
                }
                i = current_scan_idx + 1; 
            } else { 
                style_out[subst_start_idx] = STYLE_NORMAL;
                i = subst_start_idx + 1;
            }
            continue;
        }

        // 7. Keywords and Identifiers/Commands
        if (isalpha(text[i]) || text[i] == '_') {
            int token_start_idx = i;
            std::string current_word;
            while (i < length && (isalnum(text[i]) || text[i] == '_' || text[i] == ':' || text[i] == '.')) {
                current_word += text[i];
                i++;
            }

            if (tcl_keywords.count(current_word)) {
                for (int k = token_start_idx; k < i; ++k) {
                    style_out[k] = STYLE_KEYWORD;
                }
            } else {
                for (int k = token_start_idx; k < i; ++k) {
                    style_out[k] = STYLE_NORMAL; 
                }
            }
            continue;
        }
        
        // 8. Numbers (basic integer and float detection)
        if (isdigit(text[i]) || (text[i] == '.' && i + 1 < length && isdigit(text[i+1]))) {
            int num_start_idx = i;
            bool has_decimal = (text[i] == '.');
            bool has_exponent = false;

            if (text[i] == '.') i++; 
            
            while (i < length) {
                if (isdigit(text[i])) {
                    // part of number
                } else if (text[i] == '.' && !has_decimal && !has_exponent) {
                    has_decimal = true;
                } else if ((text[i] == 'e' || text[i] == 'E') && !has_exponent) {
                    has_exponent = true;
                    if (i + 1 < length && (text[i+1] == '+' || text[i+1] == '-')) {
                        i++; 
                    }
                } else {
                    break; 
                }
                i++;
            }
            if (i > num_start_idx && (isdigit(text[num_start_idx]) || (text[num_start_idx] == '.' && i > num_start_idx +1 && isdigit(text[num_start_idx+1])) )) {
                 for (int k = num_start_idx; k < i; ++k) {
                    style_out[k] = STYLE_NUMBER;
                }
            } else { 
                i = num_start_idx; 
                style_out[i] = STYLE_NORMAL;
                i++;
            }
            continue;
        }

        // If nothing else matched, it's a normal character (e.g., operator, punctuation like '{' or '}')
        // style_out[i] is already STYLE_NORMAL by default from the top of the loop.
        i++;
    }
    style_out[length] = '\0'; 
}



// --- Style Update Callback ---
void style_update_tcl(int pos, int nInserted, int nDeleted, int nRestyled,
		      const char* deletedText, void* cbArg) {
  //  Fl_Text_Editor* editor = static_cast<Fl_Text_Editor*>(cbArg);
  TclEditor* editor = static_cast<TclEditor*>(cbArg);
  Fl_Text_Buffer* textBuffer = editor->buffer(); 
  Fl_Text_Buffer* stylebuf = editor->style_buffer();

  if (editor->track_modifications()) {
    if (nInserted > 0 || nDeleted > 0) {
      editor->mark_modified();
    }
  }
	
  if (!stylebuf) {
    return;
  }
  
  int text_len = textBuffer->length();
  if (text_len == 0) {
    stylebuf->text(""); 
    editor->redisplay_range(0, 0); 
    return;
  }
  
  char* current_text = textBuffer->text_range(0, text_len);
  if (!current_text) {
    return; 
  }
  
  char* parsed_styles_temp = (char*)malloc(text_len + 1); 
  if (!parsed_styles_temp) {
    free(current_text);
    return;
    }
  
  parse_tcl_syntax(current_text, parsed_styles_temp, text_len);
  parsed_styles_temp[text_len] = '\0'; 
  
  stylebuf->text(parsed_styles_temp);
  
  free(parsed_styles_temp);
  free(current_text);
  
  editor->redisplay_range(0, text_len);
}

void initial_styling(TclEditor *editor)
{
  style_update_tcl(0, editor->buffer()->length(), 0, 0, nullptr, editor);
}

void configure_editor(TclEditor *editor, Fl_Text_Buffer *buffer)
{
  Fl_Text_Buffer* stylebuf = new Fl_Text_Buffer();
  editor->set_parent_tab(editor->parent());
  editor->buffer(buffer);
  editor->highlight_data(editor->stylebuf, styletable, 
			 sizeof(styletable) / sizeof(styletable[0]),
			 'A', NULL, NULL);
  buffer->add_modify_callback(style_update_tcl, editor);

}


// Auto-indent function
void handle_auto_indent(TclEditor* editor) {
    // Get current line
    int pos = editor->insert_position();
    Fl_Text_Buffer* buf = editor->buffer();
    int line_start = buf->line_start(pos);
    int line_end = buf->line_end(pos);
    
    char* line_text = buf->text_range(line_start, line_end);
    
    // Count leading whitespace
    int indent = 0;
    for (char* p = line_text; *p && (*p == ' ' || *p == '\t'); p++) {
        if (*p == '\t') indent += 4;
        else indent++;
    }
    
    // Check if line ends with opening brace
    bool needs_extra_indent = false;
    char* trimmed = line_text;
    while (*trimmed && isspace(*trimmed)) trimmed++;
    
    if (strlen(trimmed) > 0) {
        char* end = trimmed + strlen(trimmed) - 1;
        while (end > trimmed && isspace(*end)) end--;
        if (*end == '{') {
            needs_extra_indent = true;
        }
    }
    
    // Insert newline and indentation
    editor->insert("\n");
    
    // Add base indentation
    for (int i = 0; i < indent; i++) {
        editor->insert(" ");
    }
    
    // Add extra indentation if needed
    if (needs_extra_indent) {
        editor->insert("    ");  // 4 spaces
    }
    
    free(line_text);
}

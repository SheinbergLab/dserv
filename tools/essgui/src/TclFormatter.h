#include <string>
#include <vector>
#include <sstream>
#include <regex>
#include <cctype>

class TclFormatter {
public:
  static std::string format_tcl_code(const std::string& code,
				     int indent_size = 4) {
       std::vector<std::string> lines = split_lines(code);
        std::vector<std::string> formatted_lines;
        int indent_level = 0;
        int continuation_indent = 0; // Extra indent for continuation lines
        bool in_continuation = false;

for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            std::string trimmed = trim(line);
            
            // Skip empty lines but preserve them
            if (trimmed.empty()) {
                formatted_lines.push_back("");
                continue;
            }
            
            // Handle comments - they follow continuation rules too
            if (!trimmed.empty() && trimmed[0] == '#') {
                int comment_indent = in_continuation ? indent_level + continuation_indent : indent_level;
                formatted_lines.push_back(std::string(comment_indent * 4, ' ') + trimmed);
                
                // Comments don't affect continuation state
                bool ends_with_backslash = !trimmed.empty() && trimmed.back() == '\\';
                if (!in_continuation && ends_with_backslash) {
                    in_continuation = true;
                    continuation_indent = 1; // Standard continuation indent
                } else if (in_continuation && !ends_with_backslash) {
                    in_continuation = false;
                    continuation_indent = 0;
                }
                continue;
            }
            
            // Check for line continuation
            bool ends_with_backslash = !trimmed.empty() && trimmed.back() == '\\';
            
            // Calculate current line indent
            int current_indent = indent_level;
            
            // If we're in a continuation, add extra indent
            if (in_continuation) {
                current_indent += continuation_indent;
            }
            
            // Count braces for indent calculation (always needed)
            int open_braces = count_unquoted_char(trimmed, '{');
            int close_braces = count_unquoted_char(trimmed, '}');
            
            // Adjust current indent for special cases (only if not in continuation)
            if (!in_continuation) {
                // Adjust for closing braces at start of line
                if (!trimmed.empty() && trimmed[0] == '}') {
                    current_indent = std::max(0, indent_level - 1);
                }
                
                // Handle special keywords (these usually align with their parent)
                if (starts_with_keyword(trimmed, {"else", "elseif", "catch"})) {
                    current_indent = std::max(0, indent_level - 1);
                }
            }
            
            // Format the line with current indentation
            std::string formatted_line = format_line(trimmed, current_indent);
            formatted_lines.push_back(formatted_line);
            
            // Update continuation state and handle brace counting
            if (!in_continuation && ends_with_backslash) {
                // Starting a new continuation
                in_continuation = true;
                continuation_indent = get_continuation_indent(trimmed);
                
                // Count braces on the first line of continuation too
                indent_level += (open_braces - close_braces);
                if (indent_level < 0) {
                    indent_level = 0;
                }
            } else if (in_continuation && !ends_with_backslash) {
                // Ending continuation - count braces on the final line
                open_braces = count_unquoted_char(trimmed, '{');
                close_braces = count_unquoted_char(trimmed, '}');
                indent_level += (open_braces - close_braces);
                if (indent_level < 0) {
                    indent_level = 0;
                }
                
                in_continuation = false;
                continuation_indent = 0;
            } else if (in_continuation && ends_with_backslash) {
                // Middle of continuation - still count braces
                open_braces = count_unquoted_char(trimmed, '{');
                close_braces = count_unquoted_char(trimmed, '}');
                indent_level += (open_braces - close_braces);
                if (indent_level < 0) {
                    indent_level = 0;
                }
            } else if (!in_continuation && !ends_with_backslash) {
                // Normal line (not in continuation) - count braces
                indent_level += (open_braces - close_braces);
                if (indent_level < 0) {
                    indent_level = 0;
                }
            }
            
            // Debug output (remove this after testing)
            #ifdef DEBUG_FORMATTER
            printf("Line %zu: '%s' -> indent=%d, continuation=%s, cont_indent=%d, open=%d, close=%d, next_level=%d\n", 
                   i, trimmed.c_str(), current_indent, in_continuation ? "yes" : "no", 
                   continuation_indent, open_braces, close_braces, indent_level);
            #endif
        }
        
        return join_lines(formatted_lines);
    }	

    // Determine appropriate continuation indent based on line content
    static int get_continuation_indent(const std::string& line) {
        // For most cases, use standard continuation indent
        int base_indent = 1;
        
        // Special cases for different TCL constructs
        if (starts_with_keyword(line, {"if", "while", "for", "foreach"})) {
            // Control structures get a bit more indent to align with condition
            return base_indent + 1;
        } else if (starts_with_keyword(line, {"set", "puts", "return"})) {
            // Simple commands get standard continuation
            return base_indent;
        } else if (line.find("proc ") != std::string::npos) {
            // Procedure definitions
            return base_indent + 1;
        } else if (line.find("[") != std::string::npos) {
            // Command substitutions - align nicely
            return base_indent;
        }
        
        return base_indent;
    }

    // Make helper functions public for debugging
    static std::vector<std::string> split_lines(const std::string& text) {
        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;
        
        while (std::getline(stream, line)) {
            lines.push_back(line);
        }
        
        return lines;
    }
    
    static std::string join_lines(const std::vector<std::string>& lines) {
        std::ostringstream result;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i > 0) result << '\n';
            result << lines[i];
        }
        return result.str();
    }
    
    static std::string trim(const std::string& str) {
        size_t start = str.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        
        size_t end = str.find_last_not_of(" \t\r\n");
        return str.substr(start, end - start + 1);
    }
    
    static bool starts_with_keyword(const std::string& line, 
                                   const std::vector<std::string>& keywords) {
        for (const auto& keyword : keywords) {
            std::string pattern = "^\\s*" + keyword + "\\b";
            if (std::regex_search(line, std::regex(pattern))) {
                return true;
            }
        }
        return false;
    }
    
    static int count_unquoted_char(const std::string& str, char target) {
        int count = 0;
        bool in_quotes = false;
        bool in_braces = false;
        int brace_depth = 0;
        bool escaped = false;
        
        #ifdef DEBUG_FORMATTER
        printf("  Counting '%c' in: '%s'\n", target, str.c_str());
        #endif
        
        for (size_t i = 0; i < str.length(); ++i) {
            char c = str[i];
            
            if (escaped) {
                escaped = false;
                continue;
            }
            
            if (c == '\\') {
                escaped = true;
                continue;
            }
            
            if (c == '"' && !in_braces) {
                in_quotes = !in_quotes;
                continue;
            }
            
            // Track brace depth for nested braces
            if (!in_quotes) {
                if (c == '{') {
                    brace_depth++;
                    in_braces = (brace_depth > 0);
                } else if (c == '}') {
                    brace_depth--;
                    in_braces = (brace_depth > 0);
                }
            }
            
            // Only count if we're not inside quoted strings or nested braces
            // (for brace counting, we still want to count the outermost braces)
            if (!in_quotes && c == target) {
                if (target == '{' || target == '}' || !in_braces) {
                    count++;
                    #ifdef DEBUG_FORMATTER
                    printf("    Found '%c' at position %zu (count now %d)\n", target, i, count);
                    #endif
                }
            }
        }
        
        #ifdef DEBUG_FORMATTER
        printf("  Final count of '%c': %d\n", target, count);
        #endif
        return count;
    }
  
  
  static std::string format_line(const std::string& line,
				 int indent_level,
				 int indent_size = 4) {
    std::string indent(indent_level * indent_size, ' ');
    std::string normalized;
    bool in_quotes = false;
    bool escaped = false;
    char prev_char = '\0';
    
    // First normalize whitespace in the line content
    for (char c : line) {
      if (escaped) {
	normalized += c;
	escaped = false;
	prev_char = c;
	continue;
      }
      
      if (c == '\\') {
	normalized += c;
	escaped = true;
	prev_char = c;
	continue;
      }
      
      if (c == '"') {
	in_quotes = !in_quotes;
	normalized += c;
	prev_char = c;
	continue;
      }
      
      if (in_quotes) {
	normalized += c;
	prev_char = c;
      } else {
	// Handle spacing outside quotes
	if (std::isspace(c)) {
	  if (prev_char != ' ' && prev_char != '\0') {
	    normalized += ' ';
	    prev_char = ' ';
	  }
	} else {
	  normalized += c;
	  prev_char = c;
	}
      }
    }
    
    // Trim trailing whitespace only
    size_t end = normalized.find_last_not_of(" \t");
    if (end != std::string::npos) {
      normalized = normalized.substr(0, end + 1);
    }
    
    // Return indent + normalized content
    return indent + normalized;
  }
  
private:
  // Keep some functions private if not needed elsewhere
};

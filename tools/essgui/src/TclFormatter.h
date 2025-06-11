#include <string>
#include <vector>
#include <sstream>
#include <regex>
#include <cctype>

class TclFormatter {
public:
    // Structure to hold indentation context
    struct IndentContext {
        int base_indent_level;
        int continuation_indent;
        bool in_continuation;
        int running_bracket_depth;
    };
    
    // Calculate the proper indent level for a given line in context
    static int calculate_line_indent(const std::vector<std::string>& lines, 
                                   int current_line_index,
                                   int indent_size = 4) {
        if (current_line_index < 0 || current_line_index >= lines.size()) {
            return 0;
        }
        
        // Process all lines up to (but not including) the current line
        // to determine the proper context
        int indent_level = 0;
        int continuation_indent = 0;
        bool in_continuation = false;
        int running_bracket_depth = 0;
        
        for (int i = 0; i < current_line_index; ++i) {
            const std::string& line = lines[i];
            std::string trimmed = trim(line);
            
            if (trimmed.empty() || trimmed[0] == '#') {
                // Skip empty lines and comments for context building
                continue;
            }
            
            bool ends_with_backslash = !trimmed.empty() && trimmed.back() == '\\';
            int open_braces = count_unquoted_char(trimmed, '{');
            int close_braces = count_unquoted_char(trimmed, '}');
            int open_brackets = count_opening_brackets(trimmed);
            int close_brackets = count_closing_brackets(trimmed);
            
            // Update state based on line type
            if (!in_continuation && ends_with_backslash) {
                in_continuation = true;
                running_bracket_depth = open_brackets - close_brackets;
                indent_level += (open_braces - close_braces);
            } else if (in_continuation && ends_with_backslash) {
                running_bracket_depth += (open_brackets - close_brackets);
                if (running_bracket_depth < 0) running_bracket_depth = 0;
                indent_level += (open_braces - close_braces);
            } else if (in_continuation && !ends_with_backslash) {
                indent_level += (open_braces - close_braces);
                in_continuation = false;
                running_bracket_depth = 0;
            } else {
                indent_level += (open_braces - close_braces);
            }
            
            if (indent_level < 0) indent_level = 0;
        }
        
        // Now calculate indent for the current line
        std::string current_trimmed = trim(lines[current_line_index]);
        int current_indent = indent_level;
        
        if (in_continuation) {
            continuation_indent = 1 + running_bracket_depth;
            current_indent += continuation_indent;
        }
        
        // Special adjustments for current line
        if (!in_continuation && !current_trimmed.empty()) {
            if (current_trimmed[0] == '}') {
                current_indent = std::max(0, indent_level - 1);
            }
            if (starts_with_keyword(current_trimmed, {"else", "elseif", "catch"})) {
                current_indent = std::max(0, indent_level - 1);
            }
        }
        
        return current_indent * indent_size;
    }
    
    static std::string format_tcl_code(const std::string& code, int indent_size=4) {
        std::vector<std::string> lines = split_lines(code);
        std::vector<std::string> formatted_lines;
        int indent_level = 0;
        int continuation_indent = 0;
        bool in_continuation = false;
        int running_bracket_depth = 0;  // Track bracket depth throughout continuation
        std::string previous_line = "";
        
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            std::string trimmed = trim(line);
            
            // Skip empty lines but preserve them
            if (trimmed.empty()) {
                formatted_lines.push_back("");
                continue;
            }
            
            // Handle comments
            if (!trimmed.empty() && trimmed[0] == '#') {
                int comment_indent = in_continuation ? indent_level + continuation_indent : indent_level;
                formatted_lines.push_back(std::string(comment_indent * 4, ' ') + trimmed);
                
                bool ends_with_backslash = !trimmed.empty() && trimmed.back() == '\\';
                if (!in_continuation && ends_with_backslash) {
                    in_continuation = true;
                    running_bracket_depth = 0;
                    continuation_indent = 1;
                } else if (in_continuation && !ends_with_backslash) {
                    in_continuation = false;
                    continuation_indent = 0;
                    running_bracket_depth = 0;
                }
                continue;
            }
            
            // Check for line continuation
            bool ends_with_backslash = !trimmed.empty() && trimmed.back() == '\\';
            
            // Count braces and brackets in current line
            int open_braces = count_unquoted_char(trimmed, '{');
            int close_braces = count_unquoted_char(trimmed, '}');
            int open_brackets = count_opening_brackets(trimmed);
            int close_brackets = count_closing_brackets(trimmed);
            
            // Calculate current line indent
            int current_indent = indent_level;
            
            // If we're in a continuation, calculate the appropriate indent
            if (in_continuation) {
                // Base continuation indent is 1
                // Add 1 for each level of unclosed brackets
                continuation_indent = 1 + running_bracket_depth;
                current_indent += continuation_indent;
            }
            
            // Adjust current indent for special cases (only if not in continuation)
            if (!in_continuation) {
                // Adjust for closing braces at start of line
                if (!trimmed.empty() && trimmed[0] == '}') {
                    current_indent = std::max(0, indent_level - 1);
                }
                
                // Handle special keywords
                if (starts_with_keyword(trimmed, {"else", "elseif", "catch"})) {
                    current_indent = std::max(0, indent_level - 1);
                }
            }
            
            // Format the line with current indentation
            std::string formatted_line = format_line(trimmed, current_indent);
            formatted_lines.push_back(formatted_line);
            
            // Update state based on line type
            if (!in_continuation && ends_with_backslash) {
                // Starting a new continuation
                in_continuation = true;
                running_bracket_depth = open_brackets - close_brackets;
                
                // Count braces for overall indent level
                indent_level += (open_braces - close_braces);
                if (indent_level < 0) {
                    indent_level = 0;
                }
            } else if (in_continuation && ends_with_backslash) {
                // Continuing a continuation
                // Update running bracket depth for next line
                running_bracket_depth += (open_brackets - close_brackets);
                if (running_bracket_depth < 0) {
                    running_bracket_depth = 0;
                }
                
                // Count braces for overall indent level
                indent_level += (open_braces - close_braces);
                if (indent_level < 0) {
                    indent_level = 0;
                }
            } else if (in_continuation && !ends_with_backslash) {
                // Ending continuation
                indent_level += (open_braces - close_braces);
                if (indent_level < 0) {
                    indent_level = 0;
                }
                
                in_continuation = false;
                continuation_indent = 0;
                running_bracket_depth = 0;
            } else if (!in_continuation && !ends_with_backslash) {
                // Normal line (not in continuation)
                indent_level += (open_braces - close_braces);
                if (indent_level < 0) {
                    indent_level = 0;
                }
            }
            
            // Update previous line for context
            previous_line = trimmed;
            
            #ifdef DEBUG_FORMATTER
            printf("Line %zu: '%s' -> indent=%d, continuation=%s, cont_indent=%d, bracket_depth=%d, open_b=%d, close_b=%d\n", 
                   i, trimmed.c_str(), current_indent, in_continuation ? "yes" : "no", 
                   continuation_indent, running_bracket_depth, open_brackets, close_brackets);
            #endif
        }
        
        return join_lines(formatted_lines);
    }
    
    // Count opening brackets in a line (ignoring those in quotes)
    static int count_opening_brackets(const std::string& line) {
        int count = 0;
        bool in_quotes = false;
        bool escaped = false;
        
        for (char c : line) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                in_quotes = !in_quotes;
                continue;
            }
            if (!in_quotes && c == '[') {
                count++;
            }
        }
        return count;
    }
    
    // Count closing brackets in a line (ignoring those in quotes)
    static int count_closing_brackets(const std::string& line) {
        int count = 0;
        bool in_quotes = false;
        bool escaped = false;
        
        for (char c : line) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                in_quotes = !in_quotes;
                continue;
            }
            if (!in_quotes && c == ']') {
                count++;
            }
        }
        return count;
    }

    // Helper functions
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
            if (!in_quotes && c == target) {
                if (target == '{' || target == '}' || !in_braces) {
                    count++;
                }
            }
        }
        
        return count;
    }
    
    static std::string format_line(const std::string& line, int indent_level, int indent_size=4) {
        std::string indent(indent_level * 4, ' ');
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
};


#include <string>
#include <vector>
#include <sstream>
#include <regex>
#include <cctype>

class TclFormatter {
public:
    static std::string format_tcl_code(const std::string& code) {
        std::vector<std::string> lines = split_lines(code);
        std::vector<std::string> formatted_lines;
        int indent_level = 0;
        
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string& line = lines[i];
            std::string trimmed = trim(line);
            
            // Skip empty lines
            if (trimmed.empty()) {
                formatted_lines.push_back("");
                continue;
            }
            
            // Handle comments
            if (!trimmed.empty() && trimmed[0] == '#') {
                formatted_lines.push_back(std::string(indent_level * 4, ' ') + trimmed);
                continue;
            }
            
            // Check for line continuation
            bool ends_with_backslash = !trimmed.empty() && trimmed.back() == '\\';
            
            // Calculate current line indent
            int current_indent = indent_level;
            
            // Count braces BEFORE adjusting indent
            int open_braces = count_unquoted_char(trimmed, '{');
            int close_braces = count_unquoted_char(trimmed, '}');
            
            // Adjust for closing braces at start of line
            if (!trimmed.empty() && trimmed[0] == '}') {
                current_indent = std::max(0, indent_level - 1);
            }
            
            // Handle special keywords (these usually align with their parent)
            if (starts_with_keyword(trimmed, {"else", "elseif", "catch"})) {
                current_indent = std::max(0, indent_level - 1);
            }
            
            // Format the line with current indentation
            std::string formatted_line = format_line(trimmed, current_indent);
            formatted_lines.push_back(formatted_line);
            
            // Update indent level for NEXT line (not current)
            if (!ends_with_backslash) {
                indent_level += (open_braces - close_braces);
                if (indent_level < 0) {
                    indent_level = 0;
                }
            }
            
            // Debug output (remove this after testing)
            #ifdef DEBUG_FORMATTER
            printf("Line %zu: '%s' -> indent=%d, open=%d, close=%d, next_level=%d\n", 
                   i, trimmed.c_str(), current_indent, open_braces, close_braces, indent_level);
            #endif
        }
        
        return join_lines(formatted_lines);
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
            std::string pattern = "^\\s*" + keyword + "\\s";
            if (std::regex_search(line, std::regex(pattern))) {
                return true;
            }
        }
        return false;
    }
    
    static int count_unquoted_char(const std::string& str, char target) {
        int count = 0;
        bool in_quotes = false;
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
            
            if (c == '"') {
                in_quotes = !in_quotes;
                continue;
            }
            
            // Only skip counting if we're inside quoted strings
            if (!in_quotes && c == target) {
                count++;
            }
        }
        
        return count;
    }
    
    static std::string format_line(const std::string& line, int indent_level) {
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

private:
    // Keep some functions private if not needed elsewhere
};

// Usage example for FLTK integration
class TclFormatterFLTK {
public:
    static void format_editor_text(Fl_Text_Buffer* buffer) {
        char* text = buffer->text();
        std::string code(text);
        free(text);
        
        std::string formatted = TclFormatter::format_tcl_code(code);
        buffer->text(formatted.c_str());
    }
    
    // Alternative that preserves cursor position
    static void format_editor_text_preserve_cursor(Fl_Text_Buffer* buffer, 
                                                   Fl_Text_Editor* editor) {
        int cursor_pos = editor->insert_position();
        char* text = buffer->text();
        std::string code(text);
        free(text);
        
        std::string formatted = TclFormatter::format_tcl_code(code);
        buffer->text(formatted.c_str());
        
        // Attempt to preserve cursor position (approximate)
        int new_pos = std::min(cursor_pos, static_cast<int>(formatted.length()));
        editor->insert_position(new_pos);
    }
    
    // Debug version to help troubleshoot
    static void debug_format_editor_text(Fl_Text_Buffer* buffer) {
        char* text = buffer->text();
        std::string code(text);
        free(text);
        
        printf("Original code:\n'%s'\n", code.c_str());
        printf("=== Testing format_line function ===\n");
        
        // Test format_line directly
        std::string test_line = "puts \"hello world\"";
        for (int i = 0; i < 3; ++i) {
            std::string result = TclFormatter::format_line(test_line, i);
            printf("format_line('%s', %d) = '%s'\n", test_line.c_str(), i, result.c_str());
        }
        
        printf("=== Formatting ===\n");
        
        // Let's debug step by step
        std::vector<std::string> lines = TclFormatter::split_lines(code);
        printf("Split into %zu lines:\n", lines.size());
        for (size_t i = 0; i < lines.size(); ++i) {
            printf("Line %zu: '%s'\n", i, lines[i].c_str());
        }
        
        std::string formatted = TclFormatter::format_tcl_code(code);
        
        printf("Final formatted code:\n'%s'\n", formatted.c_str());
        printf("=== Line by line ===\n");
        std::vector<std::string> formatted_lines = TclFormatter::split_lines(formatted);
        for (size_t i = 0; i < formatted_lines.size(); ++i) {
            printf("Formatted line %zu: '%s' (length: %zu)\n", i, formatted_lines[i].c_str(), formatted_lines[i].length());
        }
        
        buffer->text(formatted.c_str());
    }
    
    // Make split_lines public for debugging
    static std::vector<std::string> split_lines(const std::string& text) {
        return TclFormatter::split_lines(text);
    }
};

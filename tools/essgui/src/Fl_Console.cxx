#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include "Fl_Console.hpp"

Fl_Console::Fl_Console(int X, int Y, int W, int H, const char *L)
    : Fl_Terminal(X, Y, W, H, L),
      mode(MODE_NORMAL),
      cursor_pos(0),
      prompt("$ "),
      completion_index(0),
      original_cursor(0),
      history_index(-1),
      max_history_size(1000),
      history_file("history.txt"),
      process_cb(nullptr),
      process_cb_data(nullptr)
{
    ansi(true);  // Enable ANSI escape sequences
    init_console();
}

Fl_Console::~Fl_Console() {
    save_history(history_file);
}

void Fl_Console::init_console() {
    load_history(history_file);
    reset_to_prompt();
}

void Fl_Console::set_callback(PROCESS_CB cb, void *cbdata) {
    process_cb = cb;
    process_cb_data = cbdata;
}

void Fl_Console::set_prompt(const std::string& prompt_str) {
    prompt = prompt_str;
    refresh_line();
}

void Fl_Console::update_command_list(const std::vector<std::string>& commands) {
    available_commands = commands;
}

int Fl_Console::do_callback(char *str) {
    if (!process_cb) return 0;
    return process_cb(str, process_cb_data);
}

void Fl_Console::reset_to_prompt() {
    current_line.clear();
    cursor_pos = 0;
    mode = MODE_NORMAL;
    history_index = -1;
    refresh_line();
}

void Fl_Console::refresh_line() {
    // Move to beginning of current line and clear it
    append("\r\x1b[K");
    
    // Show prompt
    append("\x1b[1;37;49m");  // Bold white
    append(prompt.c_str());
    append("\x1b[0m");        // Reset
    
    // Show current line
    append(current_line.c_str());
    
    // Position cursor
    if (cursor_pos < current_line.length()) {
        size_t chars_to_move = current_line.length() - cursor_pos;
        for (size_t i = 0; i < chars_to_move; i++) {
            append("\x1b[D");  // Move cursor left
        }
    }
}

std::string Fl_Console::get_current_word() const {
    if (current_line.empty() || cursor_pos == 0) return "";
    
    // Find word boundaries
    size_t word_start = cursor_pos;
    while (word_start > 0 && current_line[word_start - 1] != ' ') {
        word_start--;
    }
    
    return current_line.substr(word_start, cursor_pos - word_start);
}

void Fl_Console::start_completion() {
    if (mode == MODE_COMPLETION) return;
    
    completions.clear();
    original_line = current_line;
    original_cursor = cursor_pos;
    
    std::string partial = get_current_word();
    
    // Find matching commands
    for (const auto& cmd : available_commands) {
        if (cmd.length() >= partial.length() && 
            cmd.substr(0, partial.length()) == partial) {
            completions.add(cmd);
        }
    }
    
    if (!completions.empty()) {
        mode = MODE_COMPLETION;
        completion_index = 0;
        cycle_completion();  // Show first completion
    }
}

void Fl_Console::cycle_completion() {
    if (mode != MODE_COMPLETION || completions.empty()) return;
    
    // Restore original line
    current_line = original_line;
    cursor_pos = original_cursor;
    
    if (completion_index < completions.size()) {
        // Replace the partial word with the completion
        std::string partial = get_current_word();
        if (!partial.empty()) {
            // Remove the partial word
            size_t word_start = cursor_pos;
            while (word_start > 0 && current_line[word_start - 1] != ' ') {
                word_start--;
            }
            
            // Replace with completion
            current_line.erase(word_start, cursor_pos - word_start);
            current_line.insert(word_start, completions[completion_index]);
            cursor_pos = word_start + completions[completion_index].length();
        }
    }
    
    refresh_line();
}

void Fl_Console::accept_completion() {
    if (mode == MODE_COMPLETION) {
        mode = MODE_NORMAL;
        completions.clear();
    }
}

void Fl_Console::cancel_completion() {
    if (mode == MODE_COMPLETION) {
        current_line = original_line;
        cursor_pos = original_cursor;
        mode = MODE_NORMAL;
        completions.clear();
        refresh_line();
    }
}

void Fl_Console::insert_char(char c) {
    if (mode == MODE_COMPLETION) {
        accept_completion();
    }
    
    current_line.insert(cursor_pos, 1, c);
    cursor_pos++;
    refresh_line();
}

void Fl_Console::backspace() {
    if (mode == MODE_COMPLETION) {
        cancel_completion();
        return;
    }
    
    if (cursor_pos > 0) {
        current_line.erase(cursor_pos - 1, 1);
        cursor_pos--;
        refresh_line();
    }
}

void Fl_Console::delete_char() {
    if (mode == MODE_COMPLETION) {
        accept_completion();
    }
    
    if (cursor_pos < current_line.length()) {
        current_line.erase(cursor_pos, 1);
        refresh_line();
    }
}

void Fl_Console::move_cursor_left() {
    if (mode == MODE_COMPLETION) {
        cancel_completion();
        return;
    }
    
    if (cursor_pos > 0) {
        cursor_pos--;
        refresh_line();
    }
}

void Fl_Console::move_cursor_right() {
    if (mode == MODE_COMPLETION) {
        cancel_completion();
        return;
    }
    
    if (cursor_pos < current_line.length()) {
        cursor_pos++;
        refresh_line();
    }
}

void Fl_Console::move_to_start() {
    if (mode == MODE_COMPLETION) {
        cancel_completion();
        return;
    }
    
    cursor_pos = 0;
    refresh_line();
}

void Fl_Console::move_to_end() {
    if (mode == MODE_COMPLETION) {
        accept_completion();
    }
    
    cursor_pos = current_line.length();
    refresh_line();
}

void Fl_Console::clear_line() {
    if (mode == MODE_COMPLETION) {
        cancel_completion();
    }
    
    current_line.clear();
    cursor_pos = 0;
    refresh_line();
}

void Fl_Console::delete_word() {
    if (mode == MODE_COMPLETION) {
        cancel_completion();
        return;
    }
    
    if (cursor_pos == 0) return;
    
    size_t start = cursor_pos;
    // Skip trailing spaces
    while (start > 0 && current_line[start - 1] == ' ') {
        start--;
    }
    // Find word boundary
    while (start > 0 && current_line[start - 1] != ' ') {
        start--;
    }
    
    current_line.erase(start, cursor_pos - start);
    cursor_pos = start;
    refresh_line();
}

void Fl_Console::add_to_history(const std::string& line) {
    if (line.empty()) return;
    
    // Don't add duplicate of last entry
    if (!history.empty() && history.back() == line) return;
    
    history.push_back(line);
    if (history.size() > max_history_size) {
        history.erase(history.begin());
    }
}

void Fl_Console::history_prev() {
    if (mode == MODE_COMPLETION) {
        cancel_completion();
        return;
    }
    
    if (history.empty()) return;
    
    if (history_index == -1) {
        history_index = history.size() - 1;
    } else if (history_index > 0) {
        history_index--;
    } else {
        return; // Already at oldest
    }
    
    current_line = history[history_index];
    cursor_pos = current_line.length();
    refresh_line();
}

void Fl_Console::history_next() {
    if (mode == MODE_COMPLETION) {
        cancel_completion();
        return;
    }
    
    if (history_index == -1) return;
    
    history_index++;
    if (history_index >= (int)history.size()) {
        history_index = -1;
        current_line.clear();
        cursor_pos = 0;
    } else {
        current_line = history[history_index];
        cursor_pos = current_line.length();
    }
    refresh_line();
}

void Fl_Console::execute_line() {
    if (mode == MODE_COMPLETION) {
        accept_completion();
    }
    
    append("\n");
    
    if (!current_line.empty()) {
        add_to_history(current_line);
        save_history(history_file);
        
        // Execute the command
        char* cmd = const_cast<char*>(current_line.c_str());
        do_callback(cmd);
    }
    
    reset_to_prompt();
}

void Fl_Console::handle_escape_sequence(char c) {
    escape_sequence += c;
    
    // Handle common escape sequences
    if (escape_sequence == "[A") {        // Up arrow
        history_prev();
        mode = MODE_NORMAL;
        escape_sequence.clear();
    } else if (escape_sequence == "[B") { // Down arrow
        history_next();
        mode = MODE_NORMAL;
        escape_sequence.clear();
    } else if (escape_sequence == "[C") { // Right arrow
        move_cursor_right();
        mode = MODE_NORMAL;
        escape_sequence.clear();
    } else if (escape_sequence == "[D") { // Left arrow
        move_cursor_left();
        mode = MODE_NORMAL;
        escape_sequence.clear();
    } else if (escape_sequence == "[H") { // Home
        move_to_start();
        mode = MODE_NORMAL;
        escape_sequence.clear();
    } else if (escape_sequence == "[F") { // End
        move_to_end();
        mode = MODE_NORMAL;
        escape_sequence.clear();
    } else if (escape_sequence == "[3~") { // Delete
        delete_char();
        mode = MODE_NORMAL;
        escape_sequence.clear();
    } else if (escape_sequence.length() > 4) {
        // Unknown sequence, give up
        mode = MODE_NORMAL;
        escape_sequence.clear();
    }
}

int Fl_Console::handle(int e) {
  switch (e) {
  case FL_PASTE:
    handle_paste(Fl::event_text());
    return 1;
    
case FL_KEYDOWN: {
    int key = Fl::event_key();
    const char *text = Fl::event_text();
    unsigned int state = Fl::event_state();
    
    if (Fl::event_alt()) return 0;
    
    // Handle Control key combinations first (Emacs bindings)
    if (state & FL_CTRL) {
        switch (key) {
        case 'a': case 'A':  // Ctrl+A - start of line
            move_to_start();
            return 1;
        case 'e': case 'E':  // Ctrl+E - end of line  
            move_to_end();
            return 1;
        case 'p': case 'P':  // Ctrl+P - previous history
            history_prev();
            return 1;
        case 'n': case 'N':  // Ctrl+N - next history
            history_next();
            return 1;
        case 'u': case 'U':  // Ctrl+U - clear line
            clear_line();
            return 1;
        case 'k': case 'K':  // Ctrl+K - delete to end
            current_line.erase(cursor_pos);
            refresh_line();
            return 1;
        case 'w': case 'W':  // Ctrl+W - delete word
            delete_word();
            return 1;
        case 'l': case 'L':  // Ctrl+L - clear screen
            clear();
            refresh_line();
            return 1;
        case 'f': case 'F':  // Ctrl+F - forward char
            move_cursor_right();
            return 1;
        case 'b': case 'B':  // Ctrl+B - backward char
            move_cursor_left();
            return 1;
        case 'd': case 'D':  // Ctrl+D - delete char
            delete_char();
            return 1;
        case 'h': case 'H':  // Ctrl+H - backspace
            backspace();
            return 1;
        }
    }
    
    // Handle arrow keys directly
    switch (key) {
    case FL_Up:
        history_prev();
        return 1;
    case FL_Down:
        history_next(); 
        return 1;
    case FL_Left:
        move_cursor_left();
        return 1;
    case FL_Right:
        move_cursor_right();
        return 1;
    case FL_Home:
        move_to_start();
        return 1;
    case FL_End:
        move_to_end();
        return 1;
    case FL_Delete:
        delete_char();
        return 1;
    }
    
    // Handle modifier keys (ignore them)
    switch (key) {
    case FL_Meta_L: case FL_Meta_R:
    case FL_Alt_L: case FL_Alt_R:
    case FL_Shift_L: case FL_Shift_R:
    case FL_Control_L: case FL_Control_R:
    case FL_Caps_Lock:
        return 0;
        
    case 'c': case 'C':
        if (Fl::event_state(FL_COMMAND)) {
            copy_to_clipboard();
            return 1;
        }
        break;
        
    case 'v': case 'V':
        if (Fl::event_state(FL_COMMAND)) {
            paste_from_clipboard();
            return 1;
        }
        break;
    }
    
    // Get the character
    char c = text && text[0] ? text[0] : 0;
    
    // Handle modes
    if (mode == MODE_ESCAPE_SEQUENCE) {
        handle_escape_sequence(c);
        return 1;
    }
    
    if (mode == MODE_COMPLETION) {
        switch (c) {
        case '\t':  // Tab - cycle through completions
            completion_index = (completion_index + 1) % (completions.size() + 1);
            if (completion_index == completions.size()) {
                // Back to original
                current_line = original_line;
                cursor_pos = original_cursor;
                refresh_line();
            } else {
                cycle_completion();
            }
            return 1;
            
        case 27:    // Escape - cancel completion
            cancel_completion();
            return 1;
            
        case '\r': case '\n':  // Enter - accept and execute
            accept_completion();
            execute_line();
            return 1;
            
        default:    // Any other key - accept completion and continue
            accept_completion();
            // Fall through to normal handling
            break;
        }
    }
    
    // Normal mode key handling
    switch (c) {
    case '\t':      // Tab - start completion
        start_completion();
        break;
        
    case '\r': case '\n':  // Enter
        execute_line();
        break;
        
    case 27:        // Escape - start escape sequence
        mode = MODE_ESCAPE_SEQUENCE;
        escape_sequence.clear();
        break;
        
    case 127: case 8:  // Backspace/Delete
        backspace();
        break;
        
    default:
        if (c >= 32 && c < 127) {  // Printable characters
            insert_char(c);
        }
        break;
    }
      
    return 1;
 }
  }
  return Fl_Terminal::handle(e);
}
  

void Fl_Console::resize(int X, int Y, int W, int H) {
    Fl_Terminal::resize(X, Y, W, H);
}

void Fl_Console::copy_to_clipboard() {
    int srow, scol, erow, ecol;
    std::string sel;
    
    if (get_selection(srow, scol, erow, ecol)) {
        for (int row = srow; row <= erow; row++) {
            const Utf8Char *u8c = u8c_ring_row(row);
            int col_start = (row == srow) ? scol : 0;
            int col_end = (row == erow) ? ecol : ring_cols();
            u8c += col_start;
            
            for (int col = col_start; col < col_end; col++, u8c++) {
                sel += u8c->text_utf8();
            }
            if (row < erow) sel += '\n';
        }
        
        if (!sel.empty()) {
            Fl::copy(sel.c_str(), sel.length(), 1);
        }
        clear_mouse_selection();
        redraw();
    }
}

void Fl_Console::paste_from_clipboard() {
    Fl::paste(*this, 1);
}

void Fl_Console::handle_paste(const char* text) {
    if (!text) return;
    
    for (const char* p = text; *p; p++) {
        if (*p != '\r' && *p != '\n') {
            insert_char(*p);
        }
    }
}

void Fl_Console::load_history(const std::string& filename) {
    std::ifstream file(filename);
    std::string line;
    
    history.clear();
    while (std::getline(file, line)) {
        if (!line.empty()) {
            history.push_back(line);
        }
    }
}

void Fl_Console::save_history(const std::string& filename) {
    std::ofstream file(filename);
    for (const auto& line : history) {
        file << line << '\n';
    }
}

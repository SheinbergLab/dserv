#pragma once

#include <FL/Fl_Terminal.H>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>

typedef int (*PROCESS_CB)(char *str, void *clientData);

class Fl_Console : public Fl_Terminal {
public:
  // Completion structure
  struct Completions {
    std::vector<std::string> items;
    
    void clear() { items.clear(); }
    void add(const std::string& str) { items.push_back(str); }
    size_t size() const { return items.size(); }
    const std::string& operator[](size_t idx) const { return items[idx]; }
    bool empty() const { return items.empty(); }
  };

private:
    // Line editing state
  enum EditMode {
    MODE_NORMAL = 0,
    MODE_COMPLETION,
    MODE_ESCAPE_SEQUENCE
  };
  
  EditMode mode;
    std::string current_line;
    size_t cursor_pos;
    std::string prompt;
    
    // Completion state
    Completions completions;
    size_t completion_index;
    std::string original_line;  // Line before completion started
    size_t original_cursor;     // Cursor before completion started
    
    // Escape sequence state
    std::string escape_sequence;
    
    // History
    std::vector<std::string> history;
    int history_index;  // -1 means not browsing history
    size_t max_history_size;
    std::string history_file;
    
    // Available commands for completion
    std::vector<std::string> available_commands;
    
    // Callback
    PROCESS_CB process_cb;
    void *process_cb_data;
    
    // Internal methods
    void refresh_line();
    void start_completion();
    void cycle_completion();
    void accept_completion();
    void cancel_completion();
    void handle_escape_sequence(char c);
    void add_to_history(const std::string& line);
    void move_cursor_left();
    void move_cursor_right();
    void move_to_start();
    void move_to_end();
    void delete_char();
    void backspace();
    void clear_line();
    void delete_word();
    void history_prev();
    void history_next();
    void insert_char(char c);
    void execute_line();
    std::string get_current_word() const;
    void reset_to_prompt();

public:
    Fl_Console(int X, int Y, int W, int H, const char *L = nullptr);
    ~Fl_Console();

    // FLTK overrides
    int handle(int e) override;
    void resize(int X, int Y, int W, int H) override;

    // Console interface
    void set_callback(PROCESS_CB cb, void *cbdata);
    void set_prompt(const std::string& prompt_str);
    void update_command_list(const std::vector<std::string>& commands);
    void load_history(const std::string& filename);
    void save_history(const std::string& filename);
    void init_console();
    
    // Terminal interface  
    void copy_to_clipboard();
    void paste_from_clipboard();
    void handle_paste(const char* text);
    
    // For compatibility with existing code
    int getch() { return -1; } // Not used in integrated version
    int do_callback(char *str);
};

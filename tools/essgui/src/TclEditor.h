#pragma once

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/fl_ask.H>
#include <cstring>
#include <cctype>
#include <vector>
#include <string>
#include <sstream>
#include <regex>

#include "TclFormatter.h"


class TclEditor;
void handle_auto_indent(TclEditor* editor);
static void format_editor_text_preserve_cursor(Fl_Text_Buffer* buffer, 
					       TclEditor* editor);


class TclEditor : public Fl_Text_Editor {
private:
  Fl_Menu_Button *context_menu;
  static TclEditor* menu_editor; // For static callbacks
  std::string kill_buffer; // Store killed text for yanking
  bool last_was_kill;      // Track if last command was a kill
  
 // Static callback functions for menu items
  static void cut_cb(Fl_Widget*, void* data) {
    TclEditor* editor = static_cast<TclEditor*>(data);
    editor->kf_cut(0, editor);
  }
  
  static void copy_cb(Fl_Widget*, void* data) {
    TclEditor* editor = static_cast<TclEditor*>(data);
    editor->kf_copy(0, editor);
  }
  
  static void paste_cb(Fl_Widget*, void* data) {
    TclEditor* editor = static_cast<TclEditor*>(data);
    editor->kf_paste(0, editor);
  }
  
  static void format_cb(Fl_Widget*, void* data) {
    TclEditor* editor = static_cast<TclEditor*>(data);
    editor->format_code();
  }
  
  static void select_all_cb(Fl_Widget*, void* data) {
    TclEditor* editor = static_cast<TclEditor*>(data);
    editor->kf_select_all(0, editor);
  }
  
public:
  int indent_size = 4;
  
  Fl_Text_Buffer *textbuf;
  Fl_Text_Buffer *stylebuf;
  
  TclEditor(int X, int Y, int W, int H, const char* l = 0) : Fl_Text_Editor(X, Y, W, H, l) {
    stylebuf = new Fl_Text_Buffer();

    // Create invisible menu button for context menu
    context_menu = new Fl_Menu_Button(0, 0, 0, 0);
    context_menu->type(Fl_Menu_Button::POPUP3);
    
    // Define menu structure
    context_menu->add("Cut", FL_CTRL+'x', cut_cb, this, FL_MENU_DIVIDER);
    context_menu->add("Copy", FL_CTRL+'c', copy_cb, this, 0);
    context_menu->add("Paste", FL_CTRL+'v', paste_cb, this, FL_MENU_DIVIDER);
    context_menu->add("Select All", FL_CTRL+'a', select_all_cb, this, FL_MENU_DIVIDER);
    context_menu->add("Format", FL_META+'f', format_cb, this, 0);    
  }

  ~TclEditor() {
    delete stylebuf;
    delete context_menu;
  }
  
  int handle(int event) override {
    if (event == FL_KEYDOWN) {
      int key = Fl::event_key();
      int ctrl = Fl::event_state() & FL_CTRL;
      int shift = Fl::event_state() & FL_SHIFT;
      
      if (key == FL_Enter) {
	//	handle_auto_indent(this);
	handle_enter();
	last_was_kill = false;
	return 1;
      }

      if (key == FL_Tab && !shift) {
	// Handle Tab key press
	handle_tab();
	last_was_kill = false;
	return 1; // Event handled
      }
      
      // Emacs/macOS keybindings
      if (ctrl) {
	switch (key) {
	case 'a': case 'A': // Beginning of line
	  handle_beginning_of_line();
	  last_was_kill = false;
	  return 1;
	case 'e': case 'E': // End of line
	  handle_end_of_line();
	  last_was_kill = false;
	  return 1;
	case 'f': case 'F': // Forward char
	  handle_forward_char();
	  last_was_kill = false;
	  return 1;
	case 'b': case 'B': // Back char
	  handle_back_char();
	  last_was_kill = false;
	  return 1;
	case 'n': case 'N': // Next line
	  handle_next_line();
	  last_was_kill = false;
	  return 1;
	case 'p': case 'P': // Previous line
	  handle_previous_line();
	  last_was_kill = false;
	  return 1;
	case 'd': case 'D': // Delete char
	  handle_delete_char();
	  last_was_kill = false;
	  return 1;
	case 'k': case 'K': // Kill to end of line
	  handle_kill_line();
	  // Note: last_was_kill is set in handle_kill_line()
	  return 1;
	case 'y': case 'Y': // Yank
	  handle_yank();
	  last_was_kill = false;
	  return 1;
	}
      }
      // Any other key resets the kill sequence
      // Check for cursor movement keys that aren't handled above
      if (key == FL_Left || key == FL_Right || key == FL_Up || key == FL_Down ||
	  key == FL_Home || key == FL_End || key == FL_Page_Up || key == FL_Page_Down) {
	last_was_kill = false;
      } else if (key != (FL_KP + 'k') && key != (FL_KP + 'K')) {
	// Reset for any other key except Ctrl+K itself
	last_was_kill = false;
      }
    }
    else if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
      show_context_menu();
      return 1; // Event handled
    }    
    else  if (event == FL_MOUSEWHEEL) {
      Fl_Text_Editor::handle(event);
      return 1;			// consume even if nothing to do
    } else if (event == FL_PUSH || event == FL_DRAG || event == FL_RELEASE) {
      // Mouse events also reset the kill sequence
      last_was_kill = false;
    }    
    return Fl_Text_Editor::handle(event);
  }

  void handle_tab() {
    // Get current cursor position
    int cursor_pos = insert_position();
    
    // Get the entire buffer text
    char* text = buffer()->text();
    std::string code(text);
    free(text);
    
    // Split into lines
    std::vector<std::string> lines = TclFormatter::split_lines(code);
    
    // Find which line the cursor is on
    int current_line = 0;
    int char_count = 0;
    for (int i = 0; i < lines.size(); ++i) {
      char_count += lines[i].length() + 1; // +1 for newline
      if (cursor_pos < char_count) {
	current_line = i;
	break;
      }
    }
    
    // Calculate proper indent for this line
    int target_indent = TclFormatter::calculate_line_indent(lines, current_line, 4);
    
    // Get current line's existing indent
    std::string& line = lines[current_line];
    int current_indent = 0;
    for (char c : line) {
      if (c == ' ') current_indent++;
      else if (c == '\t') current_indent += 4; // Assuming tab = 4 spaces
      else break;
    }
    
    // Adjust indentation
    if (current_indent < target_indent) {
      // Need to add spaces
      int spaces_to_add = target_indent - current_indent;
      
      // Find line start position
      int line_start = 0;
      for (int i = 0; i < current_line; ++i) {
	line_start += lines[i].length() + 1;
      }
      
      // Insert spaces at beginning of line
      buffer()->insert(line_start, std::string(spaces_to_add, ' ').c_str());
      
      // Move cursor to end of indentation
      insert_position(line_start + target_indent);
    } else if (current_indent > target_indent) {
      // Need to remove spaces
      int spaces_to_remove = current_indent - target_indent;
      
      // Find line start position
      int line_start = 0;
      for (int i = 0; i < current_line; ++i) {
	line_start += lines[i].length() + 1;
      }
      
      // Remove excess spaces
      buffer()->remove(line_start, line_start + spaces_to_remove);
      
      // Move cursor to end of indentation
      insert_position(line_start + target_indent);
    } else {
      // Already at correct indent - move cursor to end of indentation
      int line_start = 0;
      for (int i = 0; i < current_line; ++i) {
	line_start += lines[i].length() + 1;
      }
      insert_position(line_start + current_indent);
    }
  }

  void handle_enter() {
    // Get current cursor position
    int cursor_pos = insert_position();
        
    // Insert newline first
    buffer()->insert(cursor_pos, "\n");
    cursor_pos++; // Move past the newline
    
    // Move cursor to the new line
    insert_position(cursor_pos);
    
    // Get updated text and calculate indent for new line
    char* text = buffer()->text();
    std::string code(text);
    free(text);
    
    std::vector<std::string> lines = TclFormatter::split_lines(code);
    
    // Find current line (after the newline)
    int current_line = 0;
    int char_count = 0;
    for (int i = 0; i < lines.size(); ++i) {
      char_count += lines[i].length() + 1;
      if (cursor_pos <= char_count) {
	current_line = i;
	break;
      }
    }
    
    // Calculate and insert proper indent
    int indent = TclFormatter::calculate_line_indent(lines, current_line, 4);
    if (indent > 0) {
      buffer()->insert(cursor_pos, std::string(indent, ' ').c_str());
      cursor_pos += indent;
    }
    
    // Always update cursor position to after the indent
    insert_position(cursor_pos);
  }


  // Emacs/macOS keybinding implementations
  void handle_beginning_of_line() {
    int pos = insert_position();
    int line_start = buffer()->line_start(pos);
    insert_position(line_start);
  }
    
  void handle_end_of_line() {
    int pos = insert_position();
    int line_end = buffer()->line_end(pos);
    insert_position(line_end);
  }
    
  void handle_forward_char() {
    int pos = insert_position();
    int max_pos = buffer()->length();
    if (pos < max_pos) {
      insert_position(pos + 1);
    }
  }
    
  void handle_back_char() {
    int pos = insert_position();
    if (pos > 0) {
      insert_position(pos - 1);
    }
  }
    
  void handle_next_line() {
    int pos = insert_position();
    int line = buffer()->count_lines(0, pos);
    int col = pos - buffer()->line_start(pos);
        
    // Move to next line
    if (line < buffer()->count_lines(0, buffer()->length())) {
      int next_line_start = buffer()->line_end(pos) + 1;
      if (next_line_start <= buffer()->length()) {
	int next_line_end = buffer()->line_end(next_line_start);
	int next_line_length = next_line_end - next_line_start;
                
	// Try to maintain column position
	int new_pos = next_line_start + std::min(col, next_line_length);
	insert_position(new_pos);
      }
    }
  }
    
  void handle_previous_line() {
    int pos = insert_position();
    int line_start = buffer()->line_start(pos);
    int col = pos - line_start;
    
    // Move to previous line
    if (line_start > 0) {
      int prev_line_end = line_start - 1;
      int prev_line_start = buffer()->line_start(prev_line_end);
      int prev_line_length = prev_line_end - prev_line_start;
      
      // Try to maintain column position
      int new_pos = prev_line_start + std::min(col, prev_line_length);
      insert_position(new_pos);
    }
  }

  void handle_delete_char() {
    int pos = insert_position();
    int max_pos = buffer()->length();
    
    if (pos < max_pos) {
      // Check if we're at the end of a line (before a newline)
      int line_end = buffer()->line_end(pos);
      
      if (pos == line_end && pos < max_pos) {
	// At end of line - delete the newline to join lines
	buffer()->remove(pos, pos + 1);
      } else {
	// Normal case - delete character at cursor
	buffer()->remove(pos, pos + 1);
      }
    }
    
    // Note: Unlike kill commands, delete doesn't affect the kill buffer
    // and doesn't copy to clipboard in standard Emacs
  }

  void handle_kill_line() {
    int pos = insert_position();
    int line_end = buffer()->line_end(pos);
    
    std::string killed_text;
    
    if (pos == line_end) {
      // At end of line - kill the newline character if there is one
      if (pos < buffer()->length()) {
	// Kill just the newline
	killed_text = "\n";
	buffer()->remove(pos, pos + 1);
      }
    } else {
      // Kill from cursor to end of line
      char* text = buffer()->text_range(pos, line_end);
      killed_text = std::string(text);
      free(text);
      
      buffer()->remove(pos, line_end);
    }
    
    // Append or replace kill buffer based on whether last command was also a kill
    if (last_was_kill) {
      // Append to existing kill buffer
      kill_buffer += killed_text;
    } else {
      // Start new kill buffer
      kill_buffer = killed_text;
    }
    
    // Copy entire kill buffer to system clipboard
    Fl::copy(kill_buffer.c_str(), kill_buffer.length(), 1);
    
    // Mark that this was a kill command
    last_was_kill = true;
  }
  
  void handle_yank() {
    if (!kill_buffer.empty()) {
      int pos = insert_position();
      buffer()->insert(pos, kill_buffer.c_str());
      insert_position(pos + kill_buffer.length());
    } else {
      // If kill buffer is empty, try to paste from system clipboard
      Fl::paste(*this, 1);
    }
  }  
  
  void format_code(void) {
    format_editor_text_preserve_cursor(buffer(), this);
  }
  
private:
  void show_context_menu() {
    // Get current state for context sensitivity
    int start, end;
    bool has_selection = buffer()->selection_position(&start, &end);
    bool has_content = (buffer()->length() > 0);
    bool can_paste = (Fl::clipboard_contains(Fl::clipboard_plain_text));
    
    // Update menu item states
    Fl_Menu_Item* items = const_cast<Fl_Menu_Item*>(context_menu->menu());
    
    // Find and update menu items by name
    for (int i = 0; items[i].text; i++) {
      if (strcmp(items[i].text, "Cut") == 0) {
	if (has_selection) {
	  items[i].activate();
	} else {
	  items[i].deactivate();
	}
      }
      else if (strcmp(items[i].text, "Copy") == 0) {
	if (has_selection) {
	  items[i].activate();
	} else {
	  items[i].deactivate();
	}
      }
      else if (strcmp(items[i].text, "Paste") == 0) {
	if (can_paste) {
	  items[i].activate();
	} else {
	  items[i].deactivate();
	}
      }
      else if (strcmp(items[i].text, "Select All") == 0) {
	if (has_content) {
	  items[i].activate();
	} else {
	  items[i].deactivate();
	}
      }
      else if (strcmp(items[i].text, "Format") == 0) {
	if (has_content) {
	  items[i].activate();
	} else {
	  items[i].deactivate();
	}
      }
    }
    
    // Show menu at mouse position
    context_menu->position(Fl::event_x(), Fl::event_y());
    const Fl_Menu_Item* selected = context_menu->popup();
    if (selected && selected->callback()) {
      selected->do_callback(this, this);
    }
  }  
};

void configure_editor(TclEditor *editor, Fl_Text_Buffer *buffer);
void initial_styling(TclEditor *editor);

static void format_editor_text(Fl_Text_Buffer* buffer) {
  char* text = buffer->text();
  std::string code(text);
  free(text);
  
  std::string formatted = TclFormatter::format_tcl_code(code);
  buffer->text(formatted.c_str());
}

// Alternative that preserves cursor position
static void format_editor_text_preserve_cursor(Fl_Text_Buffer* buffer, 
					       TclEditor* editor) {
  int cursor_pos = editor->insert_position();
  char* text = buffer->text();
  std::string code(text);
  free(text);
  
  std::string formatted = TclFormatter::format_tcl_code(code,
							editor->indent_size);
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


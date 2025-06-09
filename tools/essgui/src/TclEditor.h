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
    context_menu->add("Format", FL_CTRL+'f', format_cb, this, 0);    
  }

  ~TclEditor() {
    delete stylebuf;
    delete context_menu;
  }
  
  int handle(int event) override {
    if (event == FL_KEYDOWN) {
      int key = Fl::event_key();
      if (key == FL_Enter) {
	handle_auto_indent(this);

	return 1;
      }
      if (key == FL_Tab) {
	format_code();
	return 1;
      }
    }
    else if (event == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
      show_context_menu();
      return 1; // Event handled
    }    
    else  if (event == FL_MOUSEWHEEL) {
      Fl_Text_Editor::handle(event);
      return 1;			// consume even if nothing to do
    }
    return Fl_Text_Editor::handle(event);
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


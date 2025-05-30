#pragma once

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/fl_ask.H>
#include <cstring>
#include <cctype>
#include <vector>
#include <string>

void handle_auto_indent(Fl_Text_Editor* editor);

class TclEditor : public Fl_Text_Editor {
public:
  Fl_Text_Buffer *textbuf;
  Fl_Text_Buffer *stylebuf;
  
  TclEditor(int X, int Y, int W, int H, const char* l = 0) : Fl_Text_Editor(X, Y, W, H, l) {
    stylebuf = new Fl_Text_Buffer();
  }
  
  int handle(int event) override {
    if (event == FL_KEYDOWN) {
      int key = Fl::event_key();
      if (key == FL_Enter) {
	handle_auto_indent(this);

	return 1;
      }
    }
    else  if (event == FL_MOUSEWHEEL) {
      int dy = Fl::event_dy();
      
      // Get the text display widget from the editor
      Fl_Text_Display* display = this;
      
      // Check current scroll position
      int top_line = display->get_absolute_top_line_number();
      int total_lines = buffer()->count_lines(0, buffer()->length());
      
      // Calculate visible lines based on widget height and font
      int line_height = fl_height(textfont(), textsize());
      int visible_lines_count = h() / line_height;
      
      // Check if we're at boundaries
      bool at_top = (top_line <= 1 && dy < 0);
      bool at_bottom = (top_line + visible_lines_count >= total_lines && dy > 0);
      
      if (at_top || at_bottom) {
	return 1;  // Consume event to prevent propagation
      }
    }
    return Fl_Text_Editor::handle(event);
  }

  ~TclEditor() {
    delete stylebuf;
  }  
};

void configure_editor(TclEditor *editor, Fl_Text_Buffer *buffer);
void initial_styling(TclEditor *editor);


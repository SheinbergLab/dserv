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
    return Fl_Text_Editor::handle(event);
  }

  ~TclEditor() {
    delete stylebuf;
  }  
};

void configure_editor(TclEditor *editor, Fl_Text_Buffer *buffer);
void initial_styling(TclEditor *editor);


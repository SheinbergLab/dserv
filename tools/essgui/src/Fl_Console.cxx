#include <FL/Fl.H>
#include <iostream>
#include <cassert>
#include "Fl_Console.h"

Fl_Console::Fl_Console(int X,int Y,int W,int H, const char *L):
  Fl_Terminal(X,Y,W,H,L) {
  lastkey = -1;
  buflen = 4096;
  buf = new char[buflen];
  l_state.prompt = NULL;
  prompt("essgui> ");
  process_cb = nullptr;
  process_cb_data = nullptr;
}

Fl_Console::~Fl_Console(void) {
  delete[] buf;
}

std::string Fl_Console::get_current_selection(void)
{
  // Get selection
  int srow,scol,erow,ecol;
  std::string sel;
  if (get_selection(srow,scol,erow,ecol)) {                // mouse selection exists?
    // Walk entire selection from start to end
    for (int row=srow; row<=erow; row++) {                 // walk rows of selection
      const Utf8Char *u8c = u8c_ring_row(row);             // ptr to first character in row
      int col_start = (row==srow) ? scol : 0;              // start row? start at scol
      int col_end   = (row==erow) ? ecol : ring_cols();    // end row?   end at ecol
      u8c += col_start;                                    // include col offset (if any)
      for (int col=col_start; col<=col_end; col++,u8c++) { // walk columns
	sel += u8c->text_utf8();
      }
    }
  }
  return sel;
}

int Fl_Console::handle(int e) {
  
  switch (e) {
  case FL_PUSH:
    {
      if (Fl::event_button3()) {
	std::string sel = get_current_selection();
	Fl::copy(sel.c_str(), strlen(sel.c_str()), 1);
      }
    }
    break;
  case FL_KEYDOWN:
    {
      const char *keybuf = Fl::event_text();
      lastkey = keybuf[0];
      //      std::cout << "keydown: " << Fl::event_key() << " alt_status (" << Fl::event_alt() << ")" << std::endl;

      if (Fl::event_alt()) return 0;

      if (Fl::event_ctrl() && Fl::event_key() == 'c') {
	std::string sel = get_current_selection();
	Fl::copy(sel.c_str(), strlen(sel.c_str()), 1);
      }
      
      switch (Fl::event_key()) {
      case FL_Meta_L:
      case FL_Meta_R:
      case FL_Alt_L:
      case FL_Alt_R:
      case FL_Shift_L:
      case FL_Shift_R:
      case FL_Control_L:
      case FL_Control_R:
      case FL_Caps_Lock:
	return 0;

      case FL_Enter:
	{
	  int final_len = lnHandleCharacter(&l_state, (char) lastkey);
	  append("\n");
	  if (l_state.len) {
	    linenoiseHistoryAdd(&l_state, buf); /* Add to the history. */
	    linenoiseHistorySave(&l_state, "history.txt"); /* Save the history on disk. */
	    do_callback(buf);
	  }
	  lnInitState(&l_state, buf, buflen, prompt().c_str());
	  return 1;
	}
	break;
      case FL_Left:
	{
	  lnHandleCharacter(&l_state, (char) CTRL_B);
	  redraw();
	  return 1;
	}
      case FL_Right:
	{
	  lnHandleCharacter(&l_state, (char) CTRL_F);
	  redraw();
	  return 1;
	}
      case FL_Up:
	{
	  lnHandleCharacter(&l_state, (char) CTRL_P);
	  redraw();
	  return 1;
	}
      case FL_Down:
	{
	  lnHandleCharacter(&l_state, (char) CTRL_N);
	  redraw();
	  return 1;
	}
      default:
	lnHandleCharacter(&l_state, (char) lastkey);
	redraw();
	return 1;
      }
    }
  }
  return Fl_Terminal::handle(e);
}

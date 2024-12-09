#include <FL/Fl.H>
#include <iostream>
#include <cassert>
#include "Fl_Console.h"

Fl_Console::Fl_Console(int X,int Y,int W,int H, const char *L):
  Fl_Terminal(X,Y,W,H,L) {
  lastkey = -1;
  buflen = 4096;
  buf = new char[buflen];
  prompt = std::string("hb> ");
  process_cb = nullptr;
  process_cb_data = nullptr;
}

Fl_Console::~Fl_Console(void) {
  delete[] buf;
}

int Fl_Console::handle(int e) {
  
  switch (e) {
  case FL_KEYDOWN:
    {
      const char *keybuf = Fl::event_text();
      lastkey = keybuf[0];
      //      std::cout << "keydown: " << Fl::event_key() << " alt_status (" << Fl::event_alt() << ")" << std::endl;

      if (Fl::event_alt()) return 0;
      
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
	  lnInitState(&l_state, buf, buflen, prompt.c_str());
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

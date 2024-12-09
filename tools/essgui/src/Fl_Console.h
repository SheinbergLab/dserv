#ifndef FL_CONSOLE_H
#define FL_CONSOLE_H

#include "linenoise-fltk.h"
#include <iostream>
#include <string>
#include <FL/Fl.H>
#include <FL/Fl_Terminal.H>

typedef int (*PROCESS_CB)(char *str, void *clienData);

class Fl_Console : public Fl_Terminal {
private:
  int lastkey;
  struct linenoiseState l_state;
  char *buf;
  int buflen;
  std::string prompt;
  PROCESS_CB process_cb;
  void *process_cb_data;
  
public:
  Fl_Console(int X,int Y,int W,int H, const char *L=0);
  ~Fl_Console(void);

  int handle(int e) FL_OVERRIDE;

  void resize(int X,int Y,int W,int H) FL_OVERRIDE {
    Fl_Terminal::resize(X,Y,W,H);
    // could update cols here but doesn't seem to work?
    // l_state.cols = display_columns();
    //    std::cout << "cols: " << l_state.cols << " w: " << W << " fontsize: " << textsize() << std::endl;
  }
  
  int getch(void) { return lastkey; }

  void set_callback(PROCESS_CB cb, void *cbdata) {
    process_cb = cb;
    process_cb_data = cbdata;
  }

  int do_callback(char *str)
  {
    if (!process_cb) return 0;
    return (process_cb(str, process_cb_data));
  }
  
  int init_linenoise(void)
  {
    lnInitState(&l_state, buf, buflen, prompt.c_str());
    l_state.cols = display_columns();
    l_state.mode = linenoiseState::ln_read_regular;
    linenoiseHistoryLoad(&l_state, "history.txt"); /* Load the history at startup */
    return 0;
  }  
};


#endif

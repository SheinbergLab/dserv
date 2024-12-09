#ifndef EYETOUCHWIN_HPP
#define EYETOUCHWIN_HPP

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Box.H>

#include <iostream>
#include <vector>
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

class EyeTouchWin : public Fl_Box {
private:

public:

  virtual void draw() FL_OVERRIDE {
    fl_color(FL_BLACK);
    fl_rectf(x(),y(),w(),h());
  }
  
  static int clear(void) {
    window()->make_current();
    fl_color(FL_BLACK);// should be background color
    fl_rectf(x(), y(), w(), h());
    return 0;
  }
  
  static int line(float x0, float y0, float x1, float y1) 
  {
    window()->make_current();
    fl_line(x()+x0, y()+h()-y0,
	    x()+x1, y()+h()-y1);
    return 0;
  }

  static int point(float x, float y)
  {
    window()->make_current();
    fl_point(x()+x, y()+y);
    return 0;
  }

  static int circle(float x, float y, float width, int filled)
  {
    window()->make_current();
    y = h()-y;
    if (!filled) {
      fl_circle(x()+x+width/2, y()+y+width/2, width/2);
    }
    else {
      fl_begin_polygon();
      fl_arc(x()+x+width/2, y()+y+width/2, width/2, 0.0, 360.0);
      fl_end_polygon();
    }
    return 0;
  }
  
  EyeTouchWin(int X, int Y, int W, int H, const char*L=0) : Fl_Box(X,Y,W,H,L) {

  }

    
  ~EyeTouchWin()
  {
  }
  
};



#endif

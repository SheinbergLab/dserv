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

typedef enum { WINDOW_IN, WINDOW_OUT } WINDOW_STATE;
typedef enum { WINDOW_RECTANGLE, WINDOW_ELLIPSE } WINDOW_TYPE;
enum { WINDOW_NOT_INITIALIZED, WINDOW_INITIALIZED };

class EyeRegion {

public:
  int reg;
  bool active;
  WINDOW_STATE state;
  WINDOW_TYPE type;
  int center_x;
  int center_y;
  int plusminus_x;
  int plusminus_y;
};
  
class EyeTouchWin : public Fl_Box {
private:
  float deg_per_pix_x;
  float deg_per_pix_y;
  float xextent;			/* extent in degrees */
  float yextent;			/* extent in degrees */

  float em_pos_x;
  float em_pos_y;
  float em_radius;
  float xextent_2;
  float yextent_2;
  bool flipx;
  bool flipy;

  static const int nregions = 8;
  EyeRegion regions[nregions];
  
public:

  void region_set(int settings[8]) {
    if (settings[0] >= 0 && settings[0] < nregions) {
      regions[settings[0]].reg         = settings[0];
      regions[settings[0]].active      = settings[1];
      regions[settings[0]].state       = (WINDOW_STATE) settings[2];
      regions[settings[0]].type        = (WINDOW_TYPE) settings[3];
      regions[settings[0]].center_x    = settings[4];
      regions[settings[0]].center_y    = settings[5];
      regions[settings[0]].plusminus_x = settings[6];
      regions[settings[0]].plusminus_y = settings[7];
    }
  }

  void status_set(int status[4]) {
    int changes = status[0];
    int states = status[1];
    int adc_x = status[2];
    int adc_y = status[3];

    for (int i = 0; i < nregions; i++) {
      if (changes & (1 << i)) {
	regions[i].state = (WINDOW_STATE) ((states & (1 << i)) != 0);
	//	printf("set region %d -> %d\n", i, regions[i].state); 
      }
    }
    //    printf("region status: changes=%02x states=%02x adc_x=%d adc_y=%d\n",
    //	   status[0], status[1], status[2], status[3]);
  }
  
  void draw() FL_OVERRIDE {
    /* clear background */
    fl_color(FL_BLACK);
    fl_rectf(x(), y(), w(), h());

    /* clip to window */
    fl_push_clip(x(), y(), w(), h());

    /* set these for degree based operations */
    deg_per_pix_x = xextent/w();
    deg_per_pix_y = deg_per_pix_x*((float) h()/w());

    /* draw the eye position */
    draw_eye_marker();
    
    fl_pop_clip();

    redraw();
  }

  void draw_eye_marker()
  {
    fl_color(FL_WHITE);
    float xpos = x()+w()/2+(em_pos_x-em_radius/2)/deg_per_pix_x;
    float ypos = y()+h()/2+(-(em_pos_y+em_radius/2))/deg_per_pix_y;
    float radius = em_radius/deg_per_pix_x;
    fl_arc(xpos, ypos, radius, radius, 0.0, 360.0);
  }

  void em_pos(float x, float y)
  {
    bool do_redraw = false;
    if (x != em_pos_x) { em_pos_x = x; do_redraw = true; }
    if (y != em_pos_y) { em_pos_y = y; do_redraw = true; }

    if (do_redraw) {
      redraw();
    }
  }
  
  EyeTouchWin(int X, int Y, int W, int H, const char*L=0) : Fl_Box(X,Y,W,H,L)
  {
    xextent = 10*2;
    yextent = 10*2;
    xextent_2 = xextent/2;
    yextent_2 = yextent/2;
    em_pos_x = 0;
    em_pos_y = 0;
    em_radius = 0.75;
    flipx = false;
    flipy = false;
  }

    
  ~EyeTouchWin()
  {
  }
  
};



#endif

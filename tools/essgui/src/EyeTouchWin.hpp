#ifndef EYETOUCHWIN_HPP
#define EYETOUCHWIN_HPP

#include <FL/Fl.H>
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

  void set(int settings[8]) {
    reg         = settings[0];
    active      = settings[1];
    state       = (WINDOW_STATE) settings[2];
    type        = (WINDOW_TYPE) settings[3];
    center_x    = settings[4];
    center_y    = settings[5];
    plusminus_x = settings[6];
    plusminus_y = settings[7];
  }
};

class TouchRegion: public EyeRegion {
};

class EyeTouchWin : public Fl_Box {
private:
  float deg_per_pix_x;
  float deg_per_pix_y;

  float adc_points_per_deg_x;
  float adc_points_per_deg_y;

  float xextent;			/* extent in degrees */
  float yextent;			/* extent in degrees */

  float em_pos_x;
  float em_pos_y;
  float em_radius;
  float xextent_2;
  float yextent_2;
  bool flipx;
  bool flipy;

  int touch_pix_x;
  int touch_pix_y;
  
  static const int n_eye_regions = 8;
  EyeRegion eye_regions[n_eye_regions];

  static const int n_touch_regions = 8;
  EyeRegion touch_regions[n_touch_regions];

public:

  void eye_region_set(int settings[8]) {
    if (settings[0] >= 0 && settings[0] < n_eye_regions) {
      eye_regions[settings[0]].set(settings);
    }
    redraw();
  }
  
  void touch_region_set(int settings[8]) {
    if (settings[0] >= 0 && settings[0] < n_touch_regions) {
      touch_regions[settings[0]].set(settings);
    }
    redraw();
  }

  void eye_status_set(int status[4]) {
    int changes = status[0];
    int states = status[1];
    int adc_x = status[2];
    int adc_y = status[3];

    for (int i = 0; i < n_eye_regions; i++) {
      if (changes & (1 << i)) {
	eye_regions[i].state = (WINDOW_STATE) ((states & (1 << i)) != 0);
	//	printf("set region %d -> %d\n", i, eye_regions[i].state); 
      }
    }
    //    printf("region status: changes=%02x states=%02x adc_x=%d adc_y=%d\n",
    //	   status[0], status[1], status[2], status[3]);
  }

  void touch_status_set(int status[4]) {
    int changes = status[0];
    int states = status[1];

    for (int i = 0; i < n_touch_regions; i++) {
      if (changes & (1 << i)) {
	touch_regions[i].state = (WINDOW_STATE) ((states & (1 << i)) != 0);
	printf("set touch region %d -> %d\n", i, touch_regions[i].state); 
      }
    }
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

    /* draw eye regions */
    for (int i = 0; i < n_eye_regions; i++) {
      draw_eye_region(&eye_regions[i]);
    }

    /* draw touch regions */
    for (int i = 0; i < n_touch_regions; i++) {
      //      draw_touch_region(&touch_regions[i]);
    }
    
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

  void draw_eye_region(EyeRegion *region)
  {
    if (!region->active) return;

    fl_color(FL_RED);

    float cx_deg = (region->center_x-2048)/adc_points_per_deg_x;
    float cy_deg = (region->center_y-2048)/adc_points_per_deg_y;
    float w_deg = (region->plusminus_x/adc_points_per_deg_x);
    float h_deg = (region->plusminus_y/adc_points_per_deg_y);
    
    float xpos = x()+w()/2+(cx_deg)/deg_per_pix_x;
    float ypos = y()+h()/2+(cy_deg)/deg_per_pix_y;
    float w = w_deg/deg_per_pix_x;
    float h = h_deg/deg_per_pix_y;
    if (region->type == WINDOW_ELLIPSE) {
      fl_arc(xpos-w, ypos-h, 2*w, 2*h, 0.0, 360.0);
      fl_arc(xpos-2, ypos-2, 4, 4, 0.0, 360.0);
    }
    else
      fl_rect(xpos-w, ypos-h, 2*w, 2*h);
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

  void touch_pos(int x, int y)
  {
    bool do_redraw = false;
    if (x != touch_pix_x) { touch_pix_x = x; do_redraw = true; }
    if (y != touch_pix_y) { touch_pix_y = y; do_redraw = true; }

    if (do_redraw) {
      redraw();
    }
  }
  
  EyeTouchWin(int X, int Y, int W, int H, const char*L=0) : Fl_Box(X,Y,W,H,L)
  {
    /* these should be set dynamically */
    adc_points_per_deg_x = 200.0;
    adc_points_per_deg_y = 200.0;
    
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

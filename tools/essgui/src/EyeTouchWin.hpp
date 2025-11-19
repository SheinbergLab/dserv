#ifndef EYETOUCHWIN_HPP
#define EYETOUCHWIN_HPP

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Box.H>

#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern void send_virtual_eye_position(float x, float y);
extern void send_virtual_touch_event(int x, int y, int event_type);

typedef enum { WINDOW_OUT, WINDOW_IN } WINDOW_STATE;
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

  float points_per_deg_x;
  float points_per_deg_y;

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

  int _screen_w;		/* width of remote display in pixels  */
  int _screen_h;		/* height of remote display in pixels */
  float _screen_halfx;		/* degrees in half width of screen    */
  float _screen_halfy;          /* degrees in half height of screen   */
  
  static const int n_eye_regions = 8;
  EyeRegion eye_regions[n_eye_regions];

  static const int n_touch_regions = 8;
  TouchRegion touch_regions[n_touch_regions];

  // Virtual input states
  bool virtual_eye_enabled;
  bool virtual_touch_enabled;
  
  // Virtual eye state
  struct {
    float x, y;           // degrees
    int adc_x, adc_y;     // ADC values
    bool active;
    bool dragging;
    float drag_offset_x, drag_offset_y;
  } virtual_eye;

  // Virtual touch state  
  struct {
    int x, y;             // screen pixels
    bool active;
    bool dragging;
    int drag_start_x, drag_start_y;
  } virtual_touch;

  bool touch_active = false;
  
private:
  int handle_mouse_down() {
    if (!virtual_eye_enabled && !virtual_touch_enabled) return 0;
    
    int mx = Fl::event_x() - x();
    int my = Fl::event_y() - y();
    
    // Check if clicking on virtual eye marker
    if (virtual_eye_enabled && virtual_eye.active) {
      float eye_canvas_x = w()/2 + (virtual_eye.x/deg_per_pix_x);
      float eye_canvas_y = h()/2 - (virtual_eye.y/deg_per_pix_y);
      
      float dist = std::sqrt(std::pow(mx - eye_canvas_x, 2) +
			     std::pow(my - eye_canvas_y, 2));
      if (dist <= 13) { // 8px radius + 5px tolerance
	virtual_eye.dragging = true;
	virtual_eye.drag_offset_x = eye_canvas_x - mx;
	virtual_eye.drag_offset_y = eye_canvas_y - my;
	return 1;
      }
    }
    
    // Handle virtual touch start
    if (virtual_touch_enabled && !virtual_eye.dragging) {
      // Convert canvas to degrees then to touch screen coordinates
      float deg_x = (mx - w()/2) * deg_per_pix_x;
      float deg_y = -(my - h()/2) * deg_per_pix_y;
      
      float screen_pix_per_deg_x = _screen_w / (2 * _screen_halfx);
      float screen_pix_per_deg_y = _screen_h / (2 * _screen_halfy);
      
      virtual_touch.x = deg_x * screen_pix_per_deg_x + _screen_w/2;
      virtual_touch.y = -deg_y * screen_pix_per_deg_y + _screen_h/2;
      virtual_touch.active = true;
      virtual_touch.dragging = true;
      
      send_virtual_touch_event(virtual_touch.x, virtual_touch.y, 0); // PRESS
      return 1;
    }
    
    return 0;
  }
  
  int handle_mouse_drag() {
   int mx = Fl::event_x() - x();
    int my = Fl::event_y() - y();
    
    if (virtual_eye_enabled && virtual_eye.dragging) {
      // Calculate new position with drag offset
      float new_canvas_x = mx + virtual_eye.drag_offset_x;
      float new_canvas_y = my + virtual_eye.drag_offset_y;
      
      // Convert to degrees and constrain
      float deg_x = (new_canvas_x - w()/2) * deg_per_pix_x;
      float deg_y = -(new_canvas_y - h()/2) * deg_per_pix_y;
      
      // Clamp to visible range based on actual screen dimensions
      float max_x = xextent / 2;
      float max_y = yextent / 2;
      deg_x = std::max(-max_x, std::min(max_x, deg_x));
      deg_y = std::max(-max_y, std::min(max_y, deg_y));
      
      // Update position
      virtual_eye.x = deg_x;
      virtual_eye.y = deg_y;
      virtual_eye.active = true;
      virtual_eye.adc_x = deg_x * points_per_deg_x + 2048;
      virtual_eye.adc_y = -deg_y * points_per_deg_y + 2048;

      send_virtual_eye_position(virtual_eye.x, virtual_eye.y);
      
      redraw();
      return 1;
    }
  
    if (virtual_touch_enabled && virtual_touch.dragging) {
      // Convert to touch coordinates
      float deg_x = (mx - w()/2) * deg_per_pix_x;
      float deg_y = -(my - h()/2) * deg_per_pix_y;
      
      float screen_pix_per_deg_x = _screen_w / (2 * _screen_halfx);
      float screen_pix_per_deg_y = _screen_h / (2 * _screen_halfy);
      
      virtual_touch.x = deg_x * screen_pix_per_deg_x + _screen_w/2;
      virtual_touch.y = -deg_y * screen_pix_per_deg_y + _screen_h/2;
      
      send_virtual_touch_event(virtual_touch.x, virtual_touch.y, 1); // DRAG
      redraw();
      return 1;
    }
    
    return 0;
  }
  
  int handle_mouse_up() {
    if (virtual_eye.dragging) {
      virtual_eye.dragging = false;
      return 1;
    }
    
    if (virtual_touch.dragging) {
      send_virtual_touch_event(virtual_touch.x, virtual_touch.y, 2); // RELEASE
      virtual_touch.dragging = false;
      
      // Keep visible briefly after release
      Fl::add_timeout(0.5, [](void* v) {
	EyeTouchWin* w = (EyeTouchWin*)v;
	if (!w->virtual_touch.dragging) {
	  w->virtual_touch.active = false;
	  w->redraw();
	}
      }, this);
      
      return 1;
    }
    
    return 0;
  }
  
  int handle_mouse_leave() {
    // Cancel drags when mouse leaves
    if (virtual_eye.dragging) {
      virtual_eye.dragging = false;
    }
    
    if (virtual_touch.dragging) {
      send_virtual_touch_event(virtual_touch.x, virtual_touch.y, 2); // RELEASE
      virtual_touch.dragging = false;
      virtual_touch.active = false;
    }
    
    return 1;
  }
  
  void update_virtual_eye_position(float deg_x, float deg_y) {
    virtual_eye.x = deg_x;
    virtual_eye.y = deg_y;
    virtual_eye.active = true;
    
    // Convert to ADC
    virtual_eye.adc_x = deg_x * points_per_deg_x + 2048;
    virtual_eye.adc_y = -deg_y * points_per_deg_y + 2048;
    
    // Trigger callback or send data
    if (callback()) {
      do_callback();
    }
    
    redraw();
  }
  
public:

  int screen_w(void) { return _screen_w; }
  int screen_h(void) { return _screen_h; }
  float screen_halfx(void) { return _screen_halfx; }
  float screen_halfy(void) { return _screen_halfy; }
  
  void screen_w(int w) { _screen_w = w; }
  void screen_h(int h) { _screen_h = h; }
  void screen_halfx(float halfx) { _screen_halfx = halfx; }
  void screen_halfy(float halfy) { _screen_halfy = halfy; }
  
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
      eye_regions[i].state = (WINDOW_STATE) (((states & (1 << i)) != 0) && eye_regions[i].active);
	//	printf("set region %d -> %d\n", i, eye_regions[i].state); 
    }
    //    printf("region status: changes=%02x states=%02x adc_x=%d adc_y=%d\n",
    //	   status[0], status[1], status[2], status[3]);
  }

  void touch_status_set(int status[4]) {
    int changes = status[0];
    int states = status[1];

    for (int i = 0; i < n_touch_regions; i++) {
      touch_regions[i].state = (WINDOW_STATE) (((states & (1 << i)) != 0) && touch_regions[i].active);
      //      	printf("set touch region %d -> %d\n", i, touch_regions[i].state); 
    }
    redraw();
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

    /* draw eye regions */
    for (int i = 0; i < n_eye_regions; i++) {
      draw_eye_region(&eye_regions[i]);
    }

    /* draw eye status */
    draw_eye_status();
    
    /* draw touch regions */
    for (int i = 0; i < n_touch_regions; i++) {
      draw_touch_region(&touch_regions[i]);
    }

    /* draw touch status */
    draw_touch_status();

    /* draw touch marker */
    draw_touch();
    
    /* draw the eye position */
    draw_eye_marker();

    /* draw virtual eye marker */
    draw_virtual_eye();
    
    fl_pop_clip();
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

    float cx_deg = (region->center_x-2048)/points_per_deg_x;
    float cy_deg = (region->center_y-2048)/points_per_deg_y;
    float w_deg = (region->plusminus_x/points_per_deg_x);
    float h_deg = (region->plusminus_y/points_per_deg_y);
    
    float xpos = x()+w()/2+(cx_deg)/deg_per_pix_x;
    float ypos = y()+h()/2+(cy_deg)/deg_per_pix_y;
    float w = w_deg/deg_per_pix_x;
    float h = h_deg/deg_per_pix_y;
    
    // Draw light fill when inside (simulating transparency)
    if (region->state == WINDOW_IN) {
        fl_color(100, 50, 50);  // Dark red fill
        if (region->type == WINDOW_ELLIPSE) {
            fl_pie(xpos-w, ypos-h, 2*w, 2*h, 0.0, 360.0);
        } else {
            fl_rectf(xpos-w, ypos-h, 2*w, 2*h);
        }
    }
    
    // Draw the outline (always red for eye regions)
    fl_color(FL_RED);
    if (region->type == WINDOW_ELLIPSE) {
        fl_arc(xpos-w, ypos-h, 2*w, 2*h, 0.0, 360.0);
        fl_arc(xpos-2, ypos-2, 4, 4, 0.0, 360.0);
    } else {
        fl_rect(xpos-w, ypos-h, 2*w, 2*h);
    }
}

void draw_touch_region(TouchRegion *region)  // Note: TouchRegion type
{
    if (!region->active) return;

    float screen_pix_per_deg_x = screen_w()/(2*screen_halfx());
    float screen_pix_per_deg_y = screen_h()/(2*screen_halfy());
    float cx_deg = (region->center_x-screen_w()/2)/screen_pix_per_deg_x;
    float cy_deg = (region->center_y-screen_h()/2)/screen_pix_per_deg_y;
    float w_deg = region->plusminus_x/screen_pix_per_deg_x;
    float h_deg = region->plusminus_y/screen_pix_per_deg_y;
    
    float xpos = x()+w()/2+(cx_deg)/deg_per_pix_x;
    float ypos = y()+h()/2+(cy_deg)/deg_per_pix_y;
    float w = w_deg/deg_per_pix_x;
    float h = h_deg/deg_per_pix_y;
    
    // Draw light fill when inside (simulating transparency)
    if (region->state == WINDOW_IN) {
        fl_color(50, 100, 100);  // Dark cyan fill
        if (region->type == WINDOW_ELLIPSE) {
            fl_pie(xpos-w, ypos-h, 2*w, 2*h, 0.0, 360.0);
        } else {
            fl_rectf(xpos-w, ypos-h, 2*w, 2*h);
        }
    }
    
    // Draw the outline (always cyan for touch regions)
    fl_color(FL_CYAN);
    if (region->type == WINDOW_ELLIPSE) {
        fl_arc(xpos-w, ypos-h, 2*w, 2*h, 0.0, 360.0);
        fl_arc(xpos-2, ypos-2, 4, 4, 0.0, 360.0);
    } else {
        fl_rect(xpos-w, ypos-h, 2*w, 2*h);
    }
}  
  
  void draw_eye_status()
  {
    fl_color(FL_RED);
    float radius = 8;
    float xoffset = 8;
    float yoffset =14;
    float xpos;
    float ypos = y()+h()-yoffset;
    
    for (int i = 0; i < n_eye_regions; i++) {
      xpos = x()+xoffset+i*radius*1.4;
      if (eye_regions[i].active && eye_regions[i].state == WINDOW_IN) {
	fl_pie(xpos, ypos, radius, radius, 0.0, 360.0);
      }
      else {
	fl_arc(xpos, ypos, radius, radius, 0.0, 360.0);
      }
    }
  }

  void draw_touch_status()
  {
    fl_color(FL_CYAN);
    float radius = 8;
    float xoffset = w()-(n_touch_regions*1.4*radius);
    float yoffset =14;
    float xpos;
    float ypos = y()+h()-yoffset;
    
    for (int i = 0; i < n_touch_regions; i++) {
      xpos = x()+xoffset+i*radius*1.4;
      if (touch_regions[i].active && touch_regions[i].state == WINDOW_IN) {
	fl_pie(xpos, ypos, radius, radius, 0.0, 360.0);
      }
      else {
	fl_arc(xpos, ypos, radius, radius, 0.0, 360.0);
      }
    }
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

  void set_points_per_deg(float h, float v) {
    points_per_deg_x = h;
    points_per_deg_y = v;
    redraw();  
  }
  
  void draw_virtual_eye() {
    if (!virtual_eye_enabled || !virtual_eye.active) return;
    
    float xpos = x() + w()/2 + (virtual_eye.x/deg_per_pix_x);
    float ypos = y() + h()/2 - (virtual_eye.y/deg_per_pix_y);
    
    // Draw with orange color for virtual
    fl_color(virtual_eye.dragging ? FL_GREEN : fl_rgb_color(255, 140, 0));
    fl_circle(xpos, ypos, 8);
    
    // Draw crosshair
    fl_color(FL_BLACK);
    fl_line(xpos-6, ypos, xpos+6, ypos);
    fl_line(xpos, ypos-6, xpos, ypos+6);
    
    // Draw "V" indicator
    fl_color(fl_rgb_color(255, 140, 0));
    fl_font(FL_HELVETICA_BOLD, 10);
    fl_draw("V", xpos-3, ypos-12);
  }

  void show_touch(bool show) {
    touch_active = show;
    redraw();
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

  void draw_touch() {
    if (!touch_active) return;
    
    // Convert touch pixels back to degrees for drawing
    float screen_pix_per_deg_x = _screen_w / (2 * _screen_halfx);
    float screen_pix_per_deg_y = _screen_h / (2 * _screen_halfy);
    float deg_x = (touch_pix_x - _screen_w/2) / screen_pix_per_deg_x;
    float deg_y = -(touch_pix_y - _screen_h/2) / screen_pix_per_deg_y;
    
    float xpos = x() + w()/2 + (deg_x/deg_per_pix_x);
    float ypos = y() + h()/2 - (deg_y/deg_per_pix_y);
    
    // Draw diamond shape
    fl_color(FL_CYAN);
    int size = 6;
    
    fl_begin_polygon();
    fl_vertex(xpos, ypos - size);
    fl_vertex(xpos + size, ypos);
    fl_vertex(xpos, ypos + size);
    fl_vertex(xpos - size, ypos);
    fl_end_polygon();
  }

  void set_virtual_eye_enabled(bool enabled) {
    virtual_eye_enabled = enabled;
    if (enabled) {
      if (!virtual_eye.active) {
	// Initialize virtual eye at center
	virtual_eye.x = 0;
	virtual_eye.y = 0;
	virtual_eye.adc_x = 2048;
	virtual_eye.adc_y = 2048;
	virtual_eye.active = true;
      }
      redraw();
    } else {
      // Clean up when disabling
      virtual_eye.active = false;
      virtual_eye.dragging = false;
      redraw();
    }
  }
  
  void set_virtual_touch_enabled(bool enabled) {
    virtual_touch_enabled = enabled;
    if (enabled && !virtual_touch.active) {
      // Initialize virtual touch at center
      virtual_touch.x = 0;
      virtual_touch.y = 0;
      virtual_touch.active = true;  // Make it visible
      
      redraw();
    }
  }

  bool is_virtual_eye_enabled() const { return virtual_eye_enabled; }
  bool is_virtual_touch_enabled() const { return virtual_touch_enabled; }

// Virtual position getters
void get_virtual_eye_adc(int& x, int& y) const {
  x = virtual_eye.adc_x;
    y = virtual_eye.adc_y;
  }
  
  void get_virtual_touch_pos(int& x, int& y) const {
    x = virtual_touch.x;
    y = virtual_touch.y;
  }

  int handle(int event) FL_OVERRIDE {
    switch(event) {
    case FL_PUSH:
      return handle_mouse_down();
      
    case FL_DRAG:
      return handle_mouse_drag();
      
    case FL_RELEASE:
      return handle_mouse_up();
      
    case FL_LEAVE:
      handle_mouse_leave();
      return 1;
      
    case FL_MOVE:
      return 0;
      
    default:
      return Fl_Box::handle(event);
    }
  }
  
  EyeTouchWin(int X, int Y, int W, int H, const char*L=0) : Fl_Box(X,Y,W,H,L)
  {
    /* these will be updated using em/settings */
    points_per_deg_x = 8.0;
    points_per_deg_y = 8.0;
    
    xextent = 16*2;
    yextent = 16*2;
    xextent_2 = xextent/2;
    yextent_2 = yextent/2;
    em_pos_x = 0;
    em_pos_y = 0;
    em_radius = 0.75;
    flipx = false;
    flipy = false;

    for (int i = 0; i < n_eye_regions; i++) { eye_regions[i].active = false; }
    for (int i = 0; i < n_touch_regions; i++) { touch_regions[i].active = false; }
  }

    
  ~EyeTouchWin()
  {
  }
  
};



#endif

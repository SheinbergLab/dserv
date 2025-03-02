#ifndef VIRTUALJOYSTICK_HPP
#define VIRTUALJOYSTICK_HPP

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/fl_draw.H>
#include "MoveableCircle.hpp"

class VirtualJoystick: public MoveableCircle {
public:
  enum { CENTER=0, UP=1, DOWN=2, LEFT=4, RIGHT=8 };
  float joystick_pos[2];
  float xextent;
  float trigger_ecc;

protected:
  int state;
  int button_state;

  bool button_changed = false;
  bool state_changed = false;
  
  int handle(int e) FL_OVERRIDE {
    static int offset[2] = { 0, 0 };
    int ret = Fl_Box::handle(e);
    switch ( e ) {
    case FL_DRAG:
      position(offset[0]+Fl::event_x(), offset[1]+Fl::event_y());     // handle dragging
      parent()->redraw();
      update_joystick_pos();
      virtual_joystick_cb(this, NULL);
      return(1);
    case FL_RELEASE:
      {
	if (Fl::event_button() == FL_RIGHT_MOUSE) {
	  button_state = 0;
	  button_changed = true;
	  set_joystick_pos(0, 0);
	  virtual_joystick_cb(this, NULL);
	  parent()->redraw();
	}
	return(1);
      }
    case FL_PUSH:
      {
	if (Fl::event_button() == FL_RIGHT_MOUSE) {
	  button_state = 1;
	  button_changed = true;
	  virtual_joystick_cb(this, NULL);
	  parent()->redraw();
	  return(1);
	}
	else if (Fl::event_button() == FL_LEFT_MOUSE) {
	  offset[0] = x() - Fl::event_x();    // save where user clicked for dragging
	  offset[1] = y() - Fl::event_y();
	  
	  // Do we need to rearrange?
	  int last_ix = parent()->children() - 1;
	  MoveableCircle* last = (MoveableCircle*)parent()->child(last_ix);
	  if (last != this)
	    {
	      // Widgets get drawn in the order in which they were inserted.
	      // Remove this widget from the parent
	      parent()->remove(this);
	      // Re-add it at the bottom of the list
	      parent()->add(this);
	    }
	}
      }
      return(1);
    }
    return(ret);
  }
  
public:
  VirtualJoystick(int X, int Y, int W, int H, const char *l=NULL) : MoveableCircle(X,Y,W,H,l) {
    xextent = 10.0*2;
    trigger_ecc = 0.55 * (0.5*xextent);
    set_joystick_pos(0, 0);
    state = CENTER;
  }

  bool button_has_changed(void) { return button_changed; }
  
  int get_button_state(void) {
    button_changed = false;
    return button_state;
  }

  bool state_has_changed(void) { return state_changed; }

  int get_state(void) {
    state_changed = false;
    return state;
  }
    
  void update_joystick_pos(void)
  {
    float yextent = xextent*((float)parent()->h()/parent()->w());
    float deg_per_pix_x = xextent/(parent()->w());
    float deg_per_pix_y = yextent/(parent()->h());

    joystick_pos[0] = ((x()+w()/2)-parent()->x())*deg_per_pix_x-(xextent/2);
    joystick_pos[1] = -1.0*(((y()+h()/2)-parent()->y())*deg_per_pix_y-(yextent/2));

    update_joystick_state();
  }

  /* set the location of the virtual joystick to (x,y) (degrees) */
  void set_joystick_pos(float x, float y) {
    /* convert the desired position to pixel position within the parent window */
    float deg_per_pix_x = xextent/(parent()->w());
    float deg_per_pix_y = deg_per_pix_x*((float) parent()->h()/parent()->w());
    int xpos = parent()->x()+(parent()->w()/2.0)+x/deg_per_pix_x-w()/2;
    int ypos = parent()->y()+(parent()->h()/2.0)+(-1.0*y)/deg_per_pix_y-h()/2;

    /* store the position */
    joystick_pos[0] = x;
    joystick_pos[1] = y;

    update_joystick_state();
    
    /* move the circle and redraw */
    position(xpos, ypos);
    redraw();
  }

  /* check if state has changed */
  void update_joystick_state(void)
  {
    int newstate = 0;
    
    /* get left/right status */
    if (joystick_pos[0] > trigger_ecc) newstate |= RIGHT;
    else if (joystick_pos[0] < -trigger_ecc) newstate |= LEFT;

    /* get up/down status */
    if (joystick_pos[1] > trigger_ecc) newstate |= UP;
    else if (joystick_pos[1] < -trigger_ecc) newstate |= DOWN;

    if (newstate != state) {
      state_changed = true;
      state = newstate;
    }
  }
  
  virtual void draw() FL_OVERRIDE {
    fl_color(FL_CYAN);
    fl_pie(x(), y(), w(), h(), 0.0, 360.0);
  }
  
};

#endif

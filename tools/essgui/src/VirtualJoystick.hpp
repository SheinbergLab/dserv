#ifndef VIRTUALJOYSTICK_HPP
#define VIRTUALJOYSTICK_HPP

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/fl_draw.H>
#include "MoveableCircle.hpp"

class VirtualJoystick: public MoveableCircle {
public:
  float joystick_pos[2];
  float xextent;
  bool initialized;		// flag to allow setting upon new connections
protected:
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
	  set_joystick_pos(0, 0);
	  virtual_joystick_cb(this, NULL);
	  parent()->redraw();
	  return(1);
	case FL_PUSH:
	  {
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
	  return(1);
        }
        return(ret);
    }

public:
  VirtualJoystick(int X, int Y, int W, int H, const char *l=NULL) : MoveableCircle(X,Y,W,H,l) {
    xextent = 10.0*2;
    set_joystick_pos(0, 0);
  }

  void update_joystick_pos(void)
  {
    float yextent = xextent*((float)parent()->h()/parent()->w());
    float deg_per_pix_x = xextent/(parent()->w());
    float deg_per_pix_y = yextent/(parent()->h());

    joystick_pos[0] = ((x()+w()/2)-parent()->x())*deg_per_pix_x-(xextent/2);
    joystick_pos[1] = -1.0*(((y()+h()/2)-parent()->y())*deg_per_pix_y-(yextent/2));
  }

  /* set the location of the virtual eye to (x,y) (degrees) */
  void set_joystick_pos(float x, float y) {
    /* convert the desired position to pixel position within the parent window */
    float deg_per_pix_x = xextent/(parent()->w());
    float deg_per_pix_y = deg_per_pix_x*((float) parent()->h()/parent()->w());
    int xpos = parent()->x()+(parent()->w()/2.0)+x/deg_per_pix_x-w()/2;
    int ypos = parent()->y()+(parent()->h()/2.0)+(-1.0*y)/deg_per_pix_y-h()/2;

    /* store the position */
    joystick_pos[0] = x;
    joystick_pos[1] = y;

    /* move the circle and redraw */
    position(xpos, ypos);
    redraw();
  }

  virtual void draw() FL_OVERRIDE {
    fl_color(FL_CYAN);
    fl_pie(x(), y(), w(), h(), 0.0, 360.0);
  }
  
};

#endif

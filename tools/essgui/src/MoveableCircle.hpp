#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/fl_draw.H>

class MoveableCircle : public Fl_Box {
protected:
    int handle(int e) FL_OVERRIDE {
        static int offset[2] = { 0, 0 };
        int ret = Fl_Box::handle(e);
        switch ( e ) {
            case FL_DRAG:
	      position(offset[0]+Fl::event_x(), offset[1]+Fl::event_y());     // handle dragging
	      parent()->redraw();
	      return(1);
	case FL_RELEASE:
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
  MoveableCircle(int X, int Y, int W, int H, const char *l=NULL) : Fl_Box(X,Y,W,H,l) {

  }

  virtual void draw() FL_OVERRIDE {
    fl_color(FL_GREEN);
    fl_pie(x(), y(), w(), h(), 0.0, 360.0);
  }

  
};

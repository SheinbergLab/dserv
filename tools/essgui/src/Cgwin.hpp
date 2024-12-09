#ifndef CGWIN_HPP
#define CGWIN_HPP

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

#include <df.h>
#include <dynio.h>

#include <cgraph.h>
#include <gbuf.h>

static const int NColorVals = 18;
static float colorvals[] = {
  /* R    G    B   Grey -- currently we use the grey approx. */
  0.0, 0.0, 0.0, 0.0, 
  0.1, 0.1, 0.4, 0.4,
  0.0, 0.35, 0.0, 0.1,
  0.0, 0.7, 0.7, 0.7,
  0.8, 0.05, 0.0, 0.3,
  0.8, 0.0, 0.8, 0.0,
  0.0, 0.0, 0.0, 0.0,
  0.0, 0.0, 0.0, 0.0,
  0.7, 0.7, 0.7, 0.7,
  0.3, 0.45, 0.9, 0.0,
  0.05, 0.95, 0.1, 0.0,
  0.0, 0.9, 0.9, 0.9,
  0.0, 0.0, 0.0, 0.0,
  0.0, 0.0, 0.0, 0.0,
  0.94, 0.94, 0.05, 0.8,
  0.0, 0.0, 0.0, 0.2,
  1.0, 1.0, 1.0, 1.0,
  0.96, 0.96, 0.96, 0.96,
};

class CGWin;
static CGWin *currentCG;

class CGWin : public Fl_Box {
private:
  FRAME *frame;
  GBUF_DATA *gbuf;
  int linestyle;
  int clipregion[4];
  bool initialized;
public:

  virtual void draw() FL_OVERRIDE {

    /* we waited to intialized the gb buffer until the window is mapped */
    if (!initialized) init();
    
    setresol (currentCG->w(), currentCG->h());
    setfviewport(0,0,1,1);
    setwindow(0, 0, currentCG->w()-1, currentCG->h()-1);
    fl_color(FL_WHITE);
    fl_rectf(currentCG->x(),currentCG->y(),currentCG->w(),currentCG->h());

    gbPlaybackGevents();
  }
  
  GBUF_DATA *getGbuf(void) { return gbuf; }
  FRAME *getFrame(void) { return frame; }
  
  static int Clearwin(void) {
    //    currentCG->window()->make_current();
    fl_color(FL_WHITE);// should be background color
    fl_rectf(currentCG->x(), currentCG->y(), currentCG->w(), currentCG->h());
    return 0;
  }
  
  static int Line(float x0, float y0, float x1, float y1) 
  {
    //    currentCG->window()->make_current();
    fl_line(currentCG->x()+x0, currentCG->y()+currentCG->h()-y0,
	    currentCG->x()+x1, currentCG->y()+currentCG->h()-y1);
    return 0;
  }

  static int Point(float x, float y)
  {
    //    currentCG->window()->make_current();
    fl_point(currentCG->x()+x, currentCG->y()+y);
    return 0;
  }

  static int Char(float x, float y, char *string)
  {
    FRAME *f = getframe();
    y = f->ysres - y;
    fl_draw(f->orientation*90, string, currentCG->x()+x, currentCG->y()+y);
    return 0;
  }

  static int Text(float x, float y, char *string)
  {
    FRAME *f = getframe();
    y = f->ysres - y;
    int dx, dy, W, H;

    fl_text_extents(string, dx, dy, W, H);  // get width and height of string
    
    float voff = 0, hoff = 0;
    int ori;
    
    ori = getframe()->orientation;
    if (ori == 0 || ori == 2) {	/* horizontal */
      switch (getframe()->just) {
      case LEFT_JUST:     hoff = 0.0; break;
      case RIGHT_JUST:    hoff = W; break;
      case CENTER_JUST :  hoff = 0.5*W; break;
      }
      voff = (H*0.5);
    }
    else {
      switch (getframe()->just) {
      case LEFT_JUST:     voff = 0*W; break;
      case RIGHT_JUST:    voff = 1.*W; break;
      case CENTER_JUST :  voff = .5*W; break;
      }
      hoff = -(H*0.5);
    } 
    
    fl_draw(f->orientation*90, string, currentCG->x()+(x-hoff), currentCG->y()+(y+voff));
    return 0;
  }
  
  static int Setfont(char *fontname, float size)
  {
    Fl_Font font = FL_HELVETICA;
    std::string fname(fontname);
    if (fname == "SYMBOL")      font = FL_SYMBOL;
    if (fname == "HELVETICA")   font = FL_HELVETICA;
    if (fname == "TIMES")       font = FL_TIMES;
    if (fname == "SYMBOL")      font = FL_SYMBOL;
    if (fname == "ZAPF")        font = FL_ZAPF_DINGBATS;
    if (fname == "SCREEN")      font = FL_SCREEN;
    
    //    currentCG->window()->make_current();
    fl_font(font, size);
    return 0;
  }

  static int Strwidth(char *str)
  {
    int wi, hi;
    //    currentCG->window()->make_current();
    fl_measure(str, wi, hi);       // returns pixel width/height of string in current font
    return wi;
  }

  static int Strheight(char *str)
  {
    int wi, hi;
    //    currentCG->window()->make_current();
    fl_measure(str, wi, hi);       // returns pixel width/height of string in current font
    return hi;
  }
  

  static int Setcolor(int index)
  {
    int oldcolor = getcolor();
    if (index < NColorVals) {
      //      currentCG->window()->make_current();
      fl_color((uchar) (colorvals[index*4+0]*255),
	       (uchar) (colorvals[index*4+1]*255),
	       (uchar) (colorvals[index*4+2]*255));
    }
    else {
      unsigned int shifted = index >> 5;	/* RGB colors are store in bits 6-30 */
      uchar r, g, b;
      r = (shifted & 0xff0000) >> 16;
      g = (shifted & 0xff00) >> 8;
      b = (shifted & 0xff);
      //      currentCG->window()->make_current();
      fl_color(r, g, b);
    }
    return oldcolor;
  }

  static int FilledPolygon(float *verts, int nverts)
  {
    FRAME *f = getframe();

    //    currentCG->window()->make_current();
    if (nverts == 4) {
      fl_polygon(currentCG->x()+verts[0], currentCG->y()+f->ysres-verts[1],
		 currentCG->x()+verts[2], currentCG->y()+f->ysres-verts[3],
		 currentCG->x()+verts[4], currentCG->y()+f->ysres-verts[5],
		 currentCG->x()+verts[6], currentCG->y()+f->ysres-verts[7]);
      return 0;
    }
    if (nverts == 3) {
      fl_polygon(currentCG->x()+verts[0], currentCG->y()+f->ysres-verts[1],
		 currentCG->x()+verts[2], currentCG->y()+f->ysres-verts[3],
		 currentCG->x()+verts[4], currentCG->y()+f->ysres-verts[5]);
	return 0;
    }
    return 0;
  }

  static int Circle(float x, float y, float width, int filled)
  {
    //    currentCG->window()->make_current();
    FRAME *f = getframe();

    y = f->ysres-y;

    if (!filled) {
      fl_circle(currentCG->x()+x+width/2, currentCG->y()+y+width/2, width/2);
    }
    else {
      fl_begin_polygon();
      fl_arc(currentCG->x()+x+width/2, currentCG->y()+y+width/2, width/2, 0.0, 360.0);
      fl_end_polygon();
    }
    return 0;
  }

  void init(void)
  {
    setresol(w(), h());
    setwindow(0, 0, w()-1, h()-1);
    setfviewport(0, 0, 1, 1) ;
    gbInitGevents();
    initialized = true;
  }
  
  CGWin(int X, int Y, int W, int H, const char*L=0) : Fl_Box(X,Y,W,H,L) {
    currentCG = this;
    
    frame = (FRAME *) calloc(1, sizeof(FRAME));
    gbuf = (GBUF_DATA *) calloc(1, sizeof(GBUF_DATA));
    
    gbDisableGevents();				  /* valid data */
    gbInitGeventBuffer(gbuf);
    gbSetGeventBuffer(gbuf);
    gbEnableGevents();
    
    setline((LHANDLER) CGWin::Line);
    setclearfunc((HANDLER) CGWin::Clearwin);
    setpoint((PHANDLER) CGWin::Point);
    setcolorfunc((COHANDLER) CGWin::Setcolor);
    setchar((THANDLER) CGWin::Char);
    strwidthfunc((SWHANDLER) CGWin::Strwidth);
    strheightfunc((SHHANDLER) CGWin::Strheight);
    settext((THANDLER) CGWin::Text);
    setfontfunc((SFHANDLER) CGWin::Setfont);
    setfilledpoly((FHANDLER) CGWin::FilledPolygon);
    setcircfunc((CHANDLER) CGWin::Circle);

    initialized = false;
  }

  ~CGWin()
  {
    if (gbuf) free(gbuf);
    if (frame) free(frame);
  }
};



#endif

/*
 *       cgraph.h
 *       header file for c process control system graphics
 *       Created: 20-NOV-88
 *       By: Nikos K. Logothetis
 *       Edited: DLS (93-25)
 */

#ifndef __cgraph_h__
  #define __cgraph_h__

#ifdef __cplusplus
extern "C" {
#endif

#include <tcl.h>

typedef struct {
  int w;
  int h;
  int d;
  unsigned char *data;
  float x0, y0, x1, y1;
} GBUF_IMAGE;

typedef struct {
  int nimages;
  int maximages;
  int allocinc;
  GBUF_IMAGE *images;
} GBUF_IMAGES;

typedef struct {
  unsigned char *gbuf;
  int gbufindex;
  int gbufsize;
  GBUF_IMAGES images;
  int empty;
  char record_events;      // Per-buffer recording state
  char append_times;       // Per-buffer timing state  
  int event_time;          // Per-buffer event time
} GBUF_DATA;
  
typedef int (*HANDLER)();
typedef int (*LHANDLER)(float, float, float, float);	/* line */
typedef int (*PHANDLER)(float, float);	/* point */
typedef int (*THANDLER)(float, float, char *);	/* text */
typedef int (*FHANDLER)(float *, int);	/* filled polygon / polyline */
typedef int (*CHANDLER)(float, float, float, int);	/* circle */
typedef int (*LSHANDLER)(int) ;	/* set line style - from filled (1) to sparse
					dots (6) */
typedef int (*LWHANDLER)(int) ;	/* set line width, 1/100ths of pixels */
typedef int (*COHANDLER)(int) ;	/* set colour - 0 to 15 is EGA colours,
					above 32 is RGB values * 256 */
typedef int (*SWHANDLER)(char *) ;	/* string width - returns width in
						pixels of string */
typedef int (*SHHANDLER)(char *) ;	/* string height */
typedef int (*SOHANDLER)(int) ;	/* set string orientation - angle / pi */
typedef int (*SFHANDLER)(char *, float) ;	/* set font - name and size */
typedef int (*IMHANDLER)(float, float, float, float, char*);	/* image */
typedef int (*MIMHANDLER)(float, float, float, float, 
			  int, int, int, unsigned char*);	/* image */

typedef struct _FRAME           /* viewport/window graphics environment */
	{
	float xl;               /* Current screen xl-viewport coord */
	float yb;               /*                yb-viewport coord */
	float xr;               /*                xr-viewport coord */
	float yt;               /*                yt-viewport coord */
	float xul;              /* Current world window xul-window coord */
	float yub;              /*                      yub-window coord */
	float xur;              /*                      xur-window coord */
	float yut;              /*                      yut-window coord */
	float xs;               /* Scale values used in windowing(xr-xl) */
	float ys;               /*                                yt-yb  */
	float xus;              /*                               xur-xul */
	float yus;              /*                               yut-yub */
	float colsiz;           /* screen dots twixt text columns */
	float linsiz;           /* screen dots twixt text rows.   */
	char *fontname;         /* current font name              */
	float fontsize;         /* size of current font           */
	float xpos;             /* current x pos                  */
	float ypos;             /* current y pos                  */
	float xinc;             /* Screen dots between char dots  */
	float yinc;
	int grain;              /* Line jumps for dashed lines    */
	int lwidth;             /* Line width                     */
	int mode;               /* Window transform flag          */
	int clipf;              /* Clipping enable flag           */
	int just;               /* Current text justification     */
	int orientation;        /* Current text orientation       */
	int color;              /* Current drawing color          */
	int background_color;   /* Current background color       */
	HANDLER dclearfunc;     /* clear function pointer         */
	PHANDLER dclrpnt;       /* Code to clear a pixel          */
	PHANDLER dpoint;        /* Display code generator of point     */
	THANDLER dtext;         /*                           text      */
	THANDLER dchar;         /*                           character */
	LHANDLER dline;         /*                           line      */
	LHANDLER dclip;         /*                           clip      */
	FHANDLER dfilledpoly;   /* Filled polygon handler       */
	FHANDLER dpolyline;     /* Polyline handler             */
	CHANDLER dcircfunc;     /* Circle handler               */
	LSHANDLER dlinestyle ;	/* Set line style function	*/
	LWHANDLER dlinewidth ;	/* Set line width function	*/
	COHANDLER dsetcolor ;	/* Set drawing colour function	*/
	COHANDLER dsetbg ;	/* Set background colour function	*/
	SWHANDLER dstrwidth ;	/* Get string width function	*/
	SHHANDLER dstrheight ;	/* Get string height function	*/
	SOHANDLER dsetorient ;	/* Set string orientation fn	*/
	SFHANDLER dsetfont ;	/* Set text font function	*/
	IMHANDLER dimage ;	/* Draw an image file    	*/
	MIMHANDLER dmimage ;	/* Draw an in-memory image    	*/
	float wx1;              /* Working store                */
	float wy1;              /*                              */
	float wx2;              /*                              */
	float wy2;              /*                              */
	float c1;               /*                              */
	float c2;               /*                              */
	float xsres;            /* Current screen x-resolution  */
	float ysres;            /*                y-resolution  */
	struct _FRAME *parent;  /* pointer to frame to grestore */
	} FRAME;

/* viewport stack structure for pushing & popping viewports */
typedef struct {
  int size;
  int index;
  int increment;
  float *vals;
} VW_STACK;

#define VW_SIZE(v)             ((v)->size)
#define VW_INDEX(v)            ((v)->index)
#define VW_INC(v)              ((v)->increment)
#define VW_VIEWPORTS(v)        ((v)->vals)

/* Complete per-interpreter context */
typedef struct CgraphContext {
  FRAME *current_frame;           /* Current frame */
  FRAME default_frame;            /* Default frame template */
  
  /* Global handlers */
  HANDLER bframe;
  HANDLER eframe;
  
  GBUF_DATA gbuf_data;           /* Embedded gbuf data - not pointer */
  int gbuf_initialized;          /* Track initialization */
  
  /* Drawing state */
  float barwidth;
  int img_preview;
  
  /* Viewport stack */
  VW_STACK *viewport_stack;
  
  /* Static buffers and state */
  char draw_buffer[80];           /* For drawnum, drawfnum, drawf */
  char old_fontname[64];          /* For setfont */
  int labeltick;                  /* For tck() */
  
  /* Temporary variables */
  float temp_float;               /* For SWAP macro */
} CgraphContext;
  
/****************************************************************/
/*                        IO Event Stuff                        */
/****************************************************************/

typedef struct {
  char type;
  int keypress;
  int keymask;
  int mousex;
  int mousey;
  int buttons[3];
  unsigned int window;
} IOEVENT;

#define IO_EVENT_TYPE(e)     ((e)->type)
#define IO_EVENT_KEY(e)      ((e)->keypress)
#define IO_EVENT_KEYMASK(e)  ((e)->keymask)
#define IO_EVENT_MOUSE_X(e)  ((e)->mousex)
#define IO_EVENT_MOUSE_Y(e)  ((e)->mousey)
#define IO_EVENT_B1(e)       (((e)->buttons)[0])
#define IO_EVENT_B2(e)       (((e)->buttons)[1])
#define IO_EVENT_B3(e)       (((e)->buttons)[2])
#define IO_EVENT_BUTTONS(e)  ((e)->buttons)
#define IO_EVENT_WINDOW(e)   ((e)->window)

/* Event Types */
#define IO_NO_EVENT    0
#define IO_KEY_EVENT   1
#define IO_MOUSE_EVENT 2

/* Text justification constants */
#define  LEFT_JUST     (-1)
#define  CENTER_JUST    (0)
#define  RIGHT_JUST     (1)

#define DIALOG_WIN 101
#define CGRAPH_WIN 102

#define KEYPRESS 101
#define MOUSEPRESS 102
#define EXPOSE   103

#define HOME(f) {f->xpos=f->xl;f->ypos=f->yt;f->ypos-=f->linsiz;f->xpos++;}
#define NXTLIN(f) {f->ypos -= f->linsiz; if(f->ypos < f->yb && f->clipf) f->ypos = f->yt;}
#define LEFTMARG(f) {f->xpos = f->xl;}
#define INMARGIN(f) ((f->xpos+f->colsiz) >= f->xr && f->clipf)
#define NXTCOL(f) {f->xpos+=f->colsiz;if(INMARGIN(f)){LEFTMARG(f);NXTLIN(f);}}

#define XUNIT(f) (f->xus/f->xs)
#define YUNIT(f) (f->yus/f->ys)

#define TXT_HORIZONTAL 0
#define TXT_VERTICAL   1

#if !defined(__FPOINT__)
#define __FPOINT__
typedef struct {
  float x;
  float y;
} FPOINT;
#endif

/* Context management functions */
CgraphContext *CgraphCreateContext(Tcl_Interp *interp);
CgraphContext *CgraphGetContext(Tcl_Interp *interp);

/*
 * Video handlers
 */
extern void noplot(void);
extern void dotat(CgraphContext *ctx, float, float);
extern void BigDotAt(CgraphContext *ctx, float, float);
extern void SquareAt(CgraphContext *ctx, float, float);
extern void TriangleAt(CgraphContext *ctx, float, float);
extern void HbarsAt(CgraphContext *ctx, float, float);
extern void VbarsAt(CgraphContext *ctx, float, float);

/*
 * Marker-like functions
 */
extern void square(CgraphContext *ctx, float x, float y, float scale);
extern void fsquare(CgraphContext *ctx, float x, float y, float scale);
extern void circle(CgraphContext *ctx, float xarg, float yarg, float scale);
extern void fcircle(CgraphContext *ctx, float xarg, float yarg, float scale);
extern void vtick(CgraphContext *ctx, float xarg, float yarg, float scale);
extern void vtick_up(CgraphContext *ctx, float xarg, float yarg, float scale);
extern void vtick_down(CgraphContext *ctx, float xarg, float yarg, float scale);
extern void htick(CgraphContext *ctx, float xarg, float yarg, float scale);
extern void htick_left(CgraphContext *ctx, float xarg, float yarg, float scale);
extern void htick_right(CgraphContext *ctx, float xarg, float yarg, float scale);
extern void plus(CgraphContext *ctx, float xarg, float yarg, float scale);
extern void triangle(CgraphContext *ctx, float x, float y, float scale);
extern void diamond(CgraphContext *ctx, float x, float y, float scale);

/*
 * CGRAPH - functions
 */
extern int cgPS_Preview;	/* Render ps images using gs? */

extern FRAME *gsave(CgraphContext *ctx);
extern FRAME *grestore(CgraphContext *ctx);

extern void pushviewport(CgraphContext *ctx);
extern int popviewport(CgraphContext *ctx);
extern void poppushviewport(CgraphContext *ctx);
extern void seteframe(CgraphContext *ctx, HANDLER clearfunc);
extern void setbframe(CgraphContext *ctx, HANDLER clearfunc);
extern void setresol(CgraphContext *ctx, float, float);
extern void getresol(CgraphContext *ctx, float *, float *);
extern float getxscale(CgraphContext *ctx);
extern float getyscale(CgraphContext *ctx);
extern void getviewport(CgraphContext *ctx, float *, float *, float *, float *);
extern HANDLER setclearfunc(CgraphContext *ctx, HANDLER);
extern PHANDLER setpoint(CgraphContext *ctx, PHANDLER);
extern PHANDLER setclrpnt(CgraphContext *ctx, PHANDLER);
extern LHANDLER setclipfunc(CgraphContext *ctx, LHANDLER);
extern THANDLER settext(CgraphContext *ctx, THANDLER);
extern THANDLER setchar(CgraphContext *ctx, THANDLER);
extern LHANDLER setline(CgraphContext *ctx, LHANDLER);
extern FHANDLER setfilledpoly(CgraphContext *ctx, FHANDLER fillp);
extern FHANDLER setpolyline(CgraphContext *ctx, FHANDLER polyl);
extern CHANDLER setcircfunc(CgraphContext *ctx, CHANDLER);
extern LSHANDLER setlstylefunc(CgraphContext *ctx, LSHANDLER);
extern LWHANDLER setlwidthfunc(CgraphContext *ctx, LWHANDLER);
extern COHANDLER setcolorfunc(CgraphContext *ctx, COHANDLER);
extern COHANDLER setbgfunc(CgraphContext *ctx, COHANDLER);
extern SWHANDLER strwidthfunc(CgraphContext *ctx, SWHANDLER);
extern SHHANDLER strheightfunc(CgraphContext *ctx, SHHANDLER);
extern SOHANDLER setorientfunc(CgraphContext *ctx, SOHANDLER);
extern SFHANDLER setfontfunc(CgraphContext *ctx, SFHANDLER);
extern IMHANDLER setimagefunc(CgraphContext *ctx, IMHANDLER);
extern MIMHANDLER setmemimagefunc(CgraphContext *ctx, MIMHANDLER);
extern int setuser(CgraphContext *ctx, int);
extern void postscript(CgraphContext *ctx, char *, float, float);
extern int place_image(CgraphContext *ctx, int w, int h, int d, unsigned char *data, 
                       float xsize, float ysize);
extern int replace_image(CgraphContext *ctx, int ref, int w, int h, int d, unsigned char *data);
extern int setimgpreview(CgraphContext *ctx, int);
extern void group(CgraphContext *ctx);
extern void ungroup(CgraphContext *ctx);
extern int setbackgroundcolor(CgraphContext *ctx, int);
extern int setcolor(CgraphContext *ctx, int);
extern int getcolor(CgraphContext *ctx);
extern void clearscreen(CgraphContext *ctx);
extern int getbackgroundcolor(CgraphContext *ctx);
extern int setgrain(CgraphContext *ctx, int);
extern int setlstyle(CgraphContext *ctx, int);
extern int setlwidth(CgraphContext *ctx, int);
extern int strwidth(CgraphContext *ctx, char *);
extern int strheight(CgraphContext *ctx, char *);
extern float setfontsize(CgraphContext *ctx, float);
extern char *setfont(CgraphContext *ctx, char *, float size);
extern float setsfont(CgraphContext *ctx, char *, float size);
extern float getfontsize(CgraphContext *ctx);
extern char *getfontname(CgraphContext *ctx);
extern int setorientation(CgraphContext *ctx, int);
extern int getorientation(CgraphContext *ctx);
extern int setjust(CgraphContext *ctx, int);
extern int setclip(CgraphContext *ctx, int);
extern void setclipregion(CgraphContext *ctx, float, float, float, float);
extern int getclip(CgraphContext *ctx);
extern void setchrsize(CgraphContext *ctx, float, float);
extern void setviewport(CgraphContext *ctx, float, float, float, float);
extern void setfviewport(CgraphContext *ctx, float, float, float, float);
extern void setpviewport(CgraphContext *ctx, float, float, float, float);
extern void setwindow(CgraphContext *ctx, float, float, float, float);
extern void getwindow(CgraphContext *ctx, float *xul, float *yub, float *xur, float *yut);
extern float getuaspect(CgraphContext *ctx);
extern FRAME *setstatus(CgraphContext *ctx, FRAME *);
extern FRAME *setframe(CgraphContext *ctx, FRAME *);
extern FRAME *getframe(CgraphContext *ctx);
extern int code(FRAME *, float, float);  /* Stays the same - operates on frame directly */
extern void moveto(CgraphContext *ctx, float, float);
extern void lineto(CgraphContext *ctx, float, float);
extern void moverel(CgraphContext *ctx, float, float);
extern void linerel(CgraphContext *ctx, float, float);
extern void cleararea(CgraphContext *ctx, float, float, float, float);
extern void clearline(CgraphContext *ctx, float, float, float, float);
extern void rect(CgraphContext *ctx, float, float, float, float);
extern void filledrect(CgraphContext *ctx, float, float, float, float);
extern void filledpoly(CgraphContext *ctx, int, float *);
extern void polyline(CgraphContext *ctx, int, float *);
extern void drawtext(CgraphContext *ctx, char *);
extern void cleartext(CgraphContext *ctx, char *);
extern void drawtextf(CgraphContext *ctx, char *, ...);
extern void cleartextf(CgraphContext *ctx, char *, ...);
extern void drawchar(CgraphContext *ctx, int);
extern void drawclrchar(CgraphContext *ctx, int);
extern void drawnum(CgraphContext *ctx, char *, float);
extern void drawfnum(CgraphContext *ctx, int, float);
extern void drawclrnum(CgraphContext *ctx, char *fmt, float n);
extern void drawf(CgraphContext *ctx, char *, double);

/* GUTIL1 - functions */
extern void HitRetKey(void);
extern void beginframe(CgraphContext *ctx);
extern PHANDLER getpoint(CgraphContext *ctx);
extern float setwidth(CgraphContext *ctx, float w);
extern void copyframe(FRAME *from, FRAME *to);  /* Stays the same - operates on frames directly */
extern void endframe(CgraphContext *ctx);
extern void frame(CgraphContext *ctx);
extern void frameport(CgraphContext *ctx);
extern void gfill(CgraphContext *ctx, float xl, float yl, float xh, float yh);
extern int roundiv(int x, int y);  /* Stays the same - pure math */
extern void tck(CgraphContext *ctx, char *title);
extern void tickat(CgraphContext *ctx, float x, float y, char *title);
extern void viewmax(CgraphContext *ctx);
extern void screen(CgraphContext *ctx);
extern void user(CgraphContext *ctx);
extern void cross(CgraphContext *ctx);
extern void drawbox(CgraphContext *ctx, float xl, float yl, float xh, float yh);

/*
 * AXES - functions
 */
extern void axes(CgraphContext *ctx, char *, char *);
extern void boxaxes(CgraphContext *ctx, char *, char *);
extern void uboxaxes(CgraphContext *ctx);
extern void xaxis(CgraphContext *ctx, char *);
extern void yaxis(CgraphContext *ctx, char *);
extern void up_xaxis(CgraphContext *ctx, char *);
extern void right_yaxis(CgraphContext *ctx, char *);
extern int lxaxis(CgraphContext *ctx, float, float, int, char *);
extern int lyaxis(CgraphContext *ctx, float, float, int, char *);

extern int eventloop(HANDLER, IOEVENT *);
extern void window2screen(CgraphContext *ctx, int *px, int *py, float x, float y);
extern void screen2window(CgraphContext *ctx, int x, int y, float *px, float *py);
extern void window_to_screen(CgraphContext *ctx, float x, float y, int *px, int *py);
extern void screen_to_window(CgraphContext *ctx, int x, int y, float *px, float *py);
extern void maketitle(CgraphContext *ctx, char *, float, float);
extern void makeftitle(CgraphContext *ctx, char *, float, float);

#ifdef __cplusplus
}
#endif

#endif /* __cgraph_h__ */

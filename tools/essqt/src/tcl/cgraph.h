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

extern FRAME *contexp;


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

extern void Cgraph_InitInterp(Tcl_Interp *interp);
extern void Cgraph_SetInterp(Tcl_Interp *interp);

/*
 * Video handlers
 */
extern void noplot(void);
extern void dotat(float, float);
extern void BigDotAt(float, float);
extern void SquareAt(float, float);
extern void TriangleAt(float, float);
extern void HbarsAt(float, float);
extern void VbarsAt(float, float);

/*
 * Marker-like functions
 */
extern void square(float x, float y, float scale);
extern void fsquare(float x, float y, float scale);
extern void circle(float xarg, float yarg, float scale);
extern void fcircle(float xarg, float yarg, float scale);
extern void vtick(float xarg, float yarg, float scale);
extern void vtick_up(float xarg, float yarg, float scale);
extern void vtick_down(float xarg, float yarg, float scale);
extern void htick(float xarg, float yarg, float scale);
extern void htick_left(float xarg, float yarg, float scale);
extern void htick_right(float xarg, float yarg, float scale);
extern void plus(float xarg, float yarg, float scale);
extern void triangle(float x, float y, float scale);
extern void diamond(float x, float y, float scale);

/*
 * CGRAPH - functions
 */

extern int cgPS_Preview;	/* Render ps images using gs? */

extern FRAME *gsave(void);
extern FRAME *grestore(void);

void pushviewport(void);
int popviewport(void);
void poppushviewport(void);
void seteframe(HANDLER clearfunc);
void setbframe(HANDLER clearfunc);
extern void setresol(float, float);
extern void getresol(float *, float *);
extern float getxscale(void);
extern float getyscale(void);
extern void getviewport(float *, float *, float *, float *);
extern HANDLER  setclearfunc(HANDLER);
extern PHANDLER setpoint(PHANDLER);
extern PHANDLER setclrpoint(PHANDLER);
extern LHANDLER setclipfunc(LHANDLER);
extern THANDLER settext(THANDLER);
extern THANDLER setchar(THANDLER);
extern LHANDLER setline(LHANDLER);
extern FHANDLER setfilledpoly(FHANDLER fillp);
extern FHANDLER setpolyline(FHANDLER polyl);
extern CHANDLER setcircfunc(CHANDLER);
extern LSHANDLER setlstylefunc(LSHANDLER) ;
extern LWHANDLER setlwidthfunc(LWHANDLER) ;
extern COHANDLER setcolorfunc(COHANDLER) ;
extern COHANDLER setbgfunc(COHANDLER) ;
extern SWHANDLER strwidthfunc(SWHANDLER) ;
extern SHHANDLER strheightfunc(SHHANDLER) ;
extern SOHANDLER setorientfunc(SOHANDLER) ;
extern SFHANDLER setfontfunc(SFHANDLER) ;
extern IMHANDLER setimagefunc(IMHANDLER) ;
extern MIMHANDLER setmemimagefunc(MIMHANDLER) ;
extern int  setuser(int);
extern void postscript(char *, float, float);
extern int place_image(int w, int h, int d, unsigned char *data, 
		       float xsize, float ysize);
extern int replace_image(int ref, int w, int h, int d, unsigned char *data);
extern int setimgpreview(int);
extern void group(void);
extern void ungroup(void);
extern int  setcolor(int);
extern int  getcolor(void);
extern void clearscreen(void);
extern int  setbackgroundcolor(int);
extern int  getbackgroundcolor(void);
extern int  setgrain(int);
extern int  setlstyle(int);
extern int  setlwidth(int);
extern int strwidth(char *);
extern int strheight(char *);
extern float setfontsize(float);
extern char *setfont(char *, float size);
extern float setsfont(char *, float size);
extern float getfontsize();
extern char *getfontname();
extern int  setorientation(int);
extern int getorientation(void) ;
extern int  setjust(int);
extern int  setclip(int);
extern void  setclipregion(float, float, float, float);
extern int  getclip(void);
extern void setchrsize(float, float);
extern void setviewport(float, float, float, float);
extern void setfviewport(float, float, float, float);
extern void setpviewport(float, float, float, float);
extern void setwindow(float, float, float, float);
extern void getwindow(float  *xul, float *yub, float *xur, float *yut);
extern float getuaspect(void);
extern FRAME *setstatus(FRAME *);
extern FRAME *setframe(FRAME *);
extern FRAME *getframe(void);
extern int code(FRAME *, float, float);
extern void moveto(float, float);
extern void lineto(float, float);
extern void moverel(float, float);
extern void linerel(float, float);
extern void cleararea(float, float, float, float);
extern void clearline(float, float, float, float);
extern void rect(float, float, float, float);
extern void filledrect(float, float, float, float);
extern void filledpoly(int, float *);
extern void polyline(int, float *);
extern void drawtext(char *);
extern void cleartext(char *);
extern void drawtextf(char *, ...);
extern void cleartextf(char *, ...);
extern void drawchar(int);
extern void drawclrchar(int);
extern void drawnum(char *, float);
extern void drawfnum(int, float);
extern void drawclrnum (char *fmt, float n);
extern void drawf(char *, double);

/* GUTIL1 - functions
*/
extern void HitRetKey(void);
extern void beginframe(void);
extern PHANDLER getpoint();
extern float setwidth(float w);
extern void copyframe(FRAME *from, FRAME *to);
extern void endframe(void);
extern void frame(void);
extern void frameport(void);
extern void gfill(float xl, float yl, float xh, float yh);
extern int roundiv(int x, int y);
extern void tck(char *title);
extern void tickat(float x, float y, char *title);
extern void viewmax(void);
extern void screen(void);
extern void user(void);
extern void cross(void);
extern void drawbox(float xl, float yl, float xh, float yh);


/*
 * AXES - functions
 */
extern void axes (char *, char *);
extern void boxaxes (char *, char *);
extern void uboxaxes (void);
extern void xaxis (char *);
extern void yaxis (char *);
extern void up_xaxis (char *);
extern void right_yaxis (char *);
extern int lxaxis(float, float, int, char *);
extern int lyaxis(float, float, int, char *);

extern int eventloop(HANDLER, IOEVENT *);
extern void window2screen(int *px, int *py, float x, float y);
extern void screen2window(int x, int y, float *px, float *py);

extern void window_to_screen(float x, float y, int *px, int *py);
extern void screen_to_window(int x, int y, float *px, float *py);

extern void maketitle(char *, float, float);
extern void makeftitle(char *, float, float);


#ifdef __cplusplus
}
#endif

#endif /* __cgraph_h__ */

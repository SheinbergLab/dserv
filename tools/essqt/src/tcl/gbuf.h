/**************************************************************************/
/*      gbuf.h - graphics buffer package for use with cgraph              */
/*      Created: 4-Apr-94                                                 */
/**************************************************************************/

#ifndef _GBUF_H_
#define _GBUF_H_

#ifdef __cplusplus
extern "C" {
#endif

extern char RecordGEvents;

typedef enum { GBUF_RAW = 1, GBUF_ASCII, GBUF_AI88, 
	       GBUF_AI3, GBUF_AI = GBUF_AI3, 
	       GBUF_PS, GBUF_FIG, GBUF_EPS, GBUF_PDF } GbufDumpTypes;


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
  char record_events;  // Per-buffer recording state
} GBUF_DATA;

#define GB_RECORD_EVENTS(g)  ((g)->record_events)
#define GB_GBUF(g)          ((g)->gbuf)
#define GB_GBUFSIZE(g)      ((g)->gbufsize)
#define GB_GBUFINDEX(g)     ((g)->gbufindex)
#define GB_IMAGES(g)        ((g)->images)
#define GB_EMPTY(g)         ((g)->empty)
  
/*
 *  POSTSCRIPT Support
 */

#define PS_AUTOMATIC 0
#define PS_LANDSCAPE 1
#define PS_PORTRAIT  2

extern char PS_Orientation;

/* 
 *  EVENT IDS
 */

#define G_HEADER      (0)
#define G_POINT       (1+G_HEADER)
#define G_LINE        (1+G_POINT)
#define G_TEXT        (1+G_LINE)
#define G_COLOR       (1+G_TEXT)
#define G_LSTYLE      (1+G_COLOR)
#define G_ORIENTATION (1+G_LSTYLE)
#define G_LINETO      (1+G_ORIENTATION)
#define G_MOVETO      (1+G_LINETO)
#define G_JUSTIFICATION (1+G_MOVETO)
#define G_FONT        (1+G_JUSTIFICATION)
#define G_SAVE        (1+G_FONT)
#define G_CLIP        (1+G_SAVE)
#define G_FILLEDRECT  (1+G_CLIP)
#define G_TIMESTAMP   (1+G_FILLEDRECT)
#define G_GROUP       (1+G_TIMESTAMP)
#define G_CIRCLE      (1+G_GROUP)
#define G_FILLEDPOLY  (1+G_CIRCLE)
#define G_POLY        (1+G_FILLEDPOLY)
#define G_LWIDTH      (1+G_POLY)
#define G_POSTSCRIPT  (1+G_LWIDTH)
#define G_IMAGE       (1+G_POSTSCRIPT)

typedef struct _g_header {
  float version;
  float width;
  float height;
} GHeader;

#define G_VERSION(h) ((h)->version)
#define G_WIDTH(h)   ((h)->width)
#define G_HEIGHT(h)  ((h)->height)

typedef struct _g_point{
   float x, y;
} GPoint;

#define GPOINT_X(l)  ((l)->x)
#define GPOINT_Y(l)  ((l)->y)

typedef struct _g_pointlist{
  int n;
  float *points;
} GPointList;

#define GPOINTLIST_N(l)    ((l)->n)
#define GPOINTLIST_PTS(l)  ((l)->points)

typedef struct _g_line{
   float x0, y0, x1, y1;
} GLine;

#define GLINE_X0(l)  ((l)->x0)
#define GLINE_X1(l)  ((l)->x1)
#define GLINE_Y0(l)  ((l)->y0)
#define GLINE_Y1(l)  ((l)->y1)

typedef struct _g_text {
   float x, y;
   int length;
   char *str;
} GText;

#define GTEXT_X(t)       ((t)->x)
#define GTEXT_Y(t)       ((t)->y)
#define GTEXT_LENGTH(t)  ((t)->length)
#define GTEXT_STRING(t)  ((t)->str)

typedef struct _g_attr{
   int val;
} GAttr;

#define GATTR_VAL(a)  ((a)->val)

/*
 * bytes per event
 */

#define GHEADER_S     ((int) (sizeof (GHeader)))
#define GPOINT_S      ((int) (sizeof (GPoint)))
#define GLINE_S       ((int) (sizeof (GLine)))
#define GATTR_S       ((int) (sizeof (GAttr)))

/* 
 * For 64bit compatibility we need to hard code these to pick a standard
 * size independent of pointer size (which is, of course, larger on 64 bit).
 * In this case, when we read/write these structs, the pointer contents is
 * not meaningful anyway, and will be replaced with allocated return values
 */
/*
#define GPOINTLIST_S  ((int) (sizeof (GPointList)))
#define GTEXT_S       ((int) (sizeof (GText)))
*/
#define GPOINTLIST_S  (8)      /* sizeof(int)+32bit */
#define GTEXT_S       (16)     /* sizeof(float)*2+sizeof(int)+32bit */


/*
 * External functions
 */

GBUF_DATA *gbInitGeventBuffer(GBUF_DATA *gb);
GBUF_DATA *gbSetGeventBuffer(GBUF_DATA *);
GBUF_DATA *gbGetGeventBuffer();

void gbInitGevents();
void gbResetGevents();
void gbCloseGevents();
void gbRecordDefaults();

void gbEnableGeventBuffer(GBUF_DATA *gb);
void gbDisableGeventBuffer(GBUF_DATA *gb);
void gbEnableCurrentBuffer();
void gbDisableCurrentBuffer();
void gbResetCurrentBuffer();
void gbResetGeventBuffer(GBUF_DATA *gb);
void gbCleanupGeventBuffer(GBUF_DATA *gb);
void gbFreeImagesBuffer(GBUF_DATA *gb);
int gbIsRecordingEnabled();
  
int gbAddImage(int w, int h, int d, unsigned char *data,
	       float x0, float y0, float x1, float y1);
GBUF_IMAGE *gbFindImage(int ref);
int gbReplaceImage(int ref, int w, int h, int d, unsigned char *data);
int  gbWriteGevents(char *, int);
int gbWriteImageFile(FILE *fp);
int gbReadImageFile(FILE *fp);
void gbPrintGevents();
void gbEnableGevents();
void gbDisableGevents();
void gbEnableGeventTimes();
void gbDisableGeventTimes();
int  gbSetTime(int time);
int  gbIncTime(int time);
int  gbPlaybackGevents();
int  gbClearAndPlayback(void);
void gbSetPageOrientation(char ori);
void gbSetPageFill(int fill);
int  gbIsEmpty();
  
void record_gline(char, float, float, float, float);
void record_gpoint(char, float, float);
void record_gpoly(char, int, float *);
void record_gtext(char, float, float, char *);
void record_gattr(char, int);
void gbuf_dump(char *data, int nbytes, int type, FILE *outfp);
int  gbuf_dump_pdf(char *data, int nbytes, char *filename);

#ifdef __cplusplus
}
#endif

#endif




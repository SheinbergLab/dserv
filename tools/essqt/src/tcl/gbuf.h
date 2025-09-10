/**************************************************************************/
/*      gbuf.h - graphics buffer package for use with cgraph              */
/*      Created: 4-Apr-94                                                 */
/*      Refactored for thread safety - all functions now per-context      */
/**************************************************************************/

#ifndef _GBUF_H_
#define _GBUF_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct CgraphContext CgraphContext;

typedef enum { GBUF_RAW = 1, GBUF_ASCII, GBUF_AI88, 
	       GBUF_AI3, GBUF_AI = GBUF_AI3, 
	       GBUF_PS, GBUF_FIG, GBUF_EPS, GBUF_PDF } GbufDumpTypes;

/* Macros for accessing gbuf data within context */
#define GB_RECORD_EVENTS(ctx)  ((ctx)->gbuf_data.record_events)
#define GB_GBUF(ctx)          ((ctx)->gbuf_data.gbuf)
#define GB_GBUFSIZE(ctx)      ((ctx)->gbuf_data.gbufsize)
#define GB_GBUFINDEX(ctx)     ((ctx)->gbuf_data.gbufindex)
#define GB_IMAGES(ctx)        ((ctx)->gbuf_data.images)
#define GB_EMPTY(ctx)         ((ctx)->gbuf_data.empty)
#define GB_APPEND_TIMES(ctx)  ((ctx)->gbuf_data.append_times)
#define GB_EVENT_TIME(ctx)    ((ctx)->gbuf_data.event_time)
  
/*
 *  POSTSCRIPT Support
 */
#define PS_AUTOMATIC 0
#define PS_LANDSCAPE 1
#define PS_PORTRAIT  2

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
#define G_BACKGROUND  (1+G_IMAGE)

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
 * bytes per event - fixed for 64-bit compatibility
 */
#define GHEADER_S     ((int) (sizeof (GHeader)))
#define GPOINT_S      ((int) (sizeof (GPoint)))
#define GLINE_S       ((int) (sizeof (GLine)))
#define GATTR_S       ((int) (sizeof (GAttr)))
#define GPOINTLIST_S  (8)      /* sizeof(int)+32bit */
#define GTEXT_S       (16)     /* sizeof(float)*2+sizeof(int)+32bit */

/*
 * Buffer management functions - all take CgraphContext parameter
 */
void gbInitGeventBuffer(CgraphContext *ctx);
void gbResetGeventBuffer(CgraphContext *ctx);
  void gbCleanupGeventBuffer(CgraphContext *ctx);

/*
 * Recording control - per context
 */
void gbEnableGeventBuffer(CgraphContext *ctx);
void gbDisableGeventBuffer(CgraphContext *ctx);
int gbIsRecordingEnabled(CgraphContext *ctx);

/*
 * Buffer state - per context
 */
int gbIsEmpty(CgraphContext *ctx);
int gbSize(CgraphContext *ctx);
int gbSetEmpty(CgraphContext *ctx, int empty);

/*
 * Timing functions - per context (kept for compatibility)
 */
void gbEnableGeventTimes(CgraphContext *ctx);
void gbDisableGeventTimes(CgraphContext *ctx);
int gbSetTime(CgraphContext *ctx, int time);
int gbIncTime(CgraphContext *ctx, int time);

/*
 * Image management - per context
 */
int gbAddImage(CgraphContext *ctx, int w, int h, int d, unsigned char *data,
               float x0, float y0, float x1, float y1);
GBUF_IMAGE *gbFindImage(CgraphContext *ctx, int ref);
int gbReplaceImage(CgraphContext *ctx, int ref, int w, int h, int d, unsigned char *data);
void gbFreeImage(GBUF_IMAGE *image);  /* This one operates on image directly */
void gbFreeImagesBuffer(CgraphContext *ctx);
int gbWriteImageFile(CgraphContext *ctx, FILE *fp);
int gbReadImageFile(CgraphContext *ctx, FILE *fp);

/*
 * Buffer cleaning and output - per context
 */
int gbCleanGeventBuffer(CgraphContext *ctx);
int gbPlaybackGevents(CgraphContext *ctx);
int gbWriteGevents(CgraphContext *ctx, char *filename, int format);
void gbPrintGevents(CgraphContext *ctx);

/*
 * Recording functions - all take context parameter
 */
void record_gline(CgraphContext *ctx, char type, float x0, float y0, float x1, float y1);
void record_gpoint(CgraphContext *ctx, char type, float x, float y);
void record_gpoly(CgraphContext *ctx, char type, int nverts, float *verts);
void record_gtext(CgraphContext *ctx, char type, float x, float y, char *str);
void record_gattr(CgraphContext *ctx, char type, int val);

/*
 * Special functions
 */
void gbRecordDefaults(CgraphContext *ctx);

/*
 * Page setup functions - per context  
 */
void gbSetPageOrientation(CgraphContext *ctx, char ori);
void gbSetPageFill(CgraphContext *ctx, int fill);

/*
 * Legacy compatibility
 */
void gbCloseGevents(CgraphContext *ctx);  /* Wrapper around gbCleanupGeventBuffer */

/*
 * Utility functions for external dump utilities (these work on raw buffer data)
 */
extern void gbuf_dump(CgraphContext *ctx, char *buffer, int n, int type, FILE *fp);
extern int gbuf_dump_ps(CgraphContext *ctx, unsigned char *gbuf, int bufsize, int type, FILE *OutFP);

  extern int gbuf_dump_ascii(unsigned char *gbuf, int bufsize, FILE *fp);
extern char *gbuf_dump_ascii_to_string(unsigned char *data, int nbytes);
extern char *gbuf_dump_json_direct(unsigned char *data, int nbytes);
extern unsigned char *gbuf_clean(unsigned char *data, int nbytes, int *clean_size);

/* 
 * Playback functions - these operate on raw buffer data but need context for drawing
 */
extern void playback_gbuf(CgraphContext *ctx, unsigned char *data, int nbytes);
extern void playback_gfile(CgraphContext *ctx, FILE *fp);

#ifdef __cplusplus
}
#endif

#endif /* _GBUF_H_ */

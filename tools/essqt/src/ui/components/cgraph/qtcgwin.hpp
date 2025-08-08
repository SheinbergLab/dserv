#ifndef QTCGWIN_HPP
#define QTCGWIN_HPP

#include <QWidget>
#include <QPainter>
#include <QString>
#include <memory>

// Undefine any macros that might conflict
#ifdef LEFT_JUST
#undef LEFT_JUST
#endif
#ifdef CENTER_JUST
#undef CENTER_JUST
#endif
#ifdef RIGHT_JUST
#undef RIGHT_JUST
#endif

#include <tcl.h>

// Forward declaration
class QtCGManager;

#ifdef __cplusplus
extern "C" {
#endif

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

typedef struct __FRAME      /* viewport/window graphics environment */
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
	} FRAME_MINIMAL;
	

#ifdef __cplusplus
}
#endif
	
// Qt-based cgraph rendering widget
class QtCGWin : public QWidget {
    Q_OBJECT
    
public:
    QtCGWin(Tcl_Interp *interp, QWidget *parent = nullptr);
    ~QtCGWin();
    
    // Get the opaque graphics buffer pointer
    void* getGraphicsBuffer() { return gbuf; }
    
    // Force a redraw
    void refresh();
    
    void setBackgroundColor(const QColor& color); 
    
    // Export to PDF
    bool exportToPDF(const QString& filename);
    bool exportToPDFDialog(const QString& suggestedName = QString());
    
    // Text justification constants (matching cgraph.h values)
    enum Justification {
        JustLeft = -1,    // LEFT_JUST
        JustCenter = 0,   // CENTER_JUST
        JustRight = 1     // RIGHT_JUST
    };

    // Script setters
    void setMouseDownScript(const QString& script) { mouseDownScript = script; }
    void setMouseUpScript(const QString& script) { mouseUpScript = script; }
    void setMouseMoveScript(const QString& script) { mouseMoveScript = script; }
    void setMouseDoubleClickScript(const QString& script) { mouseDoubleClickScript = script; }
    void setMouseWheelScript(const QString& script) { mouseWheelScript = script; }
    void setKeyPressScript(const QString& script) { keyPressScript = script; }
    void setKeyReleaseScript(const QString& script) { keyReleaseScript = script; }
    void setFocusInScript(const QString& script) { focusInScript = script; }
    void setFocusOutScript(const QString& script) { focusOutScript = script; }
    
    // Helper to get current instance from manager
    static QtCGWin* getCurrentInstance();
    
signals:
    void graphUpdated();
    void mousePressed(QPointF pos);
    void mouseReleased(QPointF pos);
    void mouseMoved(QPointF pos);
    
protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    
private:
    Tcl_Interp *interp;
    FRAME_MINIMAL *frame;  // Now we can access frame fields directly
    void *gbuf;   // Opaque pointer to GBUF_DATA
    bool initialized;
    
    // Qt drawing context
    QPainter *currentPainter;
    QColor currentColor;
    QFont currentFont;
    int lineStyle;
    
    QColor backgroundColor;
    
    void init();

    // Mouse event scripts
    QString mouseDownScript;
    QString mouseUpScript;
    QString mouseMoveScript;
    QString mouseDoubleClickScript;
    QString mouseWheelScript;
    
    // Keyboard event scripts
    QString keyPressScript;
    QString keyReleaseScript;
    
    // Focus event scripts
    QString focusInScript;
    QString focusOutScript;
    
    // Convert Qt coordinates to cgraph coordinates
    QPointF toCGraphCoords(const QPointF& qtPoint) const {
        // cgraph uses bottom-left origin, Qt uses top-left
        return QPointF(qtPoint.x(), height() - qtPoint.y());
    }
    
    // Convert Qt key to string representation
    QString keyToString(QKeyEvent* event) const;
    
    // Drawing implementations
    static int Clearwin();
    static int Line(float x0, float y0, float x1, float y1);
    static int Point(float x, float y);
    static int Char(float x, float y, char *string);
    static int Text(float x, float y, char *string);
    static int Setfont(char *fontname, float size);
    static int Strwidth(char *str);
    static int Strheight(char *str);
    static int Setcolor(int index);
    static int FilledPolygon(float *verts, int nverts);
    static int Circle(float x, float y, float width, int filled);
    
    QString substituteEventData(const QString& script, QEvent* event, 
                                const QPointF& pos = QPointF(), 
                                int button = -1, int delta = 0) const;
    
    friend class QtCGWinBridge;  // Allow the bridge to set callbacks and access private members
};

#endif // QTCGWIN_HPP
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

// Minimal FRAME structure - just the fields we need
// This must match the layout in cgraph.h
struct FRAME_MINIMAL {
    float xl, yb, xr, yt;           // viewport coords
    float xul, yub, xur, yut;       // window coords
    float xs, ys, xus, yus;         // scale values
    float colsiz, linsiz;           // text spacing
    char *fontname;                 // current font
    float fontsize;                 // font size
    float xpos, ypos;               // current position
    float xinc, yinc;               // char spacing
    int grain;                      // line style
    int lwidth;                     // line width
    int mode;                       // transform flag
    int clipf;                      // clipping flag
    int just;                       // text justification
    int orientation;                // text orientation
    int color;                      // current color
    int background_color;           // background color
    // ... rest of the struct we don't need
};

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
    
    // Export to PDF
    bool exportToPDF(const QString& filename);
    bool exportToPDFDialog(const QString& suggestedName = QString());
    
    // Text justification constants (matching cgraph.h values)
    enum Justification {
        JustLeft = -1,    // LEFT_JUST
        JustCenter = 0,   // CENTER_JUST
        JustRight = 1     // RIGHT_JUST
    };
    
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
    
    void init();
    
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
    
    friend class QtCGWinBridge;  // Allow the bridge to set callbacks and access private members
};

#endif // QTCGWIN_HPP
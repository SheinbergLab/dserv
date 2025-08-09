#ifndef QTCGRAPH_HPP
#define QTCGRAPH_HPP

#include <QWidget>
#include <QString>
#include <QColor>
#include <tcl.h>

extern "C" {
#include <cgraph.h>
#include <gbuf.h>
}

class QtCGraph : public QWidget
{
    Q_OBJECT

public:
    // Simple constructor - always creates its own interpreter
    explicit QtCGraph(const QString& name = QString(), QWidget* parent = nullptr);
    virtual ~QtCGraph();

    // Identity
    QString name() const { return m_name; }

    // Current instance
    static QtCGraph* getCurrentInstance();
  
    // Tcl interface
    Tcl_Interp* interpreter() { return m_interp; }
    int eval(const QString& command);
    QString result() const;
    
    // Graphics operations
    void refresh();
    void clear();
    bool exportToPDF(const QString& filename);
    bool exportToPDFDialog(const QString& suggestedName = QString());
    
    // Configuration
    void setInitScript(const QString& script);
    void setBackgroundColor(const QColor& color);
    
    // Event binding
    void setMouseDownScript(const QString& script) { m_mouseDownScript = script; }
    void setMouseUpScript(const QString& script) { m_mouseUpScript = script; }
    void setMouseMoveScript(const QString& script) { m_mouseMoveScript = script; }
    void setMouseWheelScript(const QString& script) { m_mouseWheelScript = script; }
    void setKeyPressScript(const QString& script) { m_keyPressScript = script; }
    void setKeyReleaseScript(const QString& script) { m_keyReleaseScript = script; }
    void setMouseDoubleClickScript(const QString& script) { 
        m_mouseDoubleClickScript = script; 
    }
    void setFocusInScript(const QString& script) { m_focusInScript = script; }
    void setFocusOutScript(const QString& script) { m_focusOutScript = script; }

    // Graphics buffer access (for cgraph integration)
    void* getGraphicsBuffer() { return m_gbuf; }
    void setGraphicsBuffer(void* gbuf) { m_gbuf = gbuf; }
    
    // Frame access (read-only for coordinate transformation and text properties)
    FRAME* getFrame() { return m_frame; }
    void setFrame(FRAME* frame) { m_frame = frame; }

signals:
    void initialized();
    void graphUpdated();
    void commandExecuted(int result, const QString& output);
    void error(const QString& message);
    void mousePressed(const QPointF& pos);
    void mouseReleased(const QPointF& pos);
    void mouseMoved(const QPointF& pos);
    void mouseDoubleClicked(const QPointF& pos);

protected:
    // Qt event handling
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    
private slots:
    void initializeGraphics();
    
private:
    QString m_name;
    Tcl_Interp* m_interp;
    void* m_gbuf;
    FRAME* m_frame;  // Read-only access to frame properties
    bool m_initialized;
    QString m_initScript;
    QColor m_backgroundColor;
    
    // Event binding scripts
    QString m_mouseDownScript;
    QString m_mouseUpScript;
    QString m_mouseMoveScript;
    QString m_mouseDoubleClickScript;
    QString m_mouseWheelScript;
    QString m_keyPressScript;
    QString m_keyReleaseScript;
    QString m_focusInScript;
    QString m_focusOutScript;
    
    // For painting
    QPainter* m_currentPainter;
    QColor m_currentColor;
    
    // Private methods
    void initializeInterpreter();
    void cleanupGraphicsBuffer();
    void executeInitScript();
    QString substituteEventData(const QString& script, QEvent* event,
                               const QPointF& pos = QPointF(),
                               int button = -1, int delta = 0) const;
    QString keyToString(QKeyEvent* event) const;
    
    // Static callbacks for cgraph
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
    
    friend class QtCGraphBridge;
};

#endif // QTCGRAPH_HPP
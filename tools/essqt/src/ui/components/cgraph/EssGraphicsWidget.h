#ifndef ESSGRAPHICSWIDGET_H
#define ESSGRAPHICSWIDGET_H

#include "EssScriptableWidget.h"
#include <QWidget>
#include <QString>
#include <QColor>
#include <QStyle>
#include <QPainter>
#include <QToolBar>
#include <QAction>

#include <tcl.h>

extern "C" {
#include <cgraph.h>
#include <gbuf.h>
}

/**
 * @brief Graphics widget that combines EssScriptableWidget with cgraph functionality
 * 
 * This replaces QtCGraph while adding the scriptable widget development interface.
 * Provides both graphics drawing capabilities and script editor/testing tools.
 */
class EssGraphicsWidget : public EssScriptableWidget
{
    Q_OBJECT

public:
    // Layout modes for future Qt controls integration
    enum LayoutMode {
        GraphicsOnly,        // Just graphics area
        WithToolbar,         // Graphics + toolbar (current default)
        SideControls,        // Graphics + side control panel (future)
        BottomControls       // Graphics + bottom control panel (future)
    };
    Q_ENUM(LayoutMode)
    
    enum ControlPanelType {
        NoControls,
        ExperimentControls,  // Future: trial controls, stimulus settings
        PlotControls,        // Future: axis controls, data management  
        CustomControls       // Future: user-defined via Tcl
    };
    Q_ENUM(ControlPanelType)

    explicit EssGraphicsWidget(const QString& name = QString(), QWidget* parent = nullptr);
    virtual ~EssGraphicsWidget();

    // EssScriptableWidget interface
    QString getWidgetTypeName() const override { return "GraphicsWidget"; }

    // Layout management (stubs for future enhancement)
    void setLayoutMode(LayoutMode mode);
    LayoutMode layoutMode() const { return m_layoutMode; }
    
    void setControlPanelType(ControlPanelType type);
    ControlPanelType controlPanelType() const { return m_controlType; }
    
    void setControlsVisible(bool visible);
    bool controlsVisible() const { return m_controlsVisible; }

    // Graphics operations (preserve QtCGraph API)
    void refresh();
    void clear();
    bool exportToPDF(const QString& filename);
    bool exportToPDFDialog(const QString& suggestedName = QString());
    
    // Configuration
    void setBackgroundColor(const QColor& color);
    QColor backgroundColor() const { return m_backgroundColor; }
    
    // Event binding (preserve QtCGraph event API)
    void setMouseDownScript(const QString& script) { m_mouseDownScript = script; }
    void setMouseUpScript(const QString& script) { m_mouseUpScript = script; }
    void setMouseMoveScript(const QString& script) { m_mouseMoveScript = script; }
    void setMouseWheelScript(const QString& script) { m_mouseWheelScript = script; }
    void setKeyPressScript(const QString& script) { m_keyPressScript = script; }
    void setKeyReleaseScript(const QString& script) { m_keyReleaseScript = script; }
    void setMouseDoubleClickScript(const QString& script) { m_mouseDoubleClickScript = script; }
    void setFocusInScript(const QString& script) { m_focusInScript = script; }
    void setFocusOutScript(const QString& script) { m_focusOutScript = script; }

    // Graphics buffer access (for cgraph integration)
    void* getGraphicsBuffer() { return m_gbuf; }
    void setGraphicsBuffer(void* gbuf) { m_gbuf = gbuf; }
    
    // Frame access (read-only for coordinate transformation and text properties)
    FRAME* getFrame() { return m_frame; }
    void setFrame(FRAME* frame) { m_frame = frame; }

    // Getter for bridge and manager integration
    QWidget* graphWidget() const { return m_graphWidget; }
    
    // Floating or docked state   
    void setFloatingMode(bool floating);
    bool isFloating() const { return m_isFloating; }

    // Current instance tracking (for cgraph callbacks)
    static EssGraphicsWidget* getCurrentInstance();

signals:
    void graphUpdated();
    void mousePressed(const QPointF& pos);
    void mouseReleased(const QPointF& pos);
    void mouseMoved(const QPointF& pos);
    void mouseDoubleClicked(const QPointF& pos);
    void returnToTabsRequested();
    void layoutModeChanged(LayoutMode mode);  // For future use
    
protected:
    // EssScriptableWidget interface implementation
    void registerCustomCommands() override;
    QWidget* createMainWidget() override;
    void onSetupComplete() override;
    
    // Layout creation methods (stubs for future enhancement)
    virtual QWidget* createContentArea();
    virtual QWidget* createSideControls() { return nullptr; }
    virtual QWidget* createBottomControls() { return nullptr; }
    
    // Qt event handling
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    
private slots:
    void initializeGraphics();
    
private:
    // Layout state (for future Qt controls integration)
    LayoutMode m_layoutMode;
    ControlPanelType m_controlType;
    bool m_controlsVisible;
    
    // Layout containers (for future use)
    QSplitter* m_mainSplitter;
    QWidget* m_sideControlsContainer;
    QWidget* m_bottomControlsContainer;
    
    // Graphics state
    void* m_gbuf;
    FRAME* m_frame;
    bool m_graphicsInitialized;
    QColor m_backgroundColor;
    
    // UI components
    QToolBar* m_toolbar;
    QWidget* m_graphWidget;
    QAction* m_returnToTabsAction;
    
    bool m_isFloating = false;
    
    // Event binding scripts (preserve QtCGraph event system)
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
    void cleanupGraphicsBuffer();
    QString substituteEventData(const QString& script, QEvent* event,
                               const QPointF& pos = QPointF(),
                               int button = -1, int delta = 0) const;
    QString keyToString(QKeyEvent* event) const;
    
    void onMousePressEvent(QMouseEvent* event);
    void onMouseReleaseEvent(QMouseEvent* event);
    void onMouseDoubleClickEvent(QMouseEvent* event);
    void onMouseMoveEvent(QMouseEvent* event);
    void onWheelEvent(QWheelEvent* event);
    void onKeyPressEvent(QKeyEvent* event);
    void onKeyReleaseEvent(QKeyEvent* event);
    void onFocusInEvent(QFocusEvent* event);
    void onFocusOutEvent(QFocusEvent* event);
    
    // Static callbacks for cgraph (preserve existing callback system)
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
    
    // Tcl command implementations (graphics-specific)
    static int tcl_graphics_init(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_graphics_clear(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_graphics_export(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_graphics_bind(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_graphics_setcolor(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_graphics_colorlist(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    
    // Layout control commands (stubs for future enhancement)
    static int tcl_graphics_layout(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_graphics_controls(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    
    friend class EssGraphicsBridge;
};

// Bridge class for cgraph integration (renamed from QtCGraphBridge)
class EssGraphicsBridge {
public:
    static void setupCallbacks();
    static void setGraphicsBuffer(EssGraphicsWidget* widget, void* gbuf);
    static void setFrame(EssGraphicsWidget* widget, void* frame);
};

#endif // ESSGRAPHICSWIDGET_H
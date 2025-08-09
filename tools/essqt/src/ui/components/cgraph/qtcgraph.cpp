#include "qtcgraph.hpp"
#include "qtcgmanager.hpp"
#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QDebug>
#include <QTimer>

// Static member for tracking current instance during painting
static thread_local QtCGraph* s_currentInstance = nullptr;

QtCGraph::QtCGraph(const QString& name, QWidget* parent)
    : QWidget(parent)
    , m_name(name)
    , m_interp(nullptr)
    , m_gbuf(nullptr)
    , m_frame(nullptr)
    , m_initialized(false)
    , m_backgroundColor(Qt::white)
    , m_currentPainter(nullptr)
    , m_currentColor(Qt::black)
{
    // Generate unique name if not provided
    if (m_name.isEmpty()) {
        static int counter = 0;
        m_name = QString("cgraph_%1").arg(++counter);
    }
    
    // Set up widget properties
    setMinimumSize(400, 300);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::ClickFocus);
    
    // Set background
    QPalette pal = palette();
    pal.setColor(QPalette::Window, m_backgroundColor);
    setPalette(pal);
    
    // Initialize interpreter
    initializeInterpreter();
    
    // Register with manager
    QtCGManager::getInstance().registerGraph(m_name, this);
}

void QtCGraph::cleanupGraphicsBuffer()
{
    if (!m_interp) return;
    
    // Use Tcl to do the cleanup since cgraph functions are loaded there
    QString cmd = QString("qtcgraph_cleanup %1").arg((quintptr)this);
    Tcl_Eval(m_interp, cmd.toUtf8().constData());
    
    // Clear our pointers
    m_gbuf = nullptr;
    m_frame = nullptr;
}

QtCGraph::~QtCGraph()
{
    // Unregister from manager
    QtCGManager::getInstance().unregisterGraph(m_name);
    
    // Clean up graphics buffer BEFORE deleting interpreter
    cleanupGraphicsBuffer();
    
    // Clean up interpreter
    if (m_interp) {
        Tcl_DeleteInterp(m_interp);
    }
}

void QtCGraph::initializeInterpreter()
{
    // Create interpreter
    m_interp = Tcl_CreateInterp();
    if (!m_interp) {
        emit error("Failed to create Tcl interpreter");
        return;
    }
    
    // Initialize Tcl
    if (Tcl_Init(m_interp) != TCL_OK) {
        emit error(QString("Failed to initialize Tcl: %1")
                  .arg(Tcl_GetStringResult(m_interp)));
        Tcl_DeleteInterp(m_interp);
        m_interp = nullptr;
        return;
    }
    
    // Store widget pointer in interpreter
    Tcl_SetAssocData(m_interp, "qtcgraph_widget", nullptr, this);
    
    // Load required packages
    const char* initScript = R"tcl(
        # Load required packages
        set f [file dirname [info nameofexecutable]]
        if { [file exists [file join $f dlsh.zip]] } { 
            set dlshzip [file join $f dlsh.zip] 
        } else {
            set dlshzip /usr/local/dlsh/dlsh.zip
        }
        set dlshroot [file join [zipfs root] dlsh]
        zipfs unmount $dlshroot
        zipfs mount $dlshzip $dlshroot
        set ::auto_path [linsert $::auto_path 0 [file join $dlshroot/lib]]
        package require dlsh
        package require qtcgraph
    )tcl";
    
    if (Tcl_Eval(m_interp, initScript) != TCL_OK) {
        emit error(QString("Failed to load packages: %1")
                  .arg(Tcl_GetStringResult(m_interp)));
    }
    
    // Schedule graphics initialization after widget is shown
    QMetaObject::invokeMethod(this, "initializeGraphics", Qt::QueuedConnection);
}

void QtCGraph::initializeGraphics()
{
    if (m_initialized || !m_interp) return;
    
    // Make sure widget has a size
    if (width() <= 0 || height() <= 0) {
        // Widget not ready yet, will retry on next paint
        return;
    }
    
    // Clean any old buffers
    cleanupGraphicsBuffer();
     
    // Call Tcl to initialize the graphics buffer
    QString cmd = QString("qtcgraph_init_widget %1 %2 %3")
                    .arg((quintptr)this)
                    .arg(width())
                    .arg(height());
    
    if (Tcl_Eval(m_interp, cmd.toUtf8().constData()) != TCL_OK) {
        emit error(QString("Failed to initialize graphics: %1")
                  .arg(Tcl_GetStringResult(m_interp)));
        return;
    }
    
    m_initialized = true;
    
    // Schedule a clearwin after the widget is fully shown
    QTimer::singleShot(100, this, [this]() {
        if (m_interp && m_initialized) {
            Tcl_Eval(m_interp, "clearwin; flushwin");
        }
    });
    
    // Execute init script if set
    if (!m_initScript.isEmpty()) {
        executeInitScript();
    }
    
    emit initialized();
}

int QtCGraph::eval(const QString& command)
{
    if (!m_interp) {
        emit error("No interpreter available");
        return TCL_ERROR;
    }
    
    int result = Tcl_Eval(m_interp, command.toUtf8().constData());
    emit commandExecuted(result, QString(Tcl_GetStringResult(m_interp)));
    
    return result;
}

QString QtCGraph::result() const
{
    if (!m_interp) return QString();
    return QString(Tcl_GetStringResult(m_interp));
}

void QtCGraph::refresh()
{
    update();
}

void QtCGraph::clear()
{
    if (!m_interp || !m_gbuf) return;
    
    QString cmd = QString("qtcgraph_clear %1").arg((quintptr)this);
    Tcl_Eval(m_interp, cmd.toUtf8().constData());
    refresh();
}

void QtCGraph::setInitScript(const QString& script)
{
    m_initScript = script;
    if (m_interp && m_initialized) {
        executeInitScript();
    }
}

void QtCGraph::executeInitScript()
{
    if (m_initScript.isEmpty() || !m_interp) return;
    
    int result = eval(m_initScript);
    if (result != TCL_OK) {
        emit error(QString("Init script failed: %1").arg(this->result()));
    }
}

void QtCGraph::setBackgroundColor(const QColor& color)
{
    m_backgroundColor = color;
    
    // Update the palette
    QPalette pal = palette();
    pal.setColor(QPalette::Window, m_backgroundColor);
    setPalette(pal);
    
    // Trigger a repaint
    update();
}

void QtCGraph::paintEvent(QPaintEvent* event)
{
    if (!m_initialized) {
        initializeGraphics();
        if (!m_initialized) return;
    }
    
    QPainter painter(this);
    m_currentPainter = &painter;
    s_currentInstance = this;
    
    // Clear background
    painter.fillRect(rect(), m_backgroundColor);
    
    // Initialize pen and brush
    painter.setPen(m_currentColor);
    painter.setBrush(m_currentColor);
    
    // Call Tcl to playback the graphics
    if (m_interp && m_gbuf) {
        QString cmd = QString("qtcgraph_playback %1").arg((quintptr)m_gbuf);
        Tcl_Eval(m_interp, cmd.toUtf8().constData());
    }
    
    m_currentPainter = nullptr;
    s_currentInstance = nullptr;
    emit graphUpdated();
}

void QtCGraph::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    
    if (m_initialized && m_interp) {
        QString cmd = QString("qtcgraph_resize %1 %2 %3")
                        .arg((quintptr)this)
                        .arg(width())
                        .arg(height());
        Tcl_Eval(m_interp, cmd.toUtf8().constData());
        update();
    }
}

QString QtCGraph::substituteEventData(const QString& script, QEvent* event,
                                     const QPointF& pos, int button, int delta) const
{
    QString result = script;
    
    // Basic position substitutions
    if (pos != QPointF()) {
        // Get pixel coordinates from Qt
        int pixelX = (int)pos.x();
        int pixelY = (int)pos.y();
        
        // Transform coordinates if we have a frame
        if (m_frame) {
            // First flip Y (Qt uses top-left, cgraph uses bottom-left)
            float screenX = pixelX;
            float screenY = (m_frame->ysres - 1) - pixelY;
            
            // Then apply the SCREEN transformation to get window coordinates
            float winX = m_frame->xul + ((screenX - m_frame->xl) * m_frame->xus) / m_frame->xs;
            float winY = m_frame->yub + ((screenY - m_frame->yb) * m_frame->yus) / m_frame->ys;
            
            // %x, %y - window coordinates
            result.replace("%x", QString::number(winX, 'f', 2));
            result.replace("%y", QString::number(winY, 'f', 2));
        } else {
            // No frame, just use pixel coordinates
            result.replace("%x", QString::number(pixelX));
            result.replace("%y", QString::number(pixelY));
        }
        
        // %X, %Y - raw pixel coordinates
        result.replace("%X", QString::number(pixelX));
        result.replace("%Y", QString::number(pixelY));
    }
    
    // Mouse button
    if (button >= 0) {
        result.replace("%b", QString::number(button));
    }
    
    // Mouse wheel delta
    if (delta != 0) {
        result.replace("%D", QString::number(delta));
    }
    
    // Widget info
    result.replace("%w", QString::number(width()));
    result.replace("%h", QString::number(height()));
    result.replace("%W", m_name);
    
    // Event type
    QString typeStr;
    switch (event->type()) {
        case QEvent::MouseButtonPress: typeStr = "ButtonPress"; break;
        case QEvent::MouseButtonRelease: typeStr = "ButtonRelease"; break;
        case QEvent::MouseMove: typeStr = "Motion"; break;
        case QEvent::MouseButtonDblClick: typeStr = "Double"; break;
        case QEvent::Wheel: typeStr = "MouseWheel"; break;
        case QEvent::KeyPress: typeStr = "KeyPress"; break;
        case QEvent::KeyRelease: typeStr = "KeyRelease"; break;
        case QEvent::FocusIn: typeStr = "FocusIn"; break;
        case QEvent::FocusOut: typeStr = "FocusOut"; break; 
        default: typeStr = "Unknown";
    }
    result.replace("%T", typeStr);
    
    // Handle keyboard events
    QKeyEvent* keyEvent = dynamic_cast<QKeyEvent*>(event);
    if (keyEvent) {
        QString keyStr = keyToString(keyEvent);
        result.replace("%K", keyStr);
        result.replace("%k", QString::number(keyEvent->key()));
        
        // ASCII value if applicable
        QString text = keyEvent->text();
        if (!text.isEmpty()) {
            result.replace("%A", text);
            result.replace("%a", QString::number(text[0].unicode()));
        } else {
            result.replace("%A", "");
            result.replace("%a", "0");
        }
    }
    
    // Percent literal
    result.replace("%%", "%");
    
    return result;
}

QString QtCGraph::keyToString(QKeyEvent* event) const
{
    QString key;
    
    // Handle special keys
    switch(event->key()) {
        case Qt::Key_Return: key = "Return"; break;
        case Qt::Key_Enter: key = "KP_Enter"; break;
        case Qt::Key_Escape: key = "Escape"; break;
        case Qt::Key_Tab: key = "Tab"; break;
        case Qt::Key_Backspace: key = "BackSpace"; break;
        case Qt::Key_Delete: key = "Delete"; break;
        case Qt::Key_Left: key = "Left"; break;
        case Qt::Key_Right: key = "Right"; break;
        case Qt::Key_Up: key = "Up"; break;
        case Qt::Key_Down: key = "Down"; break;
        case Qt::Key_Space: key = "space"; break;
        default:
            if (!event->text().isEmpty()) {
                key = event->text();
            } else {
                key = QKeySequence(event->key()).toString();
            }
    }
    
    // Add modifiers
    QString modifiers;
    if (event->modifiers() & Qt::ControlModifier) modifiers += "Control-";
    if (event->modifiers() & Qt::AltModifier) modifiers += "Alt-";
    if (event->modifiers() & Qt::ShiftModifier) modifiers += "Shift-";
    if (event->modifiers() & Qt::MetaModifier) modifiers += "Meta-";
    
    return modifiers + key;
}

// Mouse and keyboard event handlers remain the same...
void QtCGraph::mousePressEvent(QMouseEvent *event)
{
    if (!m_mouseDownScript.isEmpty() && m_interp) {
        int button = event->button() == Qt::LeftButton ? 1 : 
                     event->button() == Qt::RightButton ? 3 : 
                     event->button() == Qt::MiddleButton ? 2 : 0;
        
        QString cmd = substituteEventData(m_mouseDownScript, event, 
                                         event->position(), button);
        Tcl_Eval(m_interp, cmd.toUtf8().constData());
    }
    emit mousePressed(event->position());
}

void QtCGraph::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_mouseUpScript.isEmpty() && m_interp) {
        int button = event->button() == Qt::LeftButton ? 1 : 
                     event->button() == Qt::RightButton ? 3 : 
                     event->button() == Qt::MiddleButton ? 2 : 0;
        
        QString cmd = substituteEventData(m_mouseUpScript, event, 
                                         event->position(), button);
        Tcl_Eval(m_interp, cmd.toUtf8().constData());
    }
    emit mouseReleased(event->position());
}

void QtCGraph::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_mouseMoveScript.isEmpty() && m_interp) {
        QString cmd = substituteEventData(m_mouseMoveScript, event, 
                                         event->position());
        Tcl_Eval(m_interp, cmd.toUtf8().constData());
    }
    emit mouseMoved(event->position());
}

void QtCGraph::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_mouseDoubleClickScript.isEmpty() && m_interp) {
        int button = event->button() == Qt::LeftButton ? 1 : 
                     event->button() == Qt::RightButton ? 3 : 
                     event->button() == Qt::MiddleButton ? 2 : 0;
        
        QString cmd = substituteEventData(m_mouseDoubleClickScript, event, 
                                         event->position(), button);
        Tcl_Eval(m_interp, cmd.toUtf8().constData());
    }
    emit mouseDoubleClicked(event->position());
}

void QtCGraph::wheelEvent(QWheelEvent *event)
{
    if (!m_mouseWheelScript.isEmpty() && m_interp) {
        int delta = event->angleDelta().y();
        QString cmd = substituteEventData(m_mouseWheelScript, event, 
                                         event->position(), -1, delta);
        Tcl_Eval(m_interp, cmd.toUtf8().constData());
    }
}

void QtCGraph::keyPressEvent(QKeyEvent *event)
{
    if (!m_keyPressScript.isEmpty() && m_interp) {
        QString cmd = substituteEventData(m_keyPressScript, event);
        Tcl_Eval(m_interp, cmd.toUtf8().constData());
    }
}

void QtCGraph::keyReleaseEvent(QKeyEvent *event)
{
    if (!m_keyReleaseScript.isEmpty() && m_interp) {
        QString cmd = substituteEventData(m_keyReleaseScript, event);
        Tcl_Eval(m_interp, cmd.toUtf8().constData());
    }
}

void QtCGraph::focusInEvent(QFocusEvent *event)
{
    if (!m_focusInScript.isEmpty() && m_interp) {
        QString cmd = substituteEventData(m_focusInScript, event);
        Tcl_Eval(m_interp, cmd.toUtf8().constData());
    }
    QWidget::focusInEvent(event);
}

void QtCGraph::focusOutEvent(QFocusEvent *event)
{
    if (!m_focusOutScript.isEmpty() && m_interp) {
        QString cmd = substituteEventData(m_focusOutScript, event);
        Tcl_Eval(m_interp, cmd.toUtf8().constData());
    }
    QWidget::focusOutEvent(event);
}

bool QtCGraph::exportToPDF(const QString& filename)
{
    if (!m_interp || !m_gbuf) {
        emit error("Cannot export: not initialized");
        return false;
    }
    
    // Make sure we're the current graph
    s_currentInstance = this;
    
    // Use cgraph's dumpwin command
    QString cmd = QString("dumpwin pdf {%1}").arg(filename);
    int result = eval(cmd);
    
    if (result != TCL_OK) {
        emit error(QString("PDF export failed: %1").arg(this->result()));
        return false;
    }
    
    return true;
}

bool QtCGraph::exportToPDFDialog(const QString& suggestedName)
{
    QString suggestion = suggestedName;
    if (suggestion.isEmpty()) {
        suggestion = m_name + ".pdf";
    }
    
    QString filename = QFileDialog::getSaveFileName(
        this,
        tr("Export Graph to PDF"),
        suggestion,
        tr("PDF Files (*.pdf);;All Files (*)")
    );
    
    if (filename.isEmpty()) {
        return false;
    }
    
    if (!filename.endsWith(".pdf", Qt::CaseInsensitive)) {
        filename += ".pdf";
    }
    
    return exportToPDF(filename);
}

QtCGraph* QtCGraph::getCurrentInstance()
{
    return s_currentInstance;
}

// Static cgraph callbacks
int QtCGraph::Clearwin()
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter) return 0;
    
    s_currentInstance->m_currentPainter->fillRect(
        s_currentInstance->rect(), 
        s_currentInstance->m_backgroundColor);
    return 0;
}

int QtCGraph::Line(float x0, float y0, float x1, float y1)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter) return 0;
    
    float h = s_currentInstance->height();
    s_currentInstance->m_currentPainter->setPen(s_currentInstance->m_currentColor);
    s_currentInstance->m_currentPainter->drawLine(QPointF(x0, h - y0), QPointF(x1, h - y1));
    return 0;
}

int QtCGraph::Point(float x, float y)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter) return 0;
    
    float h = s_currentInstance->height();
    s_currentInstance->m_currentPainter->drawPoint(QPointF(x, h - y));
    return 0;
}

int QtCGraph::Setcolor(int index)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter) return 0;
    
    // Use the color table from QtCGWin
    static const QColor colorTable[] = {
        Qt::black, Qt::blue, Qt::darkGreen, Qt::cyan,
        Qt::red, Qt::magenta, QColor(165, 42, 42), Qt::white,
        Qt::gray, QColor(173, 216, 230), Qt::green, QColor(224, 255, 255),
        QColor(255, 20, 147), QColor(147, 112, 219), Qt::yellow,
        QColor(0, 0, 128), Qt::white, Qt::lightGray
    };
    
    int oldcolor = 0; // Would track this properly
    
    if (index < 18) {
        s_currentInstance->m_currentColor = colorTable[index];
    } else {
        // Handle RGB colors encoded in upper bits
        unsigned int shifted = index >> 5;
        int r = (shifted & 0xff0000) >> 16;
        int g = (shifted & 0xff00) >> 8;
        int b = (shifted & 0xff);
        s_currentInstance->m_currentColor = QColor(r, g, b);
    }
    
    QPen pen(s_currentInstance->m_currentColor);
    s_currentInstance->m_currentPainter->setPen(pen);
    s_currentInstance->m_currentPainter->setBrush(s_currentInstance->m_currentColor);
    
    return oldcolor;
}

int QtCGraph::Char(float x, float y, char *string)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter || !string) return 0;
    
    float h = s_currentInstance->height();
    y = h - y;
    
    // Get orientation from frame
    int ori = s_currentInstance->m_frame ? s_currentInstance->m_frame->orientation : 0;
    
    // Handle rotation based on orientation
    s_currentInstance->m_currentPainter->save();
    s_currentInstance->m_currentPainter->translate(x, y);
    s_currentInstance->m_currentPainter->rotate(-ori * 90);
    s_currentInstance->m_currentPainter->drawText(0, 0, QString::fromUtf8(string));
    s_currentInstance->m_currentPainter->restore();
    
    return 0;
}

int QtCGraph::Text(float x, float y, char *string)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter || !string) return 0;
    
    float h = s_currentInstance->height();
    y = h - y;
    
    QString text = QString::fromUtf8(string);
    QFontMetrics fm(s_currentInstance->m_currentPainter->font());
    QRectF textRect = fm.boundingRect(text);
    
    float hoff = 0, voff = 0;
    int ori = s_currentInstance->m_frame ? s_currentInstance->m_frame->orientation : 0;
    int just = s_currentInstance->m_frame ? s_currentInstance->m_frame->just : -1;
    
    if (ori == 0 || ori == 2) {  // horizontal
        switch (just) {
        case -1:  // JustLeft
            hoff = 0; 
            break;
        case 1:   // JustRight
            hoff = textRect.width(); 
            break;
        case 0:   // JustCenter
            hoff = textRect.width() * 0.5; 
            break;
        }
        voff = textRect.height() * 0.5;
    } else {  // vertical
        switch (just) {
        case -1:  // JustLeft
            voff = 0; 
            break;
        case 1:   // JustRight
            voff = textRect.width(); 
            break;
        case 0:   // JustCenter
            voff = textRect.width() * 0.5; 
            break;
        }
        hoff = -textRect.height() * 0.5;
    }
    
    s_currentInstance->m_currentPainter->save();
    s_currentInstance->m_currentPainter->translate(x - hoff, y + voff);
    s_currentInstance->m_currentPainter->rotate(-ori * 90);
    s_currentInstance->m_currentPainter->drawText(0, 0, text);
    s_currentInstance->m_currentPainter->restore();
    
    return 0;
}

int QtCGraph::Setfont(char *fontname, float size)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter) return 0;
    
    QString fname(fontname);
    QFont font;
    
    if (fname == "HELVETICA") {
        font.setFamily("Helvetica");
    } else if (fname == "TIMES") {
        font.setFamily("Times");
    } else if (fname == "COURIER") {
        font.setFamily("Courier");
    } else if (fname == "SYMBOL") {
        font.setFamily("Arial Unicode MS");
    } else {
        font.setFamily(fname);
    }
    
    font.setPointSizeF(size);
    s_currentInstance->m_currentPainter->setFont(font);
    
    return 0;
}

int QtCGraph::Strwidth(char *str)
{
    if (!s_currentInstance || !str) return 0;
    
    QFontMetrics fm(s_currentInstance->m_currentPainter ? 
                    s_currentInstance->m_currentPainter->font() : 
                    s_currentInstance->font());
    return fm.horizontalAdvance(QString::fromUtf8(str));
}

int QtCGraph::Strheight(char *str)
{
    if (!s_currentInstance || !str) return 0;
    
    QFontMetrics fm(s_currentInstance->m_currentPainter ? 
                    s_currentInstance->m_currentPainter->font() : 
                    s_currentInstance->font());
    return fm.height();
}

int QtCGraph::FilledPolygon(float *verts, int nverts)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter || !verts) return 0;
    
    float h = s_currentInstance->height();
    
    QPolygonF polygon;
    for (int i = 0; i < nverts; i++) {
        polygon << QPointF(verts[i*2], h - verts[i*2+1]);
    }
    
    s_currentInstance->m_currentPainter->drawPolygon(polygon);
    
    return 0;
}

int QtCGraph::Circle(float x, float y, float width, int filled)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter) return 0;
    
    float h = s_currentInstance->height();
    y = h - y;
    
    if (filled) {
        s_currentInstance->m_currentPainter->drawEllipse(
            QPointF(x + width/2, y + width/2), width/2, width/2);
    } else {
        s_currentInstance->m_currentPainter->save();
        s_currentInstance->m_currentPainter->setBrush(Qt::NoBrush);
        s_currentInstance->m_currentPainter->drawEllipse(
            QPointF(x + width/2, y + width/2), width/2, width/2);
        s_currentInstance->m_currentPainter->restore();
    }
    
    return 0;
}
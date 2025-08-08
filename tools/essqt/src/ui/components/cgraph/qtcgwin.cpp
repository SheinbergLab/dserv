#include "qtcgwin.hpp"
#include "qtcgmanager.hpp"
#include <QMouseEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFileDialog>
#include <QMessageBox>
#include <QDebug>
#include <cmath>

// Thread-local storage for current widget (Qt is single-threaded for GUI)
static thread_local QtCGWin* currentCG = nullptr;

// Color table from FLTK version
static const int NColorVals = 18;
static float colorvals[] = {
  /* R    G    B   Grey - Index: Name */
  0.0,  0.0,  0.0,  0.00,  // 0: black
  0.0,  0.0,  1.0,  0.11,  // 1: blue (pure blue)
  0.0,  0.4,  0.0,  0.23,  // 2: dark_green
  0.0,  1.0,  1.0,  0.70,  // 3: cyan
  1.0,  0.0,  0.0,  0.30,  // 4: red (pure red)
  1.0,  0.0,  1.0,  0.41,  // 5: magenta
  0.65, 0.16, 0.16, 0.29,  // 6: brown
  1.0,  1.0,  1.0,  1.00,  // 7: white
  0.5,  0.5,  0.5,  0.50,  // 8: gray
  0.68, 0.85, 1.0,  0.82,  // 9: light_blue
  0.0,  1.0,  0.0,  0.59,  // 10: green (pure green)
  0.88, 1.0,  1.0,  0.95,  // 11: light_cyan
  1.0,  0.08, 0.58, 0.39,  // 12: deep_pink
  0.58, 0.44, 0.86, 0.49,  // 13: medium_purple
  1.0,  1.0,  0.0,  0.89,  // 14: yellow
  0.0,  0.0,  0.5,  0.06,  // 15: navy (dark blue)
  1.0,  1.0,  1.0,  1.00,  // 16: bright_white (same as white)
  0.83, 0.83, 0.83, 0.83,  // 17: light_gray
};

// QtCGWin implementation
QtCGWin::QtCGWin(Tcl_Interp *interp, QWidget *parent)
    : QWidget(parent)
    , interp(interp)
    , frame(nullptr)
    , gbuf(nullptr)
    , initialized(false)
    , currentPainter(nullptr)
    , currentColor(Qt::black)
    , lineStyle(0)
    , backgroundColor(Qt::white) 
{
    // Set widget properties
    setMinimumSize(200, 200);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(true);
    
    // Set background to white
    QPalette pal = palette();
    pal.setColor(QPalette::Window, backgroundColor);
    setPalette(pal);
    
    // Graphics buffer initialization will be done by the Tcl module
    setFocusPolicy(Qt::ClickFocus);
}

QtCGWin::~QtCGWin()
{
    // Cleanup will be done by the Tcl module
}

void QtCGWin::init()
{
    if (initialized) return;
    
    // Call Tcl to initialize the widget
    if (interp) {
        QString cmd = QString("qtcgwin_init_widget %1 %2 %3")
                        .arg((quintptr)this)
                        .arg(width())
                        .arg(height());
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
    
    initialized = true;
}

void QtCGWin::refresh()
{
    update();  // Schedule a repaint
}

QString QtCGWin::keyToString(QKeyEvent* event) const
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
        case Qt::Key_Home: key = "Home"; break;
        case Qt::Key_End: key = "End"; break;
        case Qt::Key_PageUp: key = "Prior"; break;
        case Qt::Key_PageDown: key = "Next"; break;
        case Qt::Key_F1: key = "F1"; break;
        case Qt::Key_F2: key = "F2"; break;
        case Qt::Key_F3: key = "F3"; break;
        case Qt::Key_F4: key = "F4"; break;
        case Qt::Key_F5: key = "F5"; break;
        case Qt::Key_F6: key = "F6"; break;
        case Qt::Key_F7: key = "F7"; break;
        case Qt::Key_F8: key = "F8"; break;
        case Qt::Key_F9: key = "F9"; break;
        case Qt::Key_F10: key = "F10"; break;
        case Qt::Key_F11: key = "F11"; break;
        case Qt::Key_F12: key = "F12"; break;
        case Qt::Key_Space: key = "space"; break;
        default:
            // For regular characters
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

void QtCGWin::setBackgroundColor(const QColor& color)
{
    backgroundColor = color;
    
    // Update the palette
    QPalette pal = palette();
    pal.setColor(QPalette::Window, backgroundColor);
    setPalette(pal);
    
    // Trigger a repaint
    update();
}

// In qtcgwin.cpp, add this extern declaration at the top with other includes:
extern "C" {
    void screen_to_window(int x, int y, float *px, float *py);
}

// In qtcgwin.cpp, update the substituteEventData method:

QString QtCGWin::substituteEventData(const QString& script, QEvent* event, 
                                    const QPointF& pos, 
                                    int button, int delta) const
{
    QString result = script;
    
    // Basic position substitutions
    if (pos != QPointF()) {
        // Get pixel coordinates from Qt
        int pixelX = (int)pos.x();
        int pixelY = (int)pos.y();
        
        // Do the coordinate transformation inline
        // Based on screen_to_window from cgraph.c:
        //   float argx = x;
        //   float argy = ((f->ysres-1) - y);
        //   SCREEN(f,argx,argy);
        
        if (frame) {
            // First flip Y (Qt uses top-left, cgraph uses bottom-left)
            float screenX = pixelX;
            float screenY = (frame->ysres - 1) - pixelY;
            
            // Then apply the SCREEN transformation:
            // SCREEN(f,x,y) {x=f->xul + MULDIV(x-f->xl,f->xus,f->xs); 
            //                 y=f->yub + MULDIV(y-f->yb,f->yus,f->ys);}
            
            float winX = frame->xul + ((screenX - frame->xl) * frame->xus) / frame->xs;
            float winY = frame->yub + ((screenY - frame->yb) * frame->yus) / frame->ys;
            
            // %x, %y - window coordinates (transformed through setwindow)
            result.replace("%x", QString::number(winX, 'f', 2));
            result.replace("%y", QString::number(winY, 'f', 2));
        } else {
            // No frame, just use pixel coordinates
            result.replace("%x", QString::number(pixelX));
            result.replace("%y", QString::number(pixelY));
        }
        
        // %X, %Y - raw pixel coordinates (Qt coordinates)
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
    
    // Widget dimensions
    result.replace("%w", QString::number(width()));
    result.replace("%h", QString::number(height()));
    
    // Event type string
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
    
    // Widget name (window name from manager)
    QString winName = QtCGManager::getInstance().findCGWinName(
        const_cast<QtCGWin*>(this));
    result.replace("%W", winName);
    
    // For keyboard events
    QKeyEvent* keyEvent = dynamic_cast<QKeyEvent*>(event);
    if (keyEvent) {
        // Key string
        QString keyStr = keyToString(keyEvent);
        result.replace("%K", keyStr);
        
        // ASCII character (if applicable)
        QString text = keyEvent->text();
        if (!text.isEmpty()) {
            result.replace("%A", text);
            result.replace("%a", QString::number(text[0].unicode()));
        } else {
            result.replace("%A", "");
            result.replace("%a", "0");
        }
        
        // Key code
        result.replace("%k", QString::number(keyEvent->key()));
        
        // State (modifiers as a number)
        int state = 0;
        if (keyEvent->modifiers() & Qt::ShiftModifier) state |= 1;
        if (keyEvent->modifiers() & Qt::ControlModifier) state |= 4;
        if (keyEvent->modifiers() & Qt::AltModifier) state |= 8;
        if (keyEvent->modifiers() & Qt::MetaModifier) state |= 16;
        result.replace("%s", QString::number(state));
    }
    
    // For mouse events, add state
    QMouseEvent* mouseEvent = dynamic_cast<QMouseEvent*>(event);
    if (mouseEvent) {
        int state = 0;
        if (mouseEvent->modifiers() & Qt::ShiftModifier) state |= 1;
        if (mouseEvent->modifiers() & Qt::ControlModifier) state |= 4;
        if (mouseEvent->modifiers() & Qt::AltModifier) state |= 8;
        if (mouseEvent->modifiers() & Qt::MetaModifier) state |= 16;
        // Add button states
        if (mouseEvent->buttons() & Qt::LeftButton) state |= 256;
        if (mouseEvent->buttons() & Qt::MiddleButton) state |= 512;
        if (mouseEvent->buttons() & Qt::RightButton) state |= 1024;
        result.replace("%s", QString::number(state));
    }
    
    // Time stamp (milliseconds since start)
    static qint64 startTime = QDateTime::currentMSecsSinceEpoch();
    qint64 eventTime = QDateTime::currentMSecsSinceEpoch() - startTime;
    result.replace("%t", QString::number(eventTime));
    
    // Percent literal
    result.replace("%%", "%");
    
    return result;
}

void QtCGWin::mousePressEvent(QMouseEvent *event)
{
    if (!mouseDownScript.isEmpty() && interp) {
        int button = event->button() == Qt::LeftButton ? 1 : 
                     event->button() == Qt::RightButton ? 3 : 
                     event->button() == Qt::MiddleButton ? 2 : 0;
        
        QString cmd = substituteEventData(mouseDownScript, event, 
                                         event->position(), button);
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
    emit mousePressed(event->position());
}

void QtCGWin::mouseReleaseEvent(QMouseEvent *event)
{
    if (!mouseUpScript.isEmpty() && interp) {
        int button = event->button() == Qt::LeftButton ? 1 : 
                     event->button() == Qt::RightButton ? 3 : 
                     event->button() == Qt::MiddleButton ? 2 : 0;
        
        QString cmd = substituteEventData(mouseUpScript, event, 
                                         event->position(), button);
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
    emit mouseReleased(event->position());
}

void QtCGWin::mouseMoveEvent(QMouseEvent *event)
{
    if (!mouseMoveScript.isEmpty() && interp) {
        QString cmd = substituteEventData(mouseMoveScript, event, 
                                         event->position());
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
    emit mouseMoved(event->position());
}

void QtCGWin::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!mouseDoubleClickScript.isEmpty() && interp) {
        int button = event->button() == Qt::LeftButton ? 1 : 
                     event->button() == Qt::RightButton ? 3 : 
                     event->button() == Qt::MiddleButton ? 2 : 0;
        
        QString cmd = substituteEventData(mouseDoubleClickScript, event, 
                                         event->position(), button);
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
}

void QtCGWin::wheelEvent(QWheelEvent *event)
{
    if (!mouseWheelScript.isEmpty() && interp) {
        int delta = event->angleDelta().y();
        QString cmd = substituteEventData(mouseWheelScript, event, 
                                         event->position(), -1, delta);
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
}

void QtCGWin::keyPressEvent(QKeyEvent *event)
{
    if (!keyPressScript.isEmpty() && interp) {
        QString cmd = substituteEventData(keyPressScript, event);
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
}

void QtCGWin::keyReleaseEvent(QKeyEvent *event)
{
    if (!keyReleaseScript.isEmpty() && interp) {
        QString cmd = substituteEventData(keyReleaseScript, event);
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
}

void QtCGWin::focusInEvent(QFocusEvent *event)
{
    if (!focusInScript.isEmpty() && interp) {
        QString cmd = substituteEventData(focusInScript, event);
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
    QWidget::focusInEvent(event);
}

void QtCGWin::focusOutEvent(QFocusEvent *event)
{
    if (!focusOutScript.isEmpty() && interp) {
        QString cmd = substituteEventData(focusOutScript, event);
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
    QWidget::focusOutEvent(event);
}

bool QtCGWin::exportToPDF(const QString& filename)
{
    if (!interp || !gbuf) {
        qWarning() << "Cannot export: no interpreter or graphics buffer";
        return false;
    }
    
    // Make sure this window is current
    QtCGManager::getInstance().setCurrentCGWin(this);
    
    // Set the graphics buffer as current
    if (gbuf) {
        QString cmd = QString("qtcgwin_set_current %1").arg((quintptr)gbuf);
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
    
    // Use the dumpwin command
    QString dumpCmd = QString("dumpwin pdf {%1}").arg(filename);
    int result = Tcl_Eval(interp, dumpCmd.toUtf8().constData());
    
    if (result != TCL_OK) {
        qWarning() << "PDF export failed:" << Tcl_GetStringResult(interp);
        return false;
    }
    
    return true;
}

bool QtCGWin::exportToPDFDialog(const QString& suggestedName)
{
    // Build suggested filename
    QString suggestion = suggestedName;
    if (suggestion.isEmpty()) {
        // Use manager to get our name
        QString winName = QtCGManager::getInstance().findCGWinName(this);
        if (!winName.isEmpty()) {
            suggestion = winName + ".pdf";
        } else {
            suggestion = "cgraph_export.pdf";
        }
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
    
    // Ensure .pdf extension
    if (!filename.endsWith(".pdf", Qt::CaseInsensitive)) {
        filename += ".pdf";
    }
    
    bool success = exportToPDF(filename);
    
    if (success) {
        // Optional: show success message
        QMessageBox::information(this, tr("Export Successful"), 
                               tr("Graph exported to %1").arg(filename));
    } else {
        QMessageBox::warning(this, tr("Export Failed"), 
                           tr("Failed to export graph to PDF"));
    }
    
    return success;
}

void QtCGWin::paintEvent(QPaintEvent *event)
{
    if (!initialized) init();
    
    QPainter painter(this);
    currentPainter = &painter;
    currentCG = this;
    QtCGManager::getInstance().setCurrentCGWin(this);
    
    // Clear background
    painter.fillRect(rect(), backgroundColor);
    
    // Initialize the pen and brush with the current color
    painter.setPen(currentColor);
    painter.setBrush(currentColor);
    
    // Call Tcl to playback the graphics events
    if (interp && gbuf) {
        QString cmd = QString("qtcgwin_playback %1").arg((quintptr)gbuf);
        Tcl_Eval(interp, cmd.toUtf8().constData());
    }
    
    currentPainter = nullptr;
    emit graphUpdated();
}

void QtCGWin::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (initialized && interp) {
        QString cmd = QString("qtcgwin_resize %1 %2 %3")
                        .arg((quintptr)this)
                        .arg(width())
                        .arg(height());
        Tcl_Eval(interp, cmd.toUtf8().constData());
        update();
    }
}

QtCGWin* QtCGWin::getCurrentInstance()
{
    return QtCGManager::getInstance().getCurrentCGWin();
}

// Static cgraph callbacks
int QtCGWin::Clearwin()
{
    if (!currentCG || !currentCG->currentPainter) return 0;
    
    currentCG->currentPainter->fillRect(currentCG->rect(), 
                                        currentCG->backgroundColor);
    return 0;
}

int QtCGWin::Line(float x0, float y0, float x1, float y1)
{
    if (!currentCG || !currentCG->currentPainter) return 0;
    
    // Flip Y coordinate (cgraph uses bottom-left origin)
    float h = currentCG->height();
    currentCG->currentPainter->setPen(currentCG->currentColor);
    currentCG->currentPainter->drawLine(QPointF(x0, h - y0), QPointF(x1, h - y1));
    return 0;
}

int QtCGWin::Point(float x, float y)
{
    if (!currentCG || !currentCG->currentPainter) return 0;
    
    float h = currentCG->height();
    currentCG->currentPainter->drawPoint(QPointF(x, h - y));
    return 0;
}

int QtCGWin::Char(float x, float y, char *string)
{
    if (!currentCG || !currentCG->currentPainter || !string) return 0;
    
    float h = currentCG->height();
    y = h - y;
    
    // Get orientation directly from frame
    int ori = currentCG->frame ? currentCG->frame->orientation : 0;
    
    // Handle rotation based on orientation
    currentCG->currentPainter->save();
    currentCG->currentPainter->translate(x, y);
    currentCG->currentPainter->rotate(-ori * 90);
    currentCG->currentPainter->drawText(0, 0, QString::fromUtf8(string));
    currentCG->currentPainter->restore();
    
    return 0;
}

int QtCGWin::Text(float x, float y, char *string)
{
    if (!currentCG || !currentCG->currentPainter || !string) return 0;
    
    float h = currentCG->height();
    y = h - y;
    
    QString text = QString::fromUtf8(string);
    QFontMetrics fm(currentCG->currentPainter->font());
    QRectF textRect = fm.boundingRect(text);
    
    float hoff = 0, voff = 0;
    // Get orientation and justification directly from frame
    int ori = currentCG->frame ? currentCG->frame->orientation : 0;
    int just = currentCG->frame ? currentCG->frame->just : -1;
    
    if (ori == 0 || ori == 2) {  // horizontal (0) or upside down (2)
        switch (just) {
        case -1:  // JustLeft (LEFT_JUST)
            hoff = 0; 
            break;
        case 1:   // JustRight (RIGHT_JUST)
            hoff = textRect.width(); 
            break;
        case 0:   // JustCenter (CENTER_JUST)
            hoff = textRect.width() * 0.5; 
            break;
        }
        voff = textRect.height() * 0.5;
    } else {  // vertical (1 or 3)
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
    
    currentCG->currentPainter->save();
    currentCG->currentPainter->translate(x - hoff, y + voff);
    currentCG->currentPainter->rotate(-ori * 90);
    currentCG->currentPainter->drawText(0, 0, text);
    currentCG->currentPainter->restore();
    
    return 0;
}

int QtCGWin::Setfont(char *fontname, float size)
{
    if (!currentCG || !currentCG->currentPainter) return 0;
    
    QString fname(fontname);
    QFont font;
    
    if (fname == "HELVETICA") {
        font.setFamily("Helvetica");
    } else if (fname == "TIMES") {
        font.setFamily("Times");
    } else if (fname == "COURIER") {
        font.setFamily("Courier");
    } else if (fname == "SYMBOL") {
        // Qt doesn't have a direct Symbol font, use a Unicode font
        font.setFamily("Arial Unicode MS");
    } else {
        font.setFamily(fname);
    }
    
    font.setPointSizeF(size);
    currentCG->currentPainter->setFont(font);
    
    return 0;
}

int QtCGWin::Strwidth(char *str)
{
    if (!currentCG || !str) return 0;
    
    QFontMetrics fm(currentCG->currentFont);
    return fm.horizontalAdvance(QString::fromUtf8(str));
}

int QtCGWin::Strheight(char *str)
{
    if (!currentCG || !str) return 0;
    
    QFontMetrics fm(currentCG->currentFont);
    return fm.height();
}

int QtCGWin::Setcolor(int index)
{
    if (!currentCG || !currentCG->currentPainter) return 0;
    
    static int oldcolor = 0;
    int returnColor = oldcolor;
    
    if (index < NColorVals) {
        int r = colorvals[index*4+0] * 255;
        int g = colorvals[index*4+1] * 255;
        int b = colorvals[index*4+2] * 255;
        currentCG->currentColor = QColor(r, g, b);
    } else {
        unsigned int shifted = index >> 5;  // RGB colors in bits 6-30
        int r = (shifted & 0xff0000) >> 16;
        int g = (shifted & 0xff00) >> 8;
        int b = (shifted & 0xff);
        currentCG->currentColor = QColor(r, g, b);
    }
    
    // Set up the pen with the new color
    QPen pen(currentCG->currentColor);
    pen.setWidth(1);  // Or whatever line width you want
    currentCG->currentPainter->setPen(pen);
    
    // Also set the brush for filled operations
    currentCG->currentPainter->setBrush(currentCG->currentColor);
    
    oldcolor = index;
    return returnColor;
}

int QtCGWin::FilledPolygon(float *verts, int nverts)
{
    if (!currentCG || !currentCG->currentPainter || !verts) return 0;
    
    float h = currentCG->height();
    
    QPolygonF polygon;
    for (int i = 0; i < nverts; i++) {
        polygon << QPointF(verts[i*2], h - verts[i*2+1]);
    }
    
    currentCG->currentPainter->drawPolygon(polygon);
    
    return 0;
}

int QtCGWin::Circle(float x, float y, float width, int filled)
{
    if (!currentCG || !currentCG->currentPainter) return 0;
    
    float h = currentCG->height();
    y = h - y;
    
    if (filled) {
        currentCG->currentPainter->drawEllipse(QPointF(x + width/2, y + width/2), 
                                               width/2, width/2);
    } else {
        currentCG->currentPainter->save();
        currentCG->currentPainter->setBrush(Qt::NoBrush);
        currentCG->currentPainter->drawEllipse(QPointF(x + width/2, y + width/2), 
                                               width/2, width/2);
        currentCG->currentPainter->restore();
    }
    
    return 0;
}
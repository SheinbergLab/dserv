#include "qtcgwin.hpp"
#include <QMouseEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QDebug>
#include <cmath>

// Thread-local storage for current widget (Qt is single-threaded for GUI)
static thread_local QtCGWin* currentCG = nullptr;

// Color table from FLTK version
static const int NColorVals = 18;
static float colorvals[] = {
  /* R    G    B   Grey */
  0.0, 0.0, 0.0, 0.0, 
  0.1, 0.1, 0.4, 0.4,
  0.0, 0.35, 0.0, 0.1,
  0.0, 0.7, 0.7, 0.7,
  0.8, 0.05, 0.0, 0.3,
  0.8, 0.0, 0.8, 0.0,
  0.0, 0.0, 0.0, 0.0,
  0.0, 0.0, 0.0, 0.0,
  0.7, 0.7, 0.7, 0.7,
  0.3, 0.45, 0.9, 0.0,
  0.05, 0.95, 0.1, 0.0,
  0.0, 0.9, 0.9, 0.9,
  0.0, 0.0, 0.0, 0.0,
  0.0, 0.0, 0.0, 0.0,
  0.94, 0.94, 0.05, 0.8,
  0.0, 0.0, 0.0, 0.2,
  1.0, 1.0, 1.0, 1.0,
  0.96, 0.96, 0.96, 0.96,
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
{
    // Set widget properties
    setMinimumSize(200, 200);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(true);
    
    // Set background to white
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::white);
    setPalette(pal);
    
    // Graphics buffer initialization will be done by the Tcl module
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

void QtCGWin::paintEvent(QPaintEvent *event)
{
    if (!initialized) init();
    
    QPainter painter(this);
    currentPainter = &painter;
    currentCG = this;
    QtCGTabManager::getInstance().setCurrentCGWin(this);
    
    // Clear background
    painter.fillRect(rect(), Qt::white);
    
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

void QtCGWin::mousePressEvent(QMouseEvent *event)
{
    emit mousePressed(event->position());
}

void QtCGWin::mouseReleaseEvent(QMouseEvent *event)
{
    emit mouseReleased(event->position());
}

void QtCGWin::mouseMoveEvent(QMouseEvent *event)
{
    emit mouseMoved(event->position());
}

QtCGWin* QtCGWin::getCurrentInstance()
{
    return currentCG;
}

// Static cgraph callbacks
int QtCGWin::Clearwin()
{
    if (!currentCG || !currentCG->currentPainter) return 0;
    
    currentCG->currentPainter->fillRect(currentCG->rect(), Qt::white);
    return 0;
}

int QtCGWin::Line(float x0, float y0, float x1, float y1)
{
    if (!currentCG || !currentCG->currentPainter) return 0;
    
    // Flip Y coordinate (cgraph uses bottom-left origin)
    float h = currentCG->height();
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

// Remove static storage
int QtCGWin::Text(float x, float y, char *string)
{
    if (!currentCG || !currentCG->currentPainter || !string) return 0;
    
    float h = currentCG->height();  // Use widget height directly
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
    
    static int oldcolor = 0;  // Track color locally
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
    
    currentCG->currentPainter->setPen(currentCG->currentColor);
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

// QtCGTabManager implementation
QtCGTabManager& QtCGTabManager::getInstance()
{
    static QtCGTabManager instance;
    return instance;
}

void QtCGTabManager::addCGWin(const QString& name, QtCGWin* widget)
{
    cgwin_map[name] = widget;
}

QtCGWin* QtCGTabManager::getCGWin(const QString& name)
{
    return cgwin_map.value(name, nullptr);
}

void QtCGTabManager::removeCGWin(const QString& name)
{
    cgwin_map.remove(name);
}

QString QtCGTabManager::getNextTabName()
{
    return QString("cg%1").arg(next_tab_index++);
}

// QtCGTabWidget implementation
QtCGTabWidget::QtCGTabWidget(Tcl_Interp *interp, QWidget *parent)
    : QTabWidget(parent)
    , interp(interp)
{
    setTabsClosable(true);
    setMovable(true);
    
    connect(this, &QTabWidget::currentChanged, this, &QtCGTabWidget::onTabChanged);
    connect(this, &QTabWidget::tabCloseRequested, this, &QtCGTabWidget::onTabCloseRequested);
}

QString QtCGTabWidget::addCGTab(const QString& label)
{
    QString tabName = QtCGTabManager::getInstance().getNextTabName();
    QString tabLabel = label.isEmpty() ? tabName : label;
    
    auto cgwin = new QtCGWin(interp, this);
    QtCGTabManager::getInstance().addCGWin(tabName, cgwin);
    
    addTab(cgwin, tabLabel);
    setCurrentWidget(cgwin);
    
    // Connect graph update signal
    connect(cgwin, &QtCGWin::graphUpdated, this, &QtCGTabWidget::cgraphUpdated);
    
    return tabName;
}

bool QtCGTabWidget::selectCGTab(const QString& name)
{
    QtCGWin* widget = QtCGTabManager::getInstance().getCGWin(name);
    if (widget) {
        setCurrentWidget(widget);
        return true;
    }
    return false;
}

bool QtCGTabWidget::deleteCGTab(const QString& name)
{
    QtCGWin* widget = QtCGTabManager::getInstance().getCGWin(name);
    if (widget) {
        int index = indexOf(widget);
        if (index >= 0) {
            removeTab(index);
            QtCGTabManager::getInstance().removeCGWin(name);
            widget->deleteLater();
            return true;
        }
    }
    return false;
}

void QtCGTabWidget::onTabChanged(int index)
{
    if (index >= 0) {
        QtCGWin* widget = qobject_cast<QtCGWin*>(this->widget(index));
        if (widget) {
            currentCG = widget;
            QtCGTabManager::getInstance().setCurrentCGWin(widget);
            
            // Call Tcl to set the current buffer
            if (interp && widget->getGraphicsBuffer()) {
                QString cmd = QString("qtcgwin_set_current %1").arg((quintptr)widget->getGraphicsBuffer());
                Tcl_Eval(interp, cmd.toUtf8().constData());
            }
            
            widget->refresh();
        }
    }
}

void QtCGTabWidget::onTabCloseRequested(int index)
{
    QWidget* widget = this->widget(index);
    if (widget) {
        // Find the tab name
        QString tabName;
        auto& manager = QtCGTabManager::getInstance();
        QList<QString> names = manager.getAllNames();
        for (const QString& name : names) {
            if (manager.getCGWin(name) == widget) {
                tabName = name;
                break;
            }
        }
        
        if (!tabName.isEmpty()) {
            deleteCGTab(tabName);
        }
    }
}
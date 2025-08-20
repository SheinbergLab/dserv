#include "EssGraphicsWidget.h"
#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QToolBar>
#include <QVBoxLayout>
#include <QAction>
#include <QDebug>
#include <QTimer>
#include <QPixmap>
#include <QMutex>

// Static member for tracking current instance during painting
static thread_local EssGraphicsWidget* s_currentInstance = nullptr;

// Color name mapping (preserve from QtCGraph)
static QMap<QString, int> colorNameToIndex = {
    {"black", 0}, {"blue", 1}, {"dark_green", 2}, {"cyan", 3},
    {"red", 4}, {"magenta", 5}, {"brown", 6}, {"white", 7},
    {"gray", 8}, {"grey", 8}, {"light_blue", 9}, {"green", 10},
    {"light_cyan", 11}, {"deep_pink", 12}, {"medium_purple", 13},
    {"yellow", 14}, {"navy", 15}, {"bright_white", 16},
    {"light_gray", 17}, {"light_grey", 17}
};

EssGraphicsWidget::EssGraphicsWidget(const QString& name, QWidget* parent)
    : EssScriptableWidget(name.isEmpty() ? QString("graphics_%1").arg(QDateTime::currentMSecsSinceEpoch()) : name, parent)
    , m_pixmapValid(false)    
    , m_layoutMode(WithToolbar)        
    , m_controlType(NoControls)        
    , m_controlsVisible(true)          
    , m_mainSplitter(nullptr)          
    , m_sideControlsContainer(nullptr)
    , m_bottomControlsContainer(nullptr)                             
    , m_backgroundColor(Qt::white)
    , m_toolbar(nullptr)
    , m_graphWidget(nullptr)
    , m_returnToTabsAction(nullptr)
    , m_currentColor(Qt::black)
    , m_widgetFullyConstructed(false)
    , m_pendingGraphicsInit(false)
{
    // Set default setup script - but don't call graphics_init immediately
    setSetupScript(R"tcl(
# Graphics Widget Setup Script
local_log "Graphics widget script loaded - waiting for widget ready signal"

# Don't initialize graphics here - wait for the widget ready signal
proc graphics_init_when_ready {} {
    local_log "Initializing graphics system..."
    graphics_init
    local_log "Graphics system initialized"
}

local_log "Graphics widget setup script complete"
)tcl");
    
    initializeWidget();
    
    // Mark as fully constructed - this will trigger initialization
    m_widgetFullyConstructed = true;
}

EssGraphicsWidget::~EssGraphicsWidget() {
    QMutexLocker locker(&m_pixmapMutex);
    
    if (m_graphWidget) {
        m_graphWidget->setVisible(false);
        m_graphWidget->removeEventFilter(this);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    // Clear static instance if it points to us
    if (s_currentInstance == this) {
        s_currentInstance = nullptr;
    }
    
    // That's it! No gbuf cleanup needed since we don't manage it directly
    disconnect();
}

// In registerCustomCommands(), replace existing flushwin:
static int qt_flushwin_cmd(ClientData clientData, Tcl_Interp* interp, 
                          int objc, Tcl_Obj* const objv[]) {
    auto* widget = static_cast<EssGraphicsWidget*>(clientData);
    widget->flushGBufToWidget();
    return TCL_OK;
}

void EssGraphicsWidget::registerCustomCommands()
{
    if (!interpreter()) return;
    
    // Register enhanced graphics commands
    Tcl_CreateObjCommand(interpreter(), "graphics_init", tcl_graphics_init, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "graphics_clear", tcl_graphics_clear, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "graphics_export", tcl_graphics_export, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "graphics_bind", tcl_graphics_bind, this, nullptr);
   
    // Layout control commands (stubs for future enhancement)
    Tcl_CreateObjCommand(interpreter(), "graphics_layout", tcl_graphics_layout, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "graphics_controls", tcl_graphics_controls, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "graphics_reset_buffer", tcl_reset_gbuf, this, nullptr);
    
    Tcl_CreateObjCommand(interpreter(), "flushwin", qt_flushwin_cmd, this, nullptr);

    // Create aliases for standard cgraph commands
    const char* aliasScript = R"tcl(
        # Create standard cgraph command aliases
        proc clearwin {} { graphics_clear }
        proc cgexport {{filename {}}} { 
            if {$filename eq {}} { 
                graphics_export 
            } else { 
                graphics_export $filename 
            } 
        }
        
        # Convenience binding command
        proc cgbind {event script} { graphics_bind $event $script }
        
        # Window refresh
        proc refresh {} { 
            local_log "Refreshing graphics"
        }
        
        # Layout control stubs (for future use)
        proc show_controls {} { graphics_controls show }
        proc hide_controls {} { graphics_controls hide }
        proc set_layout {mode} { graphics_layout $mode }
    )tcl";
    
    eval(aliasScript);
}

QWidget* EssGraphicsWidget::createMainWidget()
{
    QWidget* mainWidget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(mainWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    
    // Create toolbar
    m_toolbar = new QToolBar();
    m_toolbar->setIconSize(QSize(16, 16));
    m_toolbar->setMaximumHeight(24);
    m_toolbar->setMovable(false);
    
    // Clear action
	QAction* clearAction = m_toolbar->addAction(
		QIcon::fromTheme("edit-clear-all", 
			QIcon::fromTheme("view-refresh", 
				style()->standardIcon(QStyle::SP_BrowserReload))), 
		tr("Clear"));
	clearAction->setToolTip(tr("Clear the graphics canvas"));
	connect(clearAction, &QAction::triggered, this, &EssGraphicsWidget::clear);
	
	m_toolbar->addSeparator();
    
    // Export action
	QAction* exportAction = m_toolbar->addAction(
		QIcon::fromTheme("document-export", 
			QIcon::fromTheme("document-send", 
				QIcon::fromTheme("go-down", 
					style()->standardIcon(QStyle::SP_ArrowDown)))), 
		tr("Export"));
	exportAction->setToolTip(tr("Export graphics to PDF file"));
	connect(exportAction, &QAction::triggered, [this]() {
		exportToPDFDialog();
	});
	
	m_toolbar->addSeparator();
		
    // Floating action
	m_floatingAction = m_toolbar->addAction(
		QIcon::fromTheme("window-new", 
			QIcon::fromTheme("view-fullscreen", 
				style()->standardIcon(QStyle::SP_TitleBarMaxButton))), 
		tr("Float"));
	m_floatingAction->setToolTip(tr("Detach widget to floating window"));
	m_floatingAction->setCheckable(true);
	connect(m_floatingAction, &QAction::toggled, this, 
		&EssGraphicsWidget::onFloatingToggled);

    // Add "return to tabs" action (only visible when floating)
	m_returnToTabsAction = m_toolbar->addAction(
		QIcon::fromTheme("go-home",
			style()->standardIcon(QStyle::SP_ArrowBack)), tr("To Tabs"));
	m_returnToTabsAction->setToolTip(tr("Return to tab container"));
	m_returnToTabsAction->setVisible(false);
	connect(m_returnToTabsAction, &QAction::triggered, [this]() {
		emit returnToTabsRequested();
	});
    
	m_statusLabel = new QLabel();
	m_statusLabel->setStyleSheet("QLabel { color: #666; font-size: 11px; }");
	m_statusLabel->setText("Ready");
	m_toolbar->addWidget(m_statusLabel);
		
    // Add spacer
    QWidget* spacer = new QWidget();
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolbar->addWidget(spacer);
    
    // Add to layout
    layout->addWidget(m_toolbar);
    
    // Create content area using new method
    QWidget* contentArea = createContentArea();
    layout->addWidget(contentArea, 1);
    
    return mainWidget;
}

QWidget* EssGraphicsWidget::createContentArea()
{
    // Create the actual graph widget if not already created
    if (!m_graphWidget) {
        m_graphWidget = new QWidget();
        m_graphWidget->setMinimumSize(200, 150);
        m_graphWidget->setAttribute(Qt::WA_OpaquePaintEvent);
        m_graphWidget->setAutoFillBackground(true);
        m_graphWidget->setFocusPolicy(Qt::ClickFocus);
        
        // Install event filter on the graph widget to handle paint/mouse events
        m_graphWidget->installEventFilter(this);
        
        // Set background
        QPalette pal = m_graphWidget->palette();
        pal.setColor(QPalette::Window, m_backgroundColor);
        m_graphWidget->setPalette(pal);
    }
    
    switch (m_layoutMode) {
        case GraphicsOnly:
        case WithToolbar:
        default:
            return m_graphWidget;
            
        case SideControls:
            localLog("Side controls layout not yet implemented, using graphics only");
            return m_graphWidget;
            
        case BottomControls:
            localLog("Bottom controls layout not yet implemented, using graphics only");
            return m_graphWidget;
    }
}

void EssGraphicsWidget::onFloatingToggled(bool floating)
{
    // Update button states first
    m_floatingAction->setVisible(!floating);
    m_returnToTabsAction->setVisible(floating);
    
    if (m_statusLabel) {
        m_statusLabel->setText(floating ? "Floating" : "Docked");
    }
    
    // Emit signal for workspace manager to handle the actual floating
    emit floatingRequested(floating);
}

void EssGraphicsWidget::showEvent(QShowEvent* event) {
    EssScriptableWidget::showEvent(event);
    
    // Initialize graphics when widget is first shown with valid size
    static bool initialized = false;
    if (!initialized && m_graphWidget && 
        m_graphWidget->width() > 0 && m_graphWidget->height() > 0) {
        
        if (interpreter()) {
            // Set initial resolution
            QString cmd = QString("setresol %1 %2; gbufreset")
                         .arg(m_graphWidget->width())
                         .arg(m_graphWidget->height());
            eval(cmd);
            clear();
            
            initialized = true;
            localLog("Graphics initialized on first show");
        }
    }
}

void EssGraphicsWidget::flushGBufToWidget() {
    if (!interpreter()) return;
    
    // Use Tcl to dump gbuf to string - result goes directly to interpreter result
    int result = Tcl_Eval(interpreter(), "dumpwin string");
    
    if (result == TCL_OK) {
        // Get the result string directly
        const char* commands = Tcl_GetStringResult(interpreter());
        
        if (commands && strlen(commands) > 0) {
            renderCommands(QString(commands));
        }
    } else {
        localLog(QString("Failed to dump gbuf: %1").arg(Tcl_GetStringResult(interpreter())));
    }
}

void EssGraphicsWidget::renderCommands(const QString& commands) {
    m_lastGBCommands = commands;
    
    ensurePixmapSize();
    QMutexLocker locker(&m_pixmapMutex);
    m_pixmap.fill(m_backgroundColor);
    
    QPainter painter(&m_pixmap);
    //painter.setRenderHint(QPainter::Antialiasing);
    
    painter.setPen(Qt::black);
    painter.setBrush(Qt::black);
    painter.setFont(QFont("Helvetica", 10));
    
    // Initialize drawing state
    m_currentPos = QPointF(0, 0);
            
    QStringList lines = commands.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        if (line.startsWith('#')) continue;
        QStringList parts = line.split('\t', Qt::SkipEmptyParts);
        executeGBCommand(parts, &painter);
    }
    
    locker.unlock();
    if (m_graphWidget) m_graphWidget->update();

    if (m_graphWidget) {
        m_graphWidget->update();
    }
}

float EssGraphicsWidget::flipY(float y) const {
    // Use the same approach as bridge callbacks: h - y
    return m_graphWidget ? m_graphWidget->height() - y : y;
}

void EssGraphicsWidget::executeGBCommand(const QStringList &parts,
                                         QPainter *painter) {
  if (parts.isEmpty())
    return;

  QString cmd = parts[0];
  QColor currentColor;
  
  if (cmd == "setwindow" && parts.size() >= 5) {
    float x0 = parts[1].toFloat();
    float y0 = parts[2].toFloat();
    float x1 = parts[3].toFloat();
    float y1 = parts[4].toFloat();

    float originalWidth = x1 - x0;
    float originalHeight = y1 - y0;

    if (originalWidth > 0 && originalHeight > 0 && m_graphWidget) {
      float currentWidth = (float)m_graphWidget->width();
      float currentHeight = (float)m_graphWidget->height();

      float scaleX = currentWidth / originalWidth;
      float scaleY = currentHeight / originalHeight;

      if (scaleX != 1.0 || scaleY != 1.0) {
        // Simple scaling only - no coordinate system changes
        painter->scale(scaleX, scaleY);
      } else {
      }
    }
  } else if (cmd == "setfont" && parts.size() >= 3) {
    QString fontName = parts[1];
    float size = parts[2].trimmed().toFloat();
    QFont font(fontName, (int)size);
    painter->setFont(font);
  } else if (cmd == "setcolor" && parts.size() >= 2) {
    int colorIndex = parts[1].trimmed().toInt();
    currentColor = cgraphColorToQt(colorIndex);
    painter->setPen(currentColor);
    painter->setBrush(currentColor);
  } else if (cmd == "setlstyle" && parts.size() >= 2) {
    int style = parts[1].trimmed().toInt();
    Qt::PenStyle penStyle = (style == 0) ? Qt::NoPen : Qt::SolidLine;
    QPen pen = painter->pen();
    pen.setStyle(penStyle);
    painter->setPen(pen);
  } else if (cmd == "setlwidth" && parts.size() >= 2) {
    int width = parts[1].trimmed().toInt();
    QPen pen = painter->pen();
    pen.setWidth(qMax(1, width / 100)); // Convert from cgraph units, minimum 1
    painter->setPen(pen);
  } else if (cmd == "setorientation" && parts.size() >= 2) {
    m_textOrientation = parts[1].trimmed().toInt();
  } else if (cmd == "setjust" && parts.size() >= 2) {
    m_textJustification = parts[1].trimmed().toInt();
  } else if (cmd == "setclipregion" && parts.size() >= 2) {
    QStringList coords = parts[1].split(' ', Qt::SkipEmptyParts);
    if (coords.size() >= 4) {
      float x0 = coords[0].toFloat();
      float y0 = coords[1].toFloat();
      float x1 = coords[2].toFloat();
      float y1 = coords[3].toFloat();

      // Convert to Qt coordinates with flipY
      float qtY0 = flipY(y0);
      float qtY1 = flipY(y1);

      // Create clip rectangle from corners
      float left = qMin(x0, x1);
      float top = qMin(qtY0, qtY1);
      float width = qAbs(x1 - x0);
      float height = qAbs(qtY0 - qtY1);

      QRectF clipRect(left, top, width, height);
      painter->setClipRect(clipRect);
    }
  } else if (cmd == "filledrect" && parts.size() >= 2) {
    QStringList coords = parts[1].split(' ', Qt::SkipEmptyParts);
    if (coords.size() >= 4) {
      float x0 = coords[0].toFloat();
      float y0 = coords[1].toFloat();
      float x1 = coords[2].toFloat();
      float y1 = coords[3].toFloat();

      // Convert to Qt coordinates with flipY
      float qtY0 = flipY(y0);
      float qtY1 = flipY(y1);

      // Create rectangle from corners
      float left = qMin(x0, x1);
      float top = qMin(qtY0, qtY1);
      float width = qAbs(x1 - x0);
      float height = qAbs(qtY0 - qtY1);

      QRectF rect(left, top, width, height);
      if (width > 0 && height > 0) {
        painter->fillRect(rect, painter->brush());
      }
    }
  } else if (cmd == "moveto" && parts.size() >= 2) {
    QStringList coords = parts[1].split(' ', Qt::SkipEmptyParts);
    if (coords.size() >= 2) {
      m_currentPos = QPointF(coords[0].toFloat(), flipY(coords[1].toFloat()));
    }
  } else if (cmd == "lineto" && parts.size() >= 2) {
    QStringList coords = parts[1].split(' ', Qt::SkipEmptyParts);
    if (coords.size() >= 2) {
      QPointF newPos(coords[0].toFloat(), flipY(coords[1].toFloat()));
      painter->drawLine(m_currentPos, newPos);
      m_currentPos = newPos;
    }
  } else if (cmd == "line" && parts.size() >= 2) {
    QStringList coords = parts[1].split(' ', Qt::SkipEmptyParts);
    if (coords.size() >= 4) {
      float x0 = coords[0].toFloat();
      float y0 = flipY(coords[1].toFloat());
      float x1 = coords[2].toFloat();
      float y1 = flipY(coords[3].toFloat());
      painter->drawLine(QPointF(x0, y0), QPointF(x1, y1));
    }
  } else if (cmd == "circle" && parts.size() >= 2) {
    QStringList coords = parts[1].split(' ', Qt::SkipEmptyParts);
    if (coords.size() >= 4) {
      float x = coords[0].toFloat();
      float y = flipY(coords[1].toFloat());
      float radius = coords[2].toFloat()/2;
      bool filled = coords[3].toInt();

	  painter->setBrush(Qt::NoBrush);
	  painter->drawEllipse(QPointF(x, y), radius, radius);
    }
  } else if (cmd == "fcircle" && parts.size() >= 2) {
    QStringList coords = parts[1].split(' ', Qt::SkipEmptyParts);
    if (coords.size() >= 4) {
      float x = coords[0].toFloat();
      float y = flipY(coords[1].toFloat());
      float radius = coords[2].toFloat()/2;
      bool filled = coords[3].toInt();

	  painter->setBrush(currentColor);
      painter->setPen(Qt::NoPen);
      painter->drawEllipse(QPointF(x, y), radius, radius);
     }
  }

  else if (cmd == "drawtext" && parts.size() >= 2) {
    QString text = parts[1];
    if (text.startsWith('"') && text.endsWith('"')) {
      text = text.mid(1, text.length() - 2);
    }

    QFontMetrics fm(painter->font());
    QRectF textRect = fm.boundingRect(text);

    float hoff = 0, voff = 0;
    int ori = m_textOrientation;
    int just = m_textJustification;

    if (ori == 0 || ori == 2) { // horizontal
      switch (just) {
      case -1:
        hoff = 0;
        break;
      case 1:
        hoff = textRect.width();
        break;
      case 0:
        hoff = textRect.width() * 0.5;
        break;
      }
      voff = (textRect.height()) * 0.5;
    } else { // vertical
      switch (just) {
      case -1:
        voff = 0;
        break;
      case 1:
        voff = textRect.width();
        break;
      case 0:
        voff = textRect.width() * 0.5;
        break;
      }
      hoff = (textRect.height()) * 0.5;
    }

    painter->save();

    // Draw a small cross at the current point for reference
#if 0 
    painter->setPen(QPen(Qt::red, 1));
    painter->drawLine(m_currentPos.x() - 5, m_currentPos.y(),
                      m_currentPos.x() + 5, m_currentPos.y());
    painter->drawLine(m_currentPos.x(), m_currentPos.y() - 5, m_currentPos.x(),
                      m_currentPos.y() + 5);
    painter->setPen(QPen(Qt::black, 1));
#endif
    if (ori == 0) {
      painter->translate(m_currentPos.x() - hoff,
                         m_currentPos.y() + textRect.height() * 0.5);
      painter->rotate(-ori * 90);
      painter->drawText(0, 0, text);
    } else if (ori == 1) {
      painter->translate(m_currentPos.x() + (textRect.height() * .5),
                         m_currentPos.y() + voff);
      painter->rotate(-ori * 90);
      painter->drawText(0, 0, text);
    } else if (ori == 2) {
      painter->translate(m_currentPos.x() + hoff, m_currentPos.y() - voff);
      painter->rotate(-ori * 90);
      painter->drawText(0, 0, text);
    } else if (ori == 3) {
      painter->translate(m_currentPos.x() - hoff, m_currentPos.y() - voff);
      painter->rotate(-ori * 90);
      painter->drawText(0, 0, text);
    }
    painter->restore();
  } else if (cmd == "point" && parts.size() >= 2) {
    QStringList coords = parts[1].split(' ', Qt::SkipEmptyParts);
    if (coords.size() >= 2) {
      float x = coords[0].toFloat();
      float y = flipY(coords[1].toFloat());
      painter->drawPoint(QPointF(x, y));
    }
  }
  // Skip header and other non-drawing commands
  else if (cmd.startsWith('#') || cmd == "setversion") {
    // Ignore comments and version info
  }
}

void EssGraphicsWidget::resizeEvent(QResizeEvent* event) {
    EssScriptableWidget::resizeEvent(event);
	// handle cgraph pixmap resize in eventFilter()
}


bool EssGraphicsWidget::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_graphWidget) {
		if (event->type() == QEvent::Paint) {
			QPainter widgetPainter(m_graphWidget);  // Make sure this is the RIGHT widget
			
			QMutexLocker locker(&m_pixmapMutex);
			if (!m_pixmap.isNull()) {
				widgetPainter.drawPixmap(0, 0, m_pixmap);
			} else {
				widgetPainter.fillRect(m_graphWidget->rect(), m_backgroundColor);
			}
			
			return true;
		}
		else if (event->type() == QEvent::Resize) {
			QResizeEvent* resizeEvent = static_cast<QResizeEvent*>(event);
			QSize newSize = resizeEvent->size();
			
			if (newSize.width() > 0 && newSize.height() > 0) {
				// Update resolution, and reset gbuf only if it's empty
				if (interpreter()) {
					QString cmd = QString("setresol %1 %2; if { [gbufisempty] } { gbufreset }")
								 .arg(newSize.width())
								 .arg(newSize.height());
					eval(cmd);
				}
				
				// Re-render with new scaling
				flushGBufToWidget();
			}
			return false;  // Let widget handle its own resize
		}
        else if (event->type() == QEvent::MouseButtonPress) {
            onMousePressEvent(static_cast<QMouseEvent*>(event));
            return true;
        }
        else if (event->type() == QEvent::MouseButtonRelease) {
            onMouseReleaseEvent(static_cast<QMouseEvent*>(event));
            return true;
        }
        else if (event->type() == QEvent::MouseMove) {
            onMouseMoveEvent(static_cast<QMouseEvent*>(event));
            return true;
        }
        else if (event->type() == QEvent::MouseButtonDblClick) {
            onMouseDoubleClickEvent(static_cast<QMouseEvent*>(event));
            return true;
        }
        else if (event->type() == QEvent::Wheel) {
            onWheelEvent(static_cast<QWheelEvent*>(event));
            return true;
        }
        else if (event->type() == QEvent::KeyPress) {
            onKeyPressEvent(static_cast<QKeyEvent*>(event));
            return true;
        }
        else if (event->type() == QEvent::KeyRelease) {
            onKeyReleaseEvent(static_cast<QKeyEvent*>(event));
            return true;
        }
        else if (event->type() == QEvent::FocusIn) {
            onFocusInEvent(static_cast<QFocusEvent*>(event));
            return true;
        }
        else if (event->type() == QEvent::FocusOut) {
            onFocusOutEvent(static_cast<QFocusEvent*>(event));
            return true;
        }
    }
    
    // Let the base class handle it
    return EssScriptableWidget::eventFilter(obj, event);
}

void EssGraphicsWidget::ensurePixmapSize()
{
    if (!m_graphWidget) return;
    
    QSize currentSize = m_graphWidget->size();
    if (currentSize.width() <= 0 || currentSize.height() <= 0) {
        return;
    }
    
    QMutexLocker locker(&m_pixmapMutex);
    
    // Check if pixmap needs to be resized  
    if (m_pixmap.size() != currentSize) {
        // Use logical size, not device pixel ratio scaled
        m_pixmap = QPixmap(currentSize);
        // m_pixmap.setDevicePixelRatio(devicePixelRatio());  // Comment out for now
        m_pixmap.fill(m_backgroundColor);
        
        m_pixmapValid = false;
    }
}

void EssGraphicsWidget::invalidatePixmap()
{
    QMutexLocker locker(&m_pixmapMutex);
    m_pixmapValid = false;
}

int EssGraphicsWidget::eval(const QString& command)
{
    if (!interpreter()) {
        localLog("No interpreter available");
        return TCL_ERROR;
    }
    
    // Call base class eval() to do the work
    return EssScriptableWidget::eval(command);
}

void EssGraphicsWidget::refresh()
{
    flushGBufToWidget();
}

void EssGraphicsWidget::clear() {
    // Clear the cgraph buffer via a different command or direct call
    if (interpreter()) {
        eval("setfviewport 0 0 1 1; gbufreset");  // if this exists
    }
    
    // Clear cached commands
    m_lastGBCommands.clear();

    // Clear our pixmap directly
    QMutexLocker locker(&m_pixmapMutex);
    if (!m_pixmap.isNull()) {
        m_pixmap.fill(m_backgroundColor);
    }
        
    // Trigger widget update
    if (m_graphWidget) {
        m_graphWidget->update();
    }
}

void EssGraphicsWidget::setBackgroundColor(const QColor& color) {
    m_backgroundColor = color;
    
    // Update the widget palette
    if (m_graphWidget) {
        QPalette pal = m_graphWidget->palette();
        pal.setColor(QPalette::Window, m_backgroundColor);
        m_graphWidget->setPalette(pal);
    }
    
    // If we have cached commands, re-render with new background
    if (!m_lastGBCommands.isEmpty()) {
        renderCommands(m_lastGBCommands);
    } else {
        // Just clear to new background color
        QMutexLocker locker(&m_pixmapMutex);
        if (!m_pixmap.isNull()) {
            m_pixmap.fill(m_backgroundColor);
        }
        
        if (m_graphWidget) {
            m_graphWidget->update();
        }
    }
}

void EssGraphicsWidget::setFloatingMode(bool floating)
{
    m_isFloating = floating;
    if (m_returnToTabsAction) {
        m_returnToTabsAction->setVisible(floating);
    }
}


void EssGraphicsWidget::applyDevelopmentLayout()
{
    // Call base class implementation first
    EssScriptableWidget::applyDevelopmentLayout();
}

// Event handling methods (preserve QtCGraph event system)
QString EssGraphicsWidget::substituteEventData(const QString& script, QEvent* event,
                                              const QPointF& pos, int button, int delta)
{
    QString result = script;
    
    // Basic position substitutions
    if (pos != QPointF()) {
        int pixelX = (int)pos.x();
        int pixelY = (int)pos.y();
        
        // Get window coordinates from cgraph via Tcl
        if (interpreter()) {
            // Get window bounds: llx lly urx ury
            if (Tcl_Eval(interpreter(), "getwindow") == TCL_OK) {
                QString windowStr = Tcl_GetStringResult(interpreter());
                QStringList coords = windowStr.split(' ', Qt::SkipEmptyParts);
                
                if (coords.size() >= 4) {
                    float llx = coords[0].toFloat();  // lower left x
                    float lly = coords[1].toFloat();  // lower left y  
                    float urx = coords[2].toFloat();  // upper right x
                    float ury = coords[3].toFloat();  // upper right y
                    
                    // Get widget/pixmap size
                    int widgetWidth = m_graphWidget ? m_graphWidget->width() : 400;
                    int widgetHeight = m_graphWidget ? m_graphWidget->height() : 300;
                    
                    // Transform pixel coordinates to window coordinates
                    // First flip Y (Qt top-left to cgraph bottom-left)
                    float screenY = (widgetHeight - 1) - pixelY;
                    float screenX = pixelX;
                    
                    // Scale to window coordinates
                    float winX = llx + (screenX / (float)widgetWidth) * (urx - llx);
                    float winY = lly + (screenY / (float)widgetHeight) * (ury - lly);
                    
                    // %x, %y - window coordinates
                    result.replace("%x", QString::number(winX, 'f', 2));
                    result.replace("%y", QString::number(winY, 'f', 2));
                } else {
                    // Fallback to pixel coordinates
                    result.replace("%x", QString::number(pixelX));
                    result.replace("%y", QString::number(pixelY));
                }
            } else {
                // Tcl call failed, use pixel coordinates
                result.replace("%x", QString::number(pixelX));
                result.replace("%y", QString::number(pixelY));
            }
        } else {
            // No interpreter, use pixel coordinates
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
    result.replace("%w", QString::number(m_graphWidget->width()));
    result.replace("%h", QString::number(m_graphWidget->height()));
    result.replace("%W", name());
    
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

QString EssGraphicsWidget::keyToString(QKeyEvent* event) const
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

void EssGraphicsWidget::onMousePressEvent(QMouseEvent *event)
{
    if (!m_mouseDownScript.isEmpty() && interpreter()) {
        int button = event->button() == Qt::LeftButton ? 1 : 
                     event->button() == Qt::RightButton ? 3 : 
                     event->button() == Qt::MiddleButton ? 2 : 0;
        
        QString cmd = substituteEventData(m_mouseDownScript, event, 
                                         event->position(), button);
        eval(cmd);
    }
    emit mousePressed(event->position());
}

void EssGraphicsWidget::onMouseReleaseEvent(QMouseEvent *event)
{
    if (!m_mouseUpScript.isEmpty() && interpreter()) {
        int button = event->button() == Qt::LeftButton ? 1 : 
                     event->button() == Qt::RightButton ? 3 : 
                     event->button() == Qt::MiddleButton ? 2 : 0;
        
        QString cmd = substituteEventData(m_mouseUpScript, event, 
                                         event->position(), button);
        eval(cmd);
    }
    emit mouseReleased(event->position());
}

void EssGraphicsWidget::onMouseMoveEvent(QMouseEvent *event)
{
    if (!m_mouseMoveScript.isEmpty() && interpreter()) {
        QString cmd = substituteEventData(m_mouseMoveScript, event, 
                                         event->position());
        eval(cmd);
    }
    emit mouseMoved(event->position());
}

void EssGraphicsWidget::onMouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_mouseDoubleClickScript.isEmpty() && interpreter()) {
        int button = event->button() == Qt::LeftButton ? 1 : 
                     event->button() == Qt::RightButton ? 3 : 
                     event->button() == Qt::MiddleButton ? 2 : 0;
        
        QString cmd = substituteEventData(m_mouseDoubleClickScript, event, 
                                         event->position(), button);
        eval(cmd);
    }
    emit mouseDoubleClicked(event->position());
}

void EssGraphicsWidget::onWheelEvent(QWheelEvent *event)
{
    if (!m_mouseWheelScript.isEmpty() && interpreter()) {
        int delta = event->angleDelta().y();
        QString cmd = substituteEventData(m_mouseWheelScript, event, 
                                         event->position(), -1, delta);
        eval(cmd);
    }
}

void EssGraphicsWidget::onKeyPressEvent(QKeyEvent *event)
{
    if (!m_keyPressScript.isEmpty() && interpreter()) {
        QString cmd = substituteEventData(m_keyPressScript, event);
        eval(cmd);
    }
}

void EssGraphicsWidget::onKeyReleaseEvent(QKeyEvent *event)
{
    if (!m_keyReleaseScript.isEmpty() && interpreter()) {
        QString cmd = substituteEventData(m_keyReleaseScript, event);
        eval(cmd);
    }
}

void EssGraphicsWidget::onFocusInEvent(QFocusEvent *event)
{
    if (!m_focusInScript.isEmpty() && interpreter()) {
        QString cmd = substituteEventData(m_focusInScript, event);
        eval(cmd);
    }
}

void EssGraphicsWidget::onFocusOutEvent(QFocusEvent *event)
{
    if (!m_focusOutScript.isEmpty() && interpreter()) {
        QString cmd = substituteEventData(m_focusOutScript, event);
        eval(cmd);
    }
}

bool EssGraphicsWidget::exportToPDF(const QString& filename)
{
    if (!interpreter()) {
        localLog("Cannot export: not initialized");
        return false;
    }
    
    // Make sure we're the current graph
    s_currentInstance = this;
    
    // Use cgraph's dumpwin command
    QString cmd = QString("dumpwin pdf {%1}").arg(filename);
    int result = eval(cmd);
    
    if (result != TCL_OK) {
        localLog(QString("PDF export failed: %1").arg(this->result()));
        return false;
    }
    
    return true;
}

// Layout control methods (stubs for future enhancement)
void EssGraphicsWidget::setLayoutMode(LayoutMode mode)
{
    if (m_layoutMode == mode) return;
    
    m_layoutMode = mode;
    
    emit layoutModeChanged(mode);
    
    QString modeStr;
    switch (mode) {
        case GraphicsOnly: modeStr = "Graphics Only"; break;
        case WithToolbar: modeStr = "With Toolbar"; break;
        case SideControls: modeStr = "Side Controls"; break;
        case BottomControls: modeStr = "Bottom Controls"; break;
    }
}

void EssGraphicsWidget::setControlPanelType(ControlPanelType type)
{
    if (m_controlType == type) return;
    
    m_controlType = type;
    
    QString typeStr;
    switch (type) {
        case NoControls: typeStr = "No Controls"; break;
        case ExperimentControls: typeStr = "Experiment Controls"; break;
        case PlotControls: typeStr = "Plot Controls"; break;
        case CustomControls: typeStr = "Custom Controls"; break;
    }
    
    localLog(QString("Control panel type changed to: %1").arg(typeStr));
}

void EssGraphicsWidget::setControlsVisible(bool visible)
{
    if (m_controlsVisible == visible) return;
    
    m_controlsVisible = visible;
    
    localLog(QString("Controls visibility: %1").arg(visible ? "visible" : "hidden"));
}

bool EssGraphicsWidget::exportToPDFDialog(const QString& suggestedName)
{
    QString suggestion = suggestedName;
    if (suggestion.isEmpty()) {
        suggestion = name() + ".pdf";
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


QColor EssGraphicsWidget::cgraphColorToQt(int colorIndex) const {
    // Standard cgraph color table
    static const QColor colorTable[] = {
        Qt::black,              // 0
        Qt::blue,               // 1
        Qt::darkGreen,          // 2
        Qt::cyan,               // 3
        Qt::red,                // 4
        Qt::magenta,            // 5
        QColor(165, 42, 42),    // 6 - brown
        Qt::white,              // 7
        Qt::gray,               // 8
        QColor(173, 216, 230),  // 9 - light blue
        Qt::green,              // 10
        QColor(224, 255, 255),  // 11 - light cyan
        QColor(255, 20, 147),   // 12 - deep pink
        QColor(147, 112, 219),  // 13 - medium purple
        Qt::yellow,             // 14
        QColor(0, 0, 128),      // 15 - navy
        Qt::white,              // 16 - bright white
        Qt::lightGray           // 17
    };
    
    if (colorIndex >= 0 && colorIndex < 18) {
        return colorTable[colorIndex];
    }
    
    // Handle RGB colors encoded in upper bits (if you use them)
    if (colorIndex > 18) {
        unsigned int shifted = colorIndex >> 5;
        int r = (shifted & 0xff0000) >> 16;
        int g = (shifted & 0xff00) >> 8;
        int b = (shifted & 0xff);
        return QColor(r, g, b);
    }
    
    return Qt::black; // Default
}

// Tcl command implementations
int EssGraphicsWidget::tcl_graphics_init(ClientData clientData, Tcl_Interp* interp,
                                         int objc, Tcl_Obj* const objv[])
{   
    Tcl_SetObjResult(interp, Tcl_NewStringObj("graphics initialized", -1));
    return TCL_OK;
}

int EssGraphicsWidget::tcl_graphics_clear(ClientData clientData, Tcl_Interp* interp,
                                         int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssGraphicsWidget*>(clientData);
    
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    widget->clear();
    
    return TCL_OK;
}

int EssGraphicsWidget::tcl_graphics_export(ClientData clientData, Tcl_Interp* interp,
                                          int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssGraphicsWidget*>(clientData);
    
    QString filename;
    
    if (objc > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?filename?");
        return TCL_ERROR;
    }
    
    bool success;
    if (objc == 2) {
        // Filename provided
        filename = Tcl_GetString(objv[1]);
        success = widget->exportToPDF(filename);
    } else {
        // No filename - show dialog
        success = widget->exportToPDFDialog();
    }
    
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(success));
    return TCL_OK;
}

int EssGraphicsWidget::tcl_graphics_bind(ClientData clientData, Tcl_Interp* interp,
                                        int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssGraphicsWidget*>(clientData);
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "event script");
        return TCL_ERROR;
    }
    
    const char* event = Tcl_GetString(objv[1]);
    const char* script = Tcl_GetString(objv[2]);
    
    // Mouse events
    if (strcmp(event, "<ButtonPress>") == 0 || strcmp(event, "<Button>") == 0) {
        widget->setMouseDownScript(script);
    } else if (strcmp(event, "<ButtonRelease>") == 0) {
        widget->setMouseUpScript(script);
    } else if (strcmp(event, "<Motion>") == 0) {
        widget->setMouseMoveScript(script);
        widget->setMouseTracking(strlen(script) > 0);
    } else if (strcmp(event, "<Double-Button>") == 0) {
        widget->setMouseDoubleClickScript(script);
    } else if (strcmp(event, "<MouseWheel>") == 0) {
        widget->setMouseWheelScript(script);
    }
    // Keyboard events
    else if (strcmp(event, "<KeyPress>") == 0 || strcmp(event, "<Key>") == 0) {
        widget->setKeyPressScript(script);
    } else if (strcmp(event, "<KeyRelease>") == 0) {
        widget->setKeyReleaseScript(script);
    }
    // Focus events
    else if (strcmp(event, "<FocusIn>") == 0) {
        widget->setFocusInScript(script);
    } else if (strcmp(event, "<FocusOut>") == 0) {
        widget->setFocusOutScript(script);
    }
    else {
        Tcl_AppendResult(interp, "Unknown event: ", event, 
            ". Supported events: <ButtonPress>, <ButtonRelease>, <Motion>, "
            "<Double-Button>, <MouseWheel>, <KeyPress>, <KeyRelease>, "
            "<FocusIn>, <FocusOut>", NULL);
        return TCL_ERROR;
    }
    
    return TCL_OK;
}

int EssGraphicsWidget::tcl_reset_gbuf(ClientData clientData, Tcl_Interp* interp, 
                       		  int objc, Tcl_Obj* const objv[]) 
{
    auto* widget = static_cast<EssGraphicsWidget*>(clientData);
    
    if (!widget->graphWidget()) {
        Tcl_SetResult(interp, "Graphics widget not ready", TCL_STATIC);
        return TCL_ERROR;
    }
    
    int width = widget->graphWidget()->width();
    int height = widget->graphWidget()->height();
    
    if (width <= 0 || height <= 0) {
        Tcl_SetResult(interp, "Graphics widget has invalid size", TCL_STATIC);
        return TCL_ERROR;
    }
    
    // Update cgraph resolution
    QString cmd = QString("setresol %1 %2; gbResetCurrentBuffer").arg(width).arg(height);
    widget->eval(cmd);
    
    return TCL_OK;
}
// Layout control Tcl commands (stubs for future enhancement)
int EssGraphicsWidget::tcl_graphics_layout(ClientData clientData, Tcl_Interp* interp,
                                          int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssGraphicsWidget*>(clientData);
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "layout_mode");
        return TCL_ERROR;
    }
    
    QString modeStr = QString(Tcl_GetString(objv[1])).toLower();
    
    LayoutMode mode = WithToolbar;  // default
    if (modeStr == "graphics_only") {
        mode = GraphicsOnly;
    } else if (modeStr == "with_toolbar") {
        mode = WithToolbar;
    } else if (modeStr == "side_controls") {
        mode = SideControls;
    } else if (modeStr == "bottom_controls") {
        mode = BottomControls;
    } else {
        Tcl_AppendResult(interp, "Unknown layout mode: ", Tcl_GetString(objv[1]), 
            ". Valid modes: graphics_only, with_toolbar, side_controls, bottom_controls", NULL);
        return TCL_ERROR;
    }
    
    widget->setLayoutMode(mode);
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj("layout updated", -1));
    return TCL_OK;
}

int EssGraphicsWidget::tcl_graphics_controls(ClientData clientData, Tcl_Interp* interp,
                                           int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssGraphicsWidget*>(clientData);
    
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "command ?args?");
        return TCL_ERROR;
    }
    
    QString command = QString(Tcl_GetString(objv[1])).toLower();
    
    if (command == "show") {
        widget->setControlsVisible(true);
    } else if (command == "hide") {
        widget->setControlsVisible(false);
    } else if (command == "experiment") {
        widget->setControlPanelType(ExperimentControls);
        if (objc == 3) {
            QString action = QString(Tcl_GetString(objv[2])).toLower();
            if (action == "show") {
                widget->setControlsVisible(true);
            } else if (action == "hide") {
                widget->setControlsVisible(false);
            }
        }
    } else if (command == "plot") {
        widget->setControlPanelType(PlotControls);
    } else {
        Tcl_AppendResult(interp, "Unknown controls command: ", Tcl_GetString(objv[1]), 
            ". Valid commands: show, hide, experiment, plot", NULL);
        return TCL_ERROR;
    }
    
    return TCL_OK;
}
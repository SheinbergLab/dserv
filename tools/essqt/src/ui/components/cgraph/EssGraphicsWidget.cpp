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
    , m_pixmapPainter(nullptr)
    , m_pixmapValid(false)    , m_layoutMode(WithToolbar)        
    , m_controlType(NoControls)        
    , m_controlsVisible(true)          
    , m_mainSplitter(nullptr)          
    , m_sideControlsContainer(nullptr)
    , m_bottomControlsContainer(nullptr)
    , m_gbuf(nullptr)                  
    , m_frame(nullptr)                 
    , m_graphicsInitialized(false)
    , m_backgroundColor(Qt::white)
    , m_toolbar(nullptr)
    , m_graphWidget(nullptr)
    , m_returnToTabsAction(nullptr)
    , m_currentPainter(nullptr)
    , m_currentColor(Qt::black)
    , m_widgetFullyConstructed(false)
    , m_pendingGraphicsInit(false)
{
    // Connect the signal for delayed initialization
    connect(this, &EssGraphicsWidget::widgetReady, 
            this, &EssGraphicsWidget::onWidgetReady, Qt::QueuedConnection);
    
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

EssGraphicsWidget::~EssGraphicsWidget()
{
    QMutexLocker locker(&m_pixmapMutex);
    if (m_pixmapPainter) {
        delete m_pixmapPainter;
        m_pixmapPainter = nullptr;
    }
    
    if (m_graphWidget) {
        m_graphWidget->setVisible(false);
        m_graphWidget->removeEventFilter(this);
        // Force immediate processing of any pending events
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    // Clear static instance BEFORE any cleanup that might trigger callbacks
    if (s_currentInstance == this) {
        s_currentInstance = nullptr;
    }
    
    // Disable painting completely
    m_currentPainter = nullptr;
    
    // SAFETY: Clear pointers BEFORE cleanup to prevent double-free
    // The Tcl cleanup will handle the actual memory deallocation
    void* savedGbuf = m_gbuf;
    void* savedFrame = m_frame;
    m_gbuf = nullptr;
    m_frame = nullptr;
    
    // Clean up graphics buffer with original pointers
    // but don't access m_gbuf/m_frame after this
    if (savedGbuf && interpreter()) {
        cleanupGraphicsBuffer();
    }
    
    // Disconnect ALL signals to prevent any delayed signal delivery
    disconnect();
    
    // Note: The base class EssScriptableWidget destructor will handle
    // interpreter cleanup, so we don't need to do it here
}

void EssGraphicsWidget::registerCustomCommands()
{
    if (!interpreter()) return;
    
    // Load the qtcgraph package which provides all the bridge commands
    const char* packageScript = R"tcl(
       # Load graphics-specific package (core packages already loaded)
        if {[catch {package require qtcgraph} err]} {
            local_log "Warning: Could not load qtcgraph package: $err"
        } else {
            local_log "qtcgraph package loaded successfully"
        }
    )tcl";
    
    if (Tcl_Eval(interpreter(), packageScript) != TCL_OK) {
        localLog(QString("Warning: Failed to load packages: %1").arg(result()));
    }
    
    // Register enhanced graphics commands (these work with the bridge commands)
    Tcl_CreateObjCommand(interpreter(), "graphics_init", tcl_graphics_init, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "graphics_clear", tcl_graphics_clear, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "graphics_export", tcl_graphics_export, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "graphics_bind", tcl_graphics_bind, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "graphics_setcolor", tcl_graphics_setcolor, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "graphics_colorlist", tcl_graphics_colorlist, this, nullptr);
    
    // Layout control commands (stubs for future enhancement)
    Tcl_CreateObjCommand(interpreter(), "graphics_layout", tcl_graphics_layout, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "graphics_controls", tcl_graphics_controls, this, nullptr);
    
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
        
        # Enhanced setcolor that supports names
        rename setcolor _original_setcolor
        proc setcolor {color} { graphics_setcolor $color }
        
        # Convenience binding command
        proc cgbind {event script} { graphics_bind $event $script }
        
        # Window refresh
        proc refresh {} { 
            local_log "Refreshing graphics"
            # The actual refresh is handled by the Qt widget
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
        m_graphWidget->setMinimumSize(200, 150);  // Set reasonable minimum
        m_graphWidget->setAttribute(Qt::WA_OpaquePaintEvent);
        m_graphWidget->setAutoFillBackground(true);
        m_graphWidget->setFocusPolicy(Qt::ClickFocus);
        
        // Install event filter on the graph widget to handle paint/mouse events
        m_graphWidget->installEventFilter(this);
        
        // Set background
        QPalette pal = m_graphWidget->palette();
        pal.setColor(QPalette::Window, m_backgroundColor);
        m_graphWidget->setPalette(pal);
        
        // Install resize event monitoring
        m_graphWidget->installEventFilter(this);
    }
    
    switch (m_layoutMode) {
        case GraphicsOnly:
        case WithToolbar:
        default:
            // Current behavior - just return the graphics widget
            return m_graphWidget;
            
        case SideControls:
            // FUTURE: Will create splitter with side controls
            // For now, just return graphics widget
            localLog("Side controls layout not yet implemented, using graphics only");
            return m_graphWidget;
            
        case BottomControls:
            // FUTURE: Will create splitter with bottom controls  
            // For now, just return graphics widget
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


void EssGraphicsWidget::onSetupComplete()
{
//    localLog("Graphics widget setup completed");
    
    // If widget is ready and we have a pending init request, do it now
    if (m_widgetFullyConstructed && m_pendingGraphicsInit) {
        initializeGraphics();
        m_pendingGraphicsInit = false;
    }
}

bool EssGraphicsWidget::eventFilter(QObject* obj, QEvent* event)
{
   if (obj == m_graphWidget) {
		if (event->type() == QEvent::Paint) {
			// Just blit the pixmap to the widget
			QPainter widgetPainter(m_graphWidget);
			
			QMutexLocker locker(&m_pixmapMutex);
			if (!m_pixmap.isNull()) {
				widgetPainter.drawPixmap(0, 0, m_pixmap);
			} else {
				// Fallback: fill with background color
				widgetPainter.fillRect(m_graphWidget->rect(), m_backgroundColor);
			}
			
			emit graphUpdated();
			return true;
		}
		else if (event->type() == QEvent::Resize) {
			QResizeEvent* resizeEvent = static_cast<QResizeEvent*>(event);
			QSize newSize = resizeEvent->size();
			QSize oldSize = resizeEvent->oldSize();
			
			if (newSize != oldSize && newSize.width() > 0 && newSize.height() > 0) {
				// Ensure pixmap matches new size
				ensurePixmapSize();
				
				// Update cgraph dimensions
				if (m_graphicsInitialized && interpreter()) {
					QString resizeCmd = QString("qtcgraph_resize %1 %2 %3")
										.arg((quintptr)this)
										.arg(newSize.width())
										.arg(newSize.height());
					Tcl_Eval(interpreter(), resizeCmd.toUtf8().constData());
					
					// Invalidate and re-render
					invalidatePixmap();
					QTimer::singleShot(10, this, &EssGraphicsWidget::renderToPixmap);
				}
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

// Pixmap support
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
        // Clean up old painter
        if (m_pixmapPainter) {
            delete m_pixmapPainter;
            m_pixmapPainter = nullptr;
        }
        
        // Create new pixmap with device pixel ratio support
        m_pixmap = QPixmap(currentSize * devicePixelRatio());
        m_pixmap.setDevicePixelRatio(devicePixelRatio());
        m_pixmap.fill(m_backgroundColor);
        
        // Create new painter for the pixmap
        m_pixmapPainter = new QPainter(&m_pixmap);
        m_pixmapPainter->setRenderHint(QPainter::Antialiasing, true);
        m_pixmapPainter->setPen(m_currentColor);
        m_pixmapPainter->setBrush(m_currentColor);
        
        m_pixmapValid = false;  // Need to redraw
        
       // localLog(QString("Pixmap resized to %1x%2").arg(currentSize.width()).arg(currentSize.height()));
    }
}

void EssGraphicsWidget::invalidatePixmap()
{
    QMutexLocker locker(&m_pixmapMutex);
    m_pixmapValid = false;
}

void EssGraphicsWidget::renderToPixmap()
{
    if (!interpreter() || !m_gbuf || !m_pixmapPainter) {
        return;
    }
    
    QMutexLocker locker(&m_pixmapMutex);
    
    if (m_pixmapValid) {
        return;  // Already up to date
    }
    
    // Clear the pixmap
    m_pixmapPainter->fillRect(m_pixmap.rect(), m_backgroundColor);
    
    // Set up painter state
    m_pixmapPainter->setPen(m_currentColor);
    m_pixmapPainter->setBrush(m_currentColor);
    
    // Set current painter and instance for callbacks
    QPainter* oldPainter = m_currentPainter;
    m_currentPainter = m_pixmapPainter;
    s_currentInstance = this;
    
    // Execute the graphics playback
    QString cmd = QString("qtcgraph_playback %1").arg((quintptr)m_gbuf);
    int result = Tcl_Eval(interpreter(), cmd.toUtf8().constData());
    
    if (result != TCL_OK) {
        localLog(QString("Graphics playback failed: %1").arg(Tcl_GetStringResult(interpreter())));
    }
    
    // Restore previous state
    m_currentPainter = oldPainter;
    s_currentInstance = nullptr;
    
    m_pixmapValid = true;
    
    // Trigger a widget update to blit the pixmap
    QMetaObject::invokeMethod(this, [this]() {
        if (m_graphWidget) {
            m_graphWidget->update();
        }
    }, Qt::QueuedConnection);
}

void EssGraphicsWidget::requestRedraw()
{
    if (m_graphicsInitialized) {
        invalidatePixmap();
        renderToPixmap();
    }
}



void EssGraphicsWidget::forceGraphicsResize()
{
    onGraphicsWidgetResized();
}

void EssGraphicsWidget::showEvent(QShowEvent* event)
{
    EssScriptableWidget::showEvent(event);
    
    // Only trigger initialization once when widget becomes visible with valid size
    if (!m_graphicsInitialized && m_widgetFullyConstructed && m_graphWidget && 
        m_graphWidget->width() > 0 && m_graphWidget->height() > 0) {
        
 //       localLog("Widget shown with valid size - triggering graphics initialization");
        emit widgetReady();
    }
}

void EssGraphicsWidget::initializeGraphics()
{
    if (m_graphicsInitialized || !interpreter()) return;
    
    // Make sure widget has a size
    if (!m_graphWidget || m_graphWidget->width() <= 0 || m_graphWidget->height() <= 0) {
        localLog("Widget not ready for graphics initialization");
        return;
    }
 
    // ONLY cleanup if we have an existing buffer
    if (m_gbuf != nullptr) {
        cleanupGraphicsBuffer();
    }
     
    // Call Tcl to initialize the graphics buffer
    QString cmd = QString("qtcgraph_init_widget %1 %2 %3")
                    .arg((quintptr)this)
                    .arg(m_graphWidget->width())
                    .arg(m_graphWidget->height());
    
    if (Tcl_Eval(interpreter(), cmd.toUtf8().constData()) != TCL_OK) {
        localLog(QString("Failed to initialize graphics: %1").arg(result()));
        return;
    }
    
    m_graphicsInitialized = true;
  
    // Force a resize to ensure cgraph has the correct dimensions
    QString resizeCmd = QString("qtcgraph_resize %1 %2 %3")
                        .arg((quintptr)this)
                        .arg(m_graphWidget->width())
                        .arg(m_graphWidget->height());
    Tcl_Eval(interpreter(), resizeCmd.toUtf8().constData());
    
    // ADD: Initialize pixmap AFTER cgraph is set up
    ensurePixmapSize();
    
    // MODIFY: Instead of direct Tcl clearwin/flushwin, use the pixmap-aware clear method
    // This ensures the pixmap gets properly initialized and rendered
    if (interpreter() && m_graphicsInitialized) {
        // Clear the cgraph buffer
        Tcl_Eval(interpreter(), "clearwin");
        // The flushwin will be handled by our clear() method which triggers pixmap rendering
        clear();  // This will invalidate pixmap and re-render
    }

    localLog("Pixmap-based graphics system initialized successfully");
}

int EssGraphicsWidget::eval(const QString& command)
{
    if (!interpreter()) {
        localLog("No interpreter available");
        return TCL_ERROR;
    }
    
	QString contextCmd = QString("qtcgraph_set_current_buffer %1").arg((quintptr)this);
	Tcl_Eval(interpreter(), contextCmd.toUtf8().constData());
    
    // Call base class eval() to do the work
    return EssScriptableWidget::eval(command);
}

void EssGraphicsWidget::cleanupGraphicsBuffer()
{
    // Clear static instance FIRST if it points to us
    if (s_currentInstance == this) {
        s_currentInstance = nullptr;
    }
    
    // Early exit if already cleaned up
    if (m_gbuf == nullptr) {
        m_frame = nullptr;
        return;
    }
    
    // Don't try to clean up if interpreter is gone or invalid
    if (!interpreter()) {
        // Just clear our local pointers if interpreter is gone
        // The Tcl cleanup will handle the actual memory deallocation
        m_gbuf = nullptr;
        m_frame = nullptr;
        return;
    }
    
    // Try the cleanup command, but handle failure gracefully
    QString cmd = QString("qtcgraph_cleanup %1").arg((quintptr)this);
    int result = Tcl_Eval(interpreter(), cmd.toUtf8().constData());
    
    if (result != TCL_OK) {
        qDebug() << "Graphics cleanup failed (this may be normal during shutdown):" 
                 << Tcl_GetStringResult(interpreter());
    }
         
    // Always clear our pointers regardless of Tcl result
    // Let the Tcl interpreter cleanup handle the actual memory deallocation
    m_gbuf = nullptr;
    m_frame = nullptr;
}

void EssGraphicsWidget::refresh()
{
    // Instead of directly updating widget, re-render to pixmap
    if (m_graphicsInitialized) {
        invalidatePixmap();
        renderToPixmap();
    } else if (m_graphWidget) {
        m_graphWidget->update();  // Fallback for non-initialized state
    }
}


void EssGraphicsWidget::clear()
{
    if (!interpreter() || !m_gbuf) return;
    
    QString cmd = QString("qtcgraph_clear %1").arg((quintptr)this);
    Tcl_Eval(interpreter(), cmd.toUtf8().constData());
    
    // Clear pixmap and re-render
    invalidatePixmap();
    renderToPixmap();
}

void EssGraphicsWidget::setBackgroundColor(const QColor& color)
{
    m_backgroundColor = color;
    
    // Update the palette
    if (m_graphWidget) {
        QPalette pal = m_graphWidget->palette();
        pal.setColor(QPalette::Window, m_backgroundColor);
        m_graphWidget->setPalette(pal);
    }
    
    // Clear pixmap and trigger re-render
    invalidatePixmap();
    if (m_graphicsInitialized) {
        renderToPixmap();
    }
}

void EssGraphicsWidget::resizeEvent(QResizeEvent* event)
{
    EssScriptableWidget::resizeEvent(event);
    
    // Also handle case where widget was shown but had zero size initially
    if (!m_graphicsInitialized && m_widgetFullyConstructed && m_graphWidget && 
        m_graphWidget->width() > 0 && m_graphWidget->height() > 0) {
        
        emit widgetReady();
    }
    
    // Handle resize for already-initialized graphics
    if (m_graphicsInitialized && interpreter() && m_graphWidget) {
        // Use the actual visible size of the graphics widget
        QSize graphicsSize = m_graphWidget->size();
        
        // Only resize if we have a meaningful size
        if (graphicsSize.width() > 0 && graphicsSize.height() > 0) {
            QString cmd = QString("qtcgraph_resize %1 %2 %3")
                            .arg((quintptr)this)
                            .arg(graphicsSize.width())
                            .arg(graphicsSize.height());
            Tcl_Eval(interpreter(), cmd.toUtf8().constData());
            
            // Force a refresh after resize
            QTimer::singleShot(10, this, &EssGraphicsWidget::refresh);
        }
    }
}

void EssGraphicsWidget::onGraphicsWidgetResized()
{
    if (!m_graphicsInitialized || !interpreter() || !m_graphWidget) {
        return;
    }
    
    QSize newSize = m_graphWidget->size();
    
    // Only proceed if size is meaningful
    if (newSize.width() <= 0 || newSize.height() <= 0) {
        return;
    }
    
    // Notify cgraph system of the size change
    QString resizeCmd = QString("qtcgraph_resize %1 %2 %3")
                        .arg((quintptr)this)
                        .arg(newSize.width())
                        .arg(newSize.height());
    Tcl_Eval(interpreter(), resizeCmd.toUtf8().constData());
    
    // Force immediate refresh
    refresh();
}

void EssGraphicsWidget::onWidgetReady()
{
    // This is called via queued connection when widget is ready
    if (m_graphicsInitialized) {
        return; // Already initialized
    }
    
    // Call the Tcl initialization function
    if (interpreter()) {
        eval("graphics_init_when_ready");
        emit graphicsReady();
    } else {
        localLog("Interpreter not ready - marking pending");
        m_pendingGraphicsInit = true;
    }
}

void EssGraphicsWidget::setFloatingMode(bool floating)
{
    m_isFloating = floating;
    if (m_returnToTabsAction) {
        m_returnToTabsAction->setVisible(floating);
    }
}

EssGraphicsWidget* EssGraphicsWidget::getCurrentInstance()
{
    return s_currentInstance;
}

void EssGraphicsWidget::applyDevelopmentLayout()
{
    // Call base class implementation first
    EssScriptableWidget::applyDevelopmentLayout();
    
    // After layout change, update graphics buffer size
    // Use queued connection to ensure layout is fully applied first
    if (m_graphicsInitialized && m_graphWidget) {
        QMetaObject::invokeMethod(this, &EssGraphicsWidget::onGraphicsWidgetResized, 
                                  Qt::QueuedConnection);
    }
}

// Event handling methods (preserve QtCGraph event system)
QString EssGraphicsWidget::substituteEventData(const QString& script, QEvent* event,
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
    if (!interpreter() || !m_gbuf) {
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
    
    // FUTURE: Will rebuild layout here
    // For now, just emit signal and log
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
    
    // FUTURE: Will update control panel here
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
    
    // FUTURE: Will show/hide control panels here
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

// Static cgraph callbacks (preserve existing callback system)
int EssGraphicsWidget::Clearwin()
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter) return 0;
    
    s_currentInstance->m_currentPainter->fillRect(
        s_currentInstance->m_graphWidget->rect(),
        s_currentInstance->m_backgroundColor);
    return 0;
}

int EssGraphicsWidget::Line(float x0, float y0, float x1, float y1)
{    
    if (!s_currentInstance || !s_currentInstance->m_currentPainter) return 0;
    
    float h = s_currentInstance->m_graphWidget->height(); 
    s_currentInstance->m_currentPainter->setPen(s_currentInstance->m_currentColor);
    s_currentInstance->m_currentPainter->drawLine(QPointF(x0, h - y0), QPointF(x1, h - y1));
    return 0;
}

int EssGraphicsWidget::Point(float x, float y)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter) return 0;
    
    float h = s_currentInstance->m_graphWidget->height();
    s_currentInstance->m_currentPainter->drawPoint(QPointF(x, h - y));
    return 0;
}

int EssGraphicsWidget::Setcolor(int index)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter) return 0;
    
    // Use the color table from QtCGraph
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

int EssGraphicsWidget::Char(float x, float y, char *string)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter || !string) return 0;
    
    float h = s_currentInstance->m_graphWidget->height();
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

int EssGraphicsWidget::Text(float x, float y, char *string)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter || !string) return 0;
    
    float h = s_currentInstance->m_graphWidget->height();

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

int EssGraphicsWidget::Setfont(char *fontname, float size)
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

int EssGraphicsWidget::Strwidth(char *str)
{
    if (!s_currentInstance || !str) return 0;
    
    QFontMetrics fm(s_currentInstance->m_currentPainter ? 
                    s_currentInstance->m_currentPainter->font() : 
                    s_currentInstance->font());
    return fm.horizontalAdvance(QString::fromUtf8(str));
}

int EssGraphicsWidget::Strheight(char *str)
{
    if (!s_currentInstance || !str) return 0;
    
    QFontMetrics fm(s_currentInstance->m_currentPainter ? 
                    s_currentInstance->m_currentPainter->font() : 
                    s_currentInstance->font());
    return fm.height();
}

int EssGraphicsWidget::FilledPolygon(float *verts, int nverts)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter || !verts) return 0;
    
    float h = s_currentInstance->m_graphWidget->height();
    
    QPolygonF polygon;
    for (int i = 0; i < nverts; i++) {
        polygon << QPointF(verts[i*2], h - verts[i*2+1]);
    }
    
    s_currentInstance->m_currentPainter->drawPolygon(polygon);
    
    return 0;
}

int EssGraphicsWidget::Circle(float x, float y, float width, int filled)
{
    if (!s_currentInstance || !s_currentInstance->m_currentPainter) return 0;
    
    float h = s_currentInstance->m_graphWidget->height();

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

// Tcl command implementations
int EssGraphicsWidget::tcl_graphics_init(ClientData clientData, Tcl_Interp* interp,
                                         int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssGraphicsWidget*>(clientData);
    
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    // Safety checks
    if (!widget->m_graphWidget) {
        Tcl_SetResult(interp, "Graphics widget not ready", TCL_STATIC);
        return TCL_ERROR;
    }
    
    if (widget->m_graphicsInitialized) {
        Tcl_SetResult(interp, "Graphics already initialized", TCL_STATIC);
        return TCL_OK;  // Not an error
    }
    
    widget->initializeGraphics();
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj("graphics initialized", -1));
    return TCL_OK;
}

// End of EssGraphicsWidget implementation
// Note: EssGraphicsBridge functions are implemented in qtcgraph_tcl.cpp

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

int EssGraphicsWidget::tcl_graphics_setcolor(ClientData clientData, Tcl_Interp* interp,
                                            int objc, Tcl_Obj* const objv[])
{
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "color_index_or_name");
        return TCL_ERROR;
    }
    
    // Delegate to the qtcgraph_setcolor command (which has access to cgraph functions)
    QString colorArg = Tcl_GetString(objv[1]);
    QString cmd = QString("qtcgraph_setcolor {%1}").arg(colorArg);
    
    int result = Tcl_Eval(interp, cmd.toUtf8().constData());
    // Result is already set by qtcgraph_setcolor
    
    return result;
}

int EssGraphicsWidget::tcl_graphics_colorlist(ClientData clientData, Tcl_Interp* interp,
                                             int objc, Tcl_Obj* const objv[])
{
    // Delegate to the qtcgraph_colorlist command
    return Tcl_Eval(interp, "qtcgraph_colorlist");
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

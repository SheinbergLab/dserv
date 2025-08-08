#include "EssEyeTouchVisualizer.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTimerEvent>
#include <QtMath>

EssEyeTouchVisualizer::EssEyeTouchVisualizer(QWidget *parent)
    : EssVisualizationWidget(parent)
    , m_showTrails(false)
    , m_showGrid(true)
    , m_showLabels(true)
    , m_maxTrailPoints(50)
    , m_virtualInputEnabled(false)
    , m_virtualEyeDragging(false)
    , m_virtualEyePos(0, 0)
    , m_virtualTouchActive(false)
    , m_touchActive(false)
    , m_touchTimeoutTimerId(0)
    , m_eyeWindowStatus(0)
    , m_touchWindowStatus(0)
    , m_screenSize(800, 600)
    , m_screenHalfDegrees(10.0, 7.5)
    , m_continuousUpdateTimer(new QTimer(this))
    , m_continuousUpdateEnabled(false)
    , m_updateRate(250)  // Default 250 Hz
{
    // Set visual range to match FLTK (±10 degrees)
    setVisualRange(20.0, 20.0);
    
    // Create indicators
    m_eyeIndicator = new EssWindowIndicator();
    m_eyeIndicator->setLabel("Eye");
    m_eyeIndicator->setWindowCount(8);
    
    m_touchIndicator = new EssWindowIndicator();
    m_touchIndicator->setLabel("Touch");
    m_touchIndicator->setWindowCount(8);
    
    // Initialize windows
    m_eyeWindows.resize(8);
    m_touchWindows.resize(8);
    for (int i = 0; i < 8; ++i) {
        m_eyeWindows[i].id = i;
        m_eyeWindows[i].active = false;
        m_eyeWindows[i].type = Rectangle;
        
        m_touchWindows[i].id = i;
        m_touchWindows[i].active = false;
        m_touchWindows[i].type = Rectangle;
    }
    
    // Set up drawing layers
    setupDrawLayers();
    
    // Enable mouse tracking for virtual input
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    
    // Start timers
    m_touchClearTimer.start();
    m_virtualTouchTimer.start();
    
    // Start touch timeout timer
    m_touchTimeoutTimerId = startTimer(100);  // Check every 100ms

    m_drawingRect = rect();
    
    // Setup continuous update timer
    connect(m_continuousUpdateTimer, &QTimer::timeout, 
            this, &EssEyeTouchVisualizer::sendContinuousUpdate);
}

EssEyeTouchVisualizer::~EssEyeTouchVisualizer() {
    if (m_touchTimeoutTimerId) {
        killTimer(m_touchTimeoutTimerId);
    }
}

// Override drawBackground to maintain aspect ratio
void EssEyeTouchVisualizer::drawBackground(QPainter& painter) {
    // Fill entire widget black
    painter.fillRect(rect(), Qt::black);
    
    // Calculate square drawing area
    int size = qMin(width(), height());
    m_drawingRect = QRect((width() - size) / 2, (height() - size) / 2, size, size);
    
    // Optional: draw border around active area
    painter.setPen(QPen(QColor(40, 40, 40), 1));
    painter.drawRect(m_drawingRect);
}

// Update coordinate transformations to use the square area
QPointF EssEyeTouchVisualizer::degreesToCanvas(const QPointF& degrees) const {
    QPointF center(m_drawingRect.center());
    double pixPerDeg = m_drawingRect.width() / visualRange().width();
    
    return QPointF(
        center.x() + (degrees.x() * pixPerDeg),
        center.y() - (degrees.y() * pixPerDeg)  // Y inverted
    );
}

QPointF EssEyeTouchVisualizer::canvasToDegrees(const QPointF& canvasPos) const {
    QPointF center(m_drawingRect.center());
    double pixPerDeg = m_drawingRect.width() / visualRange().width();
    
    return QPointF(
        (canvasPos.x() - center.x()) / pixPerDeg,
        -1.0 * (canvasPos.y() - center.y()) / pixPerDeg  // Y inverted
    );
}

void EssEyeTouchVisualizer::setupDrawLayers() {
    // Grid layer (z=0)
    addDrawLayer("grid", [this](QPainter& p) { drawGrid(p); }, 0);
    
    // Trails layer (z=10)
    addDrawLayer("trails", [this](QPainter& p) { drawEyeTrails(p); }, 10);
    
    // Window layers (z=20, 25)
    addDrawLayer("eyeWindows", [this](QPainter& p) { drawEyeWindows(p); }, 20);
    addDrawLayer("touchWindows", [this](QPainter& p) { drawTouchWindows(p); }, 25);
    
    // Position layers (z=30, 35, 40, 45)
    addDrawLayer("eyePosition", [this](QPainter& p) { drawEyePosition(p); }, 30);
    addDrawLayer("touchPosition", [this](QPainter& p) { drawTouchPosition(p); }, 35);
    addDrawLayer("virtualEye", [this](QPainter& p) { drawVirtualEye(p); }, 40);
    addDrawLayer("virtualTouch", [this](QPainter& p) { drawVirtualTouch(p); }, 45);
}

void EssEyeTouchVisualizer::setShowTrails(bool show) {
    m_showTrails = show;
    setLayerVisible("trails", show);
    if (!show) {
        clearTrails();
    }
}

void EssEyeTouchVisualizer::setShowGrid(bool show) {
    m_showGrid = show;
    setLayerVisible("grid", show);
}

void EssEyeTouchVisualizer::setShowLabels(bool show) {
    m_showLabels = show;
    scheduleUpdate();
}

void EssEyeTouchVisualizer::setTrailLength(int maxPoints) {
    m_maxTrailPoints = qMax(1, maxPoints);
    while (m_trailPoints.size() > m_maxTrailPoints) {
        m_trailPoints.dequeue();
    }
}

void EssEyeTouchVisualizer::setVirtualInputEnabled(bool enabled) {
    m_virtualInputEnabled = enabled;
    setLayerVisible("virtualEye", enabled);
    setLayerVisible("virtualTouch", enabled);
    
    if (!enabled) {
        m_virtualEyeDragging = false;
        m_virtualTouchActive = false;
        m_continuousUpdateTimer->stop();  // Stop continuous updates
    } else {
        // When enabling, emit current virtual position
        QPoint adc = degreesToAdc(m_virtualEyePos);
        emit virtualEyePosition(adc.x(), adc.y());
        
        // Start continuous updates if they were enabled
        if (m_continuousUpdateEnabled) {
            setContinuousUpdateEnabled(true);
        }
    }
}

void EssEyeTouchVisualizer::setContinuousUpdateEnabled(bool enabled) {
    m_continuousUpdateEnabled = enabled;
    
    if (enabled && m_virtualInputEnabled) {
        m_continuousUpdateTimer->start(1000 / m_updateRate);
    } else {
        m_continuousUpdateTimer->stop();
    }
}

void EssEyeTouchVisualizer::setUpdateRate(int hz) {
    m_updateRate = qBound(1, hz, 1000);  // Limit to 1-1000 Hz
    
    if (m_continuousUpdateTimer->isActive()) {
        m_continuousUpdateTimer->setInterval(1000 / m_updateRate);
    }
}

void EssEyeTouchVisualizer::sendContinuousUpdate() {
    if (m_virtualInputEnabled && m_continuousUpdateEnabled) {
        // Emit current position
        QPoint adc = degreesToAdc(m_virtualEyePos);
        emit virtualEyePosition(adc.x(), adc.y());
    }
}

void EssEyeTouchVisualizer::setVirtualEyePosition(float degX, float degY) {
    QPointF newPos(degX, degY);
    
    if (newPos != m_virtualEyePos) {
        m_virtualEyePos = newPos;
        
        if (m_virtualInputEnabled) {
            QPoint adc = degreesToAdc(m_virtualEyePos);
            emit virtualEyePosition(adc.x(), adc.y());
        }
        
        scheduleUpdate();
    }
}

void EssEyeTouchVisualizer::updateEyePosition(int adcX, int adcY) {
    m_eyePositionRaw = QPointF(adcX, adcY);
    m_eyePosition = adcToDegrees(adcX, adcY);
    
    // If virtual input is enabled but not dragging, follow real eye
    if (m_virtualInputEnabled && !m_virtualEyeDragging) {
        // Update virtual position to match real position
        if (m_virtualEyePos != m_eyePosition) {
            m_virtualEyePos = m_eyePosition;
            // Don't emit here - only emit when dragging
        }
    }
    
    if (m_showTrails) {
        addTrailPoint(m_eyePosition);
    }
    
    emit eyePositionChanged(m_eyePosition.x(), m_eyePosition.y());
    scheduleUpdate();
}

void EssEyeTouchVisualizer::updateTouchPosition(int screenX, int screenY) {
    m_touchPositionRaw = QPoint(screenX, screenY);
    m_touchPosition = touchPixelsToDegrees(screenX, screenY);
    m_touchActive = true;
    m_touchClearTimer.restart();
    
    emit touchPositionChanged(m_touchPosition.x(), m_touchPosition.y());
    scheduleUpdate();
}

void EssEyeTouchVisualizer::updateEyeWindows(const QVector<Window>& windows) {
    m_eyeWindows = windows;
    scheduleUpdate();
}

void EssEyeTouchVisualizer::updateTouchWindows(const QVector<Window>& windows) {
    m_touchWindows = windows;
    
    // Convert raw coordinates to degrees
    for (auto& window : m_touchWindows) {
        window.center = touchPixelsToDegrees(window.centerRaw.x(), window.centerRaw.y());
        window.size = QSizeF(
            window.sizeRaw.width() / (m_screenSize.width() / (2 * m_screenHalfDegrees.x())),
            window.sizeRaw.height() / (m_screenSize.height() / (2 * m_screenHalfDegrees.y()))
        );
    }
    
    scheduleUpdate();
}

void EssEyeTouchVisualizer::updateEyeWindowStatus(uint8_t statusMask) {
    m_eyeWindowStatus = statusMask;
    
    // Update indicators
    for (int i = 0; i < 8; ++i) {
        bool inside = (statusMask & (1 << i)) != 0;
        m_eyeIndicator->setWindowStatus(i, m_eyeWindows[i].active, inside);
    }
    
    scheduleUpdate();
}

void EssEyeTouchVisualizer::updateTouchWindowStatus(uint8_t statusMask) {
    m_touchWindowStatus = statusMask;
    
    // Update indicators
    for (int i = 0; i < 8; ++i) {
        bool inside = (statusMask & (1 << i)) != 0;
        m_touchIndicator->setWindowStatus(i, m_touchWindows[i].active, inside);
    }
    
    scheduleUpdate();
}

void EssEyeTouchVisualizer::updateScreenDimensions(int width, int height, 
                                                  double halfX, double halfY) {
    m_screenSize = QSize(width, height);
    m_screenHalfDegrees = QPointF(halfX, halfY);
    
    // Recalculate touch window positions if any exist
    for (auto& window : m_touchWindows) {
        window.center = touchPixelsToDegrees(window.centerRaw.x(), window.centerRaw.y());
        window.size = QSizeF(
            window.sizeRaw.width() / (m_screenSize.width() / (2 * m_screenHalfDegrees.x())),
            window.sizeRaw.height() / (m_screenSize.height() / (2 * m_screenHalfDegrees.y()))
        );
    }
    
    scheduleUpdate();
}

void EssEyeTouchVisualizer::clearTrails() {
    m_trailPoints.clear();
    scheduleUpdate();
}

void EssEyeTouchVisualizer::clearTouchPosition() {
    m_touchActive = false;
    scheduleUpdate();
}

void EssEyeTouchVisualizer::resetVirtualInput() {
    m_virtualEyePos = QPointF(0, 0);
    m_virtualTouchActive = false;
    
    if (m_virtualInputEnabled) {
        QPoint adc = degreesToAdc(m_virtualEyePos);
        emit virtualEyePosition(adc.x(), adc.y());
    }
    
    scheduleUpdate();
}

void EssEyeTouchVisualizer::timerEvent(QTimerEvent *event) {
    if (event->timerId() == m_touchTimeoutTimerId) {
        // Check if touch should be cleared
        if (m_touchActive && m_touchClearTimer.elapsed() > 500) {
            clearTouchPosition();
        }
    }
    EssVisualizationWidget::timerEvent(event);
}

// Drawing implementations
void EssEyeTouchVisualizer::drawGrid(QPainter& painter) {
    painter.setPen(QPen(QColor(51, 51, 51), 1));
    
    // Draw vertical lines
    for (int deg = -10; deg <= 10; deg += 5) {
        QPointF start = degreesToCanvas(QPointF(deg, -10));
        QPointF end = degreesToCanvas(QPointF(deg, 10));
        painter.drawLine(start, end);
    }
    
    // Draw horizontal lines
    for (int deg = -10; deg <= 10; deg += 5) {
        QPointF start = degreesToCanvas(QPointF(-10, deg));
        QPointF end = degreesToCanvas(QPointF(10, deg));
        painter.drawLine(start, end);
    }
    
    // Center crosshair
    painter.setPen(QPen(QColor(102, 102, 102), 2));
    QPointF center = canvasCenter();
    painter.drawLine(center.x() - 10, center.y(), center.x() + 10, center.y());
    painter.drawLine(center.x(), center.y() - 10, center.x(), center.y() + 10);
}

void EssEyeTouchVisualizer::drawEyeWindows(QPainter& painter) {
    for (const auto& window : m_eyeWindows) {
        if (window.active) {
            bool isInside = (m_eyeWindowStatus & (1 << window.id)) != 0;
            drawWindow(painter, window, isInside ? QColor(0, 255, 0) : QColor(255, 0, 0), isInside);
        }
    }
}

void EssEyeTouchVisualizer::drawTouchWindows(QPainter& painter) {
    for (const auto& window : m_touchWindows) {
        if (window.active) {
            bool isInside = (m_touchWindowStatus & (1 << window.id)) != 0;
            drawWindow(painter, window, isInside ? QColor(0, 255, 255) : QColor(0, 136, 170), isInside);
        }
    }
}

// In EssEyeTouchVisualizer.cpp, update drawWindow:
void EssEyeTouchVisualizer::drawWindow(QPainter& painter, const Window& window, 
                                      const QColor& color, bool isInside) {
    QPointF pos = degreesToCanvas(window.center);
    
    // Calculate square pixel-per-degree from our drawing rect
    double pixPerDeg = m_drawingRect.width() / visualRange().width();
    QSizeF size(window.size.width() * pixPerDeg * 2,
                window.size.height() * pixPerDeg * 2);
    
    painter.setPen(QPen(color, isInside ? 3 : 1));
    
    if (window.type == Ellipse) {
        painter.drawEllipse(pos, size.width() / 2, size.height() / 2);
    } else {
        painter.drawRect(QRectF(pos.x() - size.width() / 2,
                               pos.y() - size.height() / 2,
                               size.width(), size.height()));
    }
    
    // Draw center point
    painter.fillRect(QRectF(pos.x() - 2, pos.y() - 2, 4, 4), color);
    
    // Draw label
    if (m_showLabels) {
		QFont font;
		font.setStyleHint(QFont::Monospace);
		font.setPointSize(10);    
        painter.setFont(font);
        QString label = QString("%1%2")
                       .arg(window.id < m_eyeWindows.size() ? "E" : "T")
                       .arg(window.id);
        painter.drawText(pos.x() - size.width() / 2 + 2,
                        pos.y() - size.height() / 2 - 5, label);
    }
}

void EssEyeTouchVisualizer::drawEyePosition(QPainter& painter) {
    if (m_virtualInputEnabled) return;  // Don't draw real eye when virtual is enabled
    
    QPointF pos = degreesToCanvas(m_eyePosition);
    
    // White circle with red outline
    painter.setBrush(Qt::white);
    painter.setPen(QPen(Qt::red, 2));
    painter.drawEllipse(pos, 5, 5);
    
    // Crosshair
    painter.setPen(QPen(Qt::white, 1));
    painter.drawLine(pos.x() - 10, pos.y(), pos.x() + 10, pos.y());
    painter.drawLine(pos.x(), pos.y() - 10, pos.x(), pos.y() + 10);
}

void EssEyeTouchVisualizer::drawTouchPosition(QPainter& painter) {
    if (!m_touchActive) return;
    
    QPointF pos = degreesToCanvas(m_touchPosition);
    
    // Diamond shape
    painter.setBrush(QColor(0, 255, 255));
    painter.setPen(QPen(QColor(0, 136, 170), 2));
    
    QPolygonF diamond;
    diamond << QPointF(pos.x(), pos.y() - 7)
            << QPointF(pos.x() + 7, pos.y())
            << QPointF(pos.x(), pos.y() + 7)
            << QPointF(pos.x() - 7, pos.y());
    painter.drawPolygon(diamond);
}

void EssEyeTouchVisualizer::drawEyeTrails(QPainter& painter) {
    if (m_trailPoints.size() < 2) return;
    
    painter.setPen(QPen(QColor(255, 0, 0, 128), 2));
    
    QPainterPath path;
    bool first = true;
    
    for (const auto& point : m_trailPoints) {
        QPointF pos = degreesToCanvas(point.position);
        if (first) {
            path.moveTo(pos);
            first = false;
        } else {
            path.lineTo(pos);
        }
    }
    
    painter.drawPath(path);
}

void EssEyeTouchVisualizer::drawVirtualEye(QPainter& painter) {
    if (!m_virtualInputEnabled) return;
    
    QPointF pos = degreesToCanvas(m_virtualEyePos);
    
    // Orange circle for virtual
    painter.setBrush(Qt::white);
    painter.setPen(QPen(m_virtualEyeDragging ? QColor(0, 255, 0) : QColor(255, 140, 0), 2));
    painter.drawEllipse(pos, 8, 8);
    
    // Crosshair
    painter.setPen(Qt::black);
    painter.drawLine(pos.x() - 6, pos.y(), pos.x() + 6, pos.y());
    painter.drawLine(pos.x(), pos.y() - 6, pos.x(), pos.y() + 6);
    
    // "V" indicator
	QFont font;
	font.setStyleHint(QFont::Monospace);
	font.setPointSize(10);    
	font.setWeight(QFont::Bold);
	painter.setFont(font);

    painter.setPen(QColor(255, 140, 0));
    painter.drawText(pos.x() - 3, pos.y() - 12, "V");
}

void EssEyeTouchVisualizer::drawVirtualTouch(QPainter& painter) {
    if (!m_virtualInputEnabled || !m_virtualTouchActive) return;
    
    // Clear after 200ms
    if (m_virtualTouchTimer.elapsed() > 200) {
        m_virtualTouchActive = false;
        return;
    }
    
    QPointF pos = degreesToCanvas(m_virtualTouchPos);
    
    // Orange diamond for virtual
    painter.setBrush(QColor(255, 140, 0));
    painter.setPen(QPen(QColor(204, 102, 0), 2));
    
    QPolygonF diamond;
    diamond << QPointF(pos.x(), pos.y() - 9)
            << QPointF(pos.x() + 9, pos.y())
            << QPointF(pos.x(), pos.y() + 9)
            << QPointF(pos.x() - 9, pos.y());
    painter.drawPolygon(diamond);
    
    // "V" indicator
	QFont font;
	font.setStyleHint(QFont::Monospace);
	font.setPointSize(8);    
	font.setWeight(QFont::Bold);
	painter.setFont(font);    
	painter.setPen(Qt::white);
    painter.drawText(pos.x() - 2, pos.y() + 2, "V");
}

// Mouse event handling
void EssEyeTouchVisualizer::mousePressEvent(QMouseEvent *event) {
    if (!m_virtualInputEnabled) {
        EssVisualizationWidget::mousePressEvent(event);
        return;
    }
    
    QPointF canvasPos(event->pos());
    QPointF degrees = canvasToDegrees(canvasPos);
    
    // Check if clicking on virtual eye marker
    QPointF eyePos = degreesToCanvas(m_virtualEyePos);
    double distance = QLineF(canvasPos, eyePos).length();
    
    if (distance <= 13) {  // 8px radius + 5px tolerance
        m_virtualEyeDragging = true;
        m_dragOffset = eyePos - canvasPos;
        event->accept();
        return;
    }
    
    // Otherwise, it's a touch event
    m_virtualTouchPos = degrees;
    m_virtualTouchActive = true;
    m_virtualTouchTimer.restart();
    
    // Emit virtual touch
    QPoint touchPixels = degreesToTouchPixels(degrees);
    emit virtualTouchEvent(touchPixels.x(), touchPixels.y());
    
    scheduleUpdate();
    event->accept();
}

void EssEyeTouchVisualizer::mouseMoveEvent(QMouseEvent *event) {
    if (!m_virtualInputEnabled || !m_virtualEyeDragging) {
        EssVisualizationWidget::mouseMoveEvent(event);
        return;
    }
    
    QPointF canvasPos = event->pos() + m_dragOffset;
    
    QPointF degrees = canvasToDegrees(canvasPos);
    
    // Constrain to visual range
    double maxX = visualRange().width() / 2;
    double maxY = visualRange().height() / 2;
    degrees.setX(qBound(-maxX, degrees.x(), maxX));
    degrees.setY(qBound(-maxY, degrees.y(), maxY));
    
    // Only update if position actually changed (with small tolerance)
    QPointF diff = degrees - m_virtualEyePos;
    if (qAbs(diff.x()) > 0.01 || qAbs(diff.y()) > 0.01) {
        m_virtualEyePos = degrees;
        
        // Emit virtual eye position
        QPoint adc = degreesToAdc(degrees);
        emit virtualEyePosition(adc.x(), adc.y());
        
        scheduleUpdate();
    }
    
    event->accept();
}

void EssEyeTouchVisualizer::mouseReleaseEvent(QMouseEvent *event) {
    if (m_virtualEyeDragging) {
        m_virtualEyeDragging = false;
        scheduleUpdate();
        event->accept();
    } else {
        EssVisualizationWidget::mouseReleaseEvent(event);
    }
}

void EssEyeTouchVisualizer::keyPressEvent(QKeyEvent *event) {
    if (m_virtualInputEnabled && event->key() == Qt::Key_R) {
        resetVirtualInput();
        event->accept();
    } else {
        EssVisualizationWidget::keyPressEvent(event);
    }
}

// Coordinate conversions matching FLTK exactly
QPointF EssEyeTouchVisualizer::adcToDegrees(int adcX, int adcY) const {
    return QPointF(
        (adcX - ADC_CENTER) * DEG_PER_ADC,
        -1.0 * (adcY - ADC_CENTER) * DEG_PER_ADC  // Y inverted
    );
}

QPoint EssEyeTouchVisualizer::degreesToAdc(const QPointF& degrees) const {
    return QPoint(
        qRound(degrees.x() * ADC_TO_DEG + ADC_CENTER),
        qRound(-degrees.y() * ADC_TO_DEG + ADC_CENTER)  // Y inverted
    );
}

QPointF EssEyeTouchVisualizer::touchPixelsToDegrees(int pixX, int pixY) const {
    double screenPixPerDegX = m_screenSize.width() / (2 * m_screenHalfDegrees.x());
    double screenPixPerDegY = m_screenSize.height() / (2 * m_screenHalfDegrees.y());
    
    return QPointF(
        (pixX - m_screenSize.width() / 2.0) / screenPixPerDegX,
        -1.0 * (pixY - m_screenSize.height() / 2.0) / screenPixPerDegY
    );
}

QPoint EssEyeTouchVisualizer::degreesToTouchPixels(const QPointF& degrees) const {
    double screenPixPerDegX = m_screenSize.width() / (2 * m_screenHalfDegrees.x());
    double screenPixPerDegY = m_screenSize.height() / (2 * m_screenHalfDegrees.y());
    
    return QPoint(
        qRound(degrees.x() * screenPixPerDegX + m_screenSize.width() / 2),
        qRound(-degrees.y() * screenPixPerDegY + m_screenSize.height() / 2)
    );
}

void EssEyeTouchVisualizer::addTrailPoint(const QPointF& point) {
    TrailPoint tp;
    tp.position = point;
    tp.timestamp = m_frameTimer.elapsed();  // Use the instance's elapsed time
    
    m_trailPoints.enqueue(tp);
    
    while (m_trailPoints.size() > m_maxTrailPoints) {
        m_trailPoints.dequeue();
    }
}
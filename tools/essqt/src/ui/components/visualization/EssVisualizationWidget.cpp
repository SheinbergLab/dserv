 #include "EssVisualizationWidget.h"
#include <QPaintEvent>
#include <algorithm>

EssVisualizationWidget::EssVisualizationWidget(QWidget *parent)
    : QWidget(parent)
    , m_updateTimer(new QTimer(this))
    , m_targetFPS(60)
    , m_actualFPS(0.0)
    , m_frameCount(0)
    , m_lastFPSUpdate(0)
    , m_updatesPaused(false)
    , m_visualRange(20.0, 20.0)  // ±10 degrees default
    , m_layersNeedSort(false)
{
    // Set widget attributes for better performance
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    
    // Setup update timer
    connect(m_updateTimer, &QTimer::timeout, this, [this]() {
        if (!m_updatesPaused) {
            update();
        }
    });
    setTargetFPS(60);
    
    // Start frame timer
    m_frameTimer.start();
}

EssVisualizationWidget::~EssVisualizationWidget() = default;

void EssVisualizationWidget::setTargetFPS(int fps) {
    m_targetFPS = qBound(1, fps, 120);
    if (m_updateTimer->isActive()) {
        m_updateTimer->setInterval(1000 / m_targetFPS);
    }
}

void EssVisualizationWidget::setVisualRange(double horizontalDegrees, double verticalDegrees) {
    m_visualRange = QSizeF(horizontalDegrees, verticalDegrees);
    updateTransforms();
    scheduleUpdate();
}

void EssVisualizationWidget::addDrawLayer(const QString& name, 
                                         std::function<void(QPainter&)> drawFunc, 
                                         int zOrder) {
    // Remove existing layer with same name
    removeDrawLayer(name);
    
    DrawLayer layer;
    layer.name = name;
    layer.drawFunc = drawFunc;
    layer.zOrder = zOrder;
    layer.visible = true;
    
    m_layers.append(layer);
    m_layersNeedSort = true;
    scheduleUpdate();
}

void EssVisualizationWidget::removeDrawLayer(const QString& name) {
    auto it = std::remove_if(m_layers.begin(), m_layers.end(),
                            [&name](const DrawLayer& layer) {
                                return layer.name == name;
                            });
    if (it != m_layers.end()) {
        m_layers.erase(it, m_layers.end());
        scheduleUpdate();
    }
}

void EssVisualizationWidget::setLayerVisible(const QString& name, bool visible) {
    for (auto& layer : m_layers) {
        if (layer.name == name) {
            layer.visible = visible;
            scheduleUpdate();
            break;
        }
    }
}

void EssVisualizationWidget::setStimulusRenderer(std::function<void(QPainter&, const QRectF&)> renderer) {
    m_stimulusRenderer = renderer;
    scheduleUpdate();
}

void EssVisualizationWidget::clearStimulusRenderer() {
    m_stimulusRenderer = nullptr;
    scheduleUpdate();
}

QPointF EssVisualizationWidget::degreesToCanvas(const QPointF& degrees) const {
    return QPointF(
        canvasCenter().x() + (degrees.x() * m_pixelsPerDegree.x()),
        canvasCenter().y() - (degrees.y() * m_pixelsPerDegree.y())  // Y inverted
    );
}

QPointF EssVisualizationWidget::canvasToDegrees(const QPointF& canvasPos) const {
    return QPointF(
        (canvasPos.x() - canvasCenter().x()) / m_pixelsPerDegree.x(),
        -1.0 * (canvasPos.y() - canvasCenter().y()) / m_pixelsPerDegree.y()  // Y inverted
    );
}

QPointF EssVisualizationWidget::normalizedToCanvas(const QPointF& normalized) const {
    return QPointF(
        normalized.x() * width(),
        normalized.y() * height()
    );
}

QPointF EssVisualizationWidget::canvasToNormalized(const QPointF& canvasPos) const {
    return QPointF(
        canvasPos.x() / width(),
        canvasPos.y() / height()
    );
}

void EssVisualizationWidget::paintEvent(QPaintEvent *event) {
    if (m_updatesPaused) return;
    
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    
    // Draw background
    drawBackground(painter);
    
    // Draw stimulus underlay if provided
    if (m_stimulusRenderer) {
        painter.save();
        QRectF canvasRect(0, 0, width(), height());
        m_stimulusRenderer(painter, canvasRect);
        painter.restore();
    }
    
    // Sort layers if needed
    if (m_layersNeedSort) {
        std::sort(m_layers.begin(), m_layers.end(),
                 [](const DrawLayer& a, const DrawLayer& b) {
                     return a.zOrder < b.zOrder;
                 });
        m_layersNeedSort = false;
    }
    
    // Draw all visible layers
    for (const auto& layer : m_layers) {
        if (layer.visible && layer.drawFunc) {
            painter.save();
            layer.drawFunc(painter);
            painter.restore();
        }
    }
    
    // Update metrics
    updateMetrics();
}

void EssVisualizationWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    updateTransforms();
    emit canvasResized(size());
}

void EssVisualizationWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    m_updateTimer->start(1000 / m_targetFPS);
    m_frameTimer.restart();
}

void EssVisualizationWidget::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    m_updateTimer->stop();
}

void EssVisualizationWidget::drawBackground(QPainter& painter) {
    painter.fillRect(rect(), Qt::black);
}

void EssVisualizationWidget::scheduleUpdate() {
    if (!m_updatesPaused) {
        if (!m_updateTimer->isActive() && isVisible()) {
            m_updateTimer->start(1000 / m_targetFPS);
        }
        update();
    }
}

void EssVisualizationWidget::updateTransforms() {
    if (width() > 0 && height() > 0) {
        m_pixelsPerDegree = QPointF(
            width() / m_visualRange.width(),
            height() / m_visualRange.height()
        );
    }
}

void EssVisualizationWidget::updateMetrics() {
    m_frameCount++;
    qint64 now = m_frameTimer.elapsed();
    
    if (now - m_lastFPSUpdate >= 1000) {
        m_actualFPS = m_frameCount * 1000.0 / (now - m_lastFPSUpdate);
        m_frameCount = 0;
        m_lastFPSUpdate = now;
        emit frameRendered(m_actualFPS);
    }
}
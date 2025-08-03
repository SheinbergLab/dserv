#pragma once
#include <QWidget>
#include <QPointF>
#include <QTimer>
#include <QPainter>
#include <QElapsedTimer>
#include <functional>
#include <memory>

class EssVisualizationWidget : public QWidget {
    Q_OBJECT
    
public:
    explicit EssVisualizationWidget(QWidget *parent = nullptr);
    virtual ~EssVisualizationWidget();
    
    // Frame rate control
    void setTargetFPS(int fps);
    int targetFPS() const { return m_targetFPS; }
    double actualFPS() const { return m_actualFPS; }
    
    // Coordinate system
    void setVisualRange(double horizontalDegrees, double verticalDegrees);
    QSizeF visualRange() const { return m_visualRange; }
    
    // Layer system for drawing order
    void addDrawLayer(const QString& name, std::function<void(QPainter&)> drawFunc, int zOrder = 0);
    void removeDrawLayer(const QString& name);
    void setLayerVisible(const QString& name, bool visible);
    
    // Coordinate transformations
    QPointF degreesToCanvas(const QPointF& degrees) const;
    QPointF canvasToDegrees(const QPointF& canvasPos) const;
    QPointF normalizedToCanvas(const QPointF& normalized) const;
    QPointF canvasToNormalized(const QPointF& canvasPos) const;
    
    // Stimulus underlay support
    void setStimulusRenderer(std::function<void(QPainter&, const QRectF&)> renderer);
    void clearStimulusRenderer();
    
    // Update control
    void pauseUpdates() { m_updatesPaused = true; }
    void resumeUpdates() { m_updatesPaused = false; scheduleUpdate(); }
    bool areUpdatesPaused() const { return m_updatesPaused; }
    
signals:
    void frameRendered(double fps);
    void canvasResized(const QSize& newSize);
    
protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    
    // Override for custom background
    virtual void drawBackground(QPainter& painter);
    
    // Canvas info
    QPointF canvasCenter() const { return QPointF(width() / 2.0, height() / 2.0); }
    QPointF pixelsPerDegree() const { return m_pixelsPerDegree; }
    
    // Force update
    void scheduleUpdate();
    
        QElapsedTimer m_frameTimer;
        
private:
    struct DrawLayer {
        QString name;
        std::function<void(QPainter&)> drawFunc;
        int zOrder;
        bool visible;
    };
    
    void updateTransforms();
    void updateMetrics();
    
    // Frame rate control
    QTimer* m_updateTimer;
    int m_targetFPS;
    double m_actualFPS;
    int m_frameCount;
    qint64 m_lastFPSUpdate;
    bool m_updatesPaused;
    
    // Coordinate system
    QSizeF m_visualRange;  // degrees
    QPointF m_pixelsPerDegree;
    
    // Layers
    QVector<DrawLayer> m_layers;
    bool m_layersNeedSort;
    
    // Stimulus underlay
    std::function<void(QPainter&, const QRectF&)> m_stimulusRenderer;
};
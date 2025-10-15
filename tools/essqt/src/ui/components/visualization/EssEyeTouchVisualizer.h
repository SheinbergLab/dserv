#pragma once
#include "EssVisualizationWidget.h"
#include "EssWindowIndicator.h"
#include <QQueue>
#include <QElapsedTimer>
#include <memory>

class EssEyeTouchVisualizer : public EssVisualizationWidget {
    Q_OBJECT
    
public:
    enum WindowType {
        Rectangle = 0,
        Ellipse = 1
    };
    
    struct Window {
        int id = 0;
        bool active = false;
        WindowType type = Rectangle;
        QPointF center;      // degrees
        QSizeF size;         // degrees
        QPointF centerRaw;   // raw units (ADC or pixels)
        QSizeF sizeRaw;      // raw units
    };
    
    explicit EssEyeTouchVisualizer(QWidget *parent = nullptr);
    ~EssEyeTouchVisualizer();
    
    // Display options
    void setShowTrails(bool show);
    void setShowGrid(bool show);
    void setShowLabels(bool show);
    void setTrailLength(int maxPoints);
    
    // Virtual input control
    void setVirtualInputEnabled(bool enabled);
    bool isVirtualInputEnabled() const { return m_virtualInputEnabled; }
    void setVirtualEyePosition(float degX, float degY);
    
    // Continuous update mode
    void setContinuousUpdateEnabled(bool enabled);
    bool isContinuousUpdateEnabled() const { return m_continuousUpdateEnabled; }
    void setUpdateRate(int hz);
    int updateRate() const { return m_updateRate; }
    
    // Data updates
    void updateEyePosition(int adcX, int adcY);
    void updateTouchPosition(int screenX, int screenY);
    void updateEyeWindows(const QVector<Window>& windows);
    void updateTouchWindows(const QVector<Window>& windows);
    void updateEyeWindowStatus(uint8_t statusMask);
    void updateTouchWindowStatus(uint8_t statusMask);
    void updateScreenDimensions(int width, int height, double halfX, double halfY);
    
    // Clear functions
    void clearTrails();
    void clearTouchPosition();
    void resetVirtualInput();
    
    // Access to window data
    const QVector<Window>& eyeWindows() const { return m_eyeWindows; }
    const QVector<Window>& touchWindows() const { return m_touchWindows; }
    
    // Access to current window status
	uint8_t eyeWindowStatus() const { return m_eyeWindowStatus; }
    uint8_t touchWindowStatus() const { return m_touchWindowStatus; }
    
    // Access to indicators
    EssWindowIndicator* eyeWindowIndicator() const { return m_eyeIndicator; }
    EssWindowIndicator* touchWindowIndicator() const { return m_touchIndicator; }

  // Conversion from eye units to degrees
  void setPointsPerDegree(double x, double y) {
    m_pointsPerDegX = x;
    m_pointsPerDegY = y;
  }
  double pointsPerDegreeX() const { return m_pointsPerDegX; }
  double pointsPerDegreeY() const { return m_pointsPerDegY; }  
signals:
    void virtualEyePosition(int adcX, int adcY);
    void virtualTouchEvent(int screenX, int screenY);
    void eyePositionChanged(double degX, double degY);
    void touchPositionChanged(double degX, double degY);
    
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void timerEvent(QTimerEvent *event) override;
    void drawBackground(QPainter& painter) override;
    
private slots:
    void sendContinuousUpdate();
    
private:
    // Conversion constants matching FLTK version
    static constexpr int ADC_CENTER = 2048;
    double m_pointsPerDegX;
    double m_pointsPerDegY;
    
    // Drawing functions
    void setupDrawLayers();
    void drawGrid(QPainter& painter);
    void drawEyeWindows(QPainter& painter);
    void drawTouchWindows(QPainter& painter);
    void drawEyePosition(QPainter& painter);
    void drawTouchPosition(QPainter& painter);
    void drawEyeTrails(QPainter& painter);
    void drawVirtualEye(QPainter& painter);
    void drawVirtualTouch(QPainter& painter);
    void drawWindow(QPainter& painter, const Window& window, 
                   const QColor& color, bool isInside);
    
    // Coordinate conversions
    QPointF adcToDegrees(int adcX, int adcY) const;
    QPoint degreesToAdc(const QPointF& degrees) const;
    QPointF touchPixelsToDegrees(int pixX, int pixY) const;
    QPoint degreesToTouchPixels(const QPointF& degrees) const;
    QPointF degreesToCanvas(const QPointF& degrees) const;
    QPointF canvasToDegrees(const QPointF& canvasPos) const;

    // Trail management
    void addTrailPoint(const QPointF& point);
    
    // Display options
    bool m_showTrails;
    bool m_showGrid;
    bool m_showLabels;
    int m_maxTrailPoints;
    QRect m_drawingRect; 
    
    // Virtual input
    bool m_virtualInputEnabled;
    bool m_virtualEyeDragging;
    QPointF m_virtualEyePos;  // degrees
    QPointF m_dragOffset;
    QElapsedTimer m_virtualTouchTimer;
    QPointF m_virtualTouchPos;  // degrees
    bool m_virtualTouchActive;
    
    // Continuous update mode
    QTimer* m_continuousUpdateTimer;
    bool m_continuousUpdateEnabled;
    int m_updateRate;  // Hz
    
    // Current data
    QPointF m_eyePosition;     // degrees
    QPointF m_eyePositionRaw;  // ADC units
    QPointF m_touchPosition;   // degrees
    QPoint m_touchPositionRaw; // screen pixels
    bool m_touchActive;
    QElapsedTimer m_touchClearTimer;
    int m_touchTimeoutTimerId;
    
    // Windows
    QVector<Window> m_eyeWindows;
    QVector<Window> m_touchWindows;
    uint8_t m_eyeWindowStatus;
    uint8_t m_touchWindowStatus;
    
    // Screen info for touch conversion
    QSize m_screenSize;
    QPointF m_screenHalfDegrees;
    
    // Trail data
    struct TrailPoint {
        QPointF position;  // degrees
        qint64 timestamp;
    };
    QQueue<TrailPoint> m_trailPoints;
    
    // Window indicators
    EssWindowIndicator* m_eyeIndicator;
    EssWindowIndicator* m_touchIndicator;
};

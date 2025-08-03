#pragma once
#include "EssEyeTouchVisualizer.h"
#include <QWidget>
#include <QCheckBox>
#include <QPushButton>
#include <QSpinBox>

class EssEyeTouchVisualizerWidget : public QWidget {
    Q_OBJECT
    
public:
    explicit EssEyeTouchVisualizerWidget(QWidget *parent = nullptr);
    
    // Access to the visualizer
    EssEyeTouchVisualizer* visualizer() const { return m_visualizer; }
    
    // Data update methods matching your format
    void updateTouchPosition(const QVariant& data);
    void updateEyeWindowSetting(const QString& data);
    void updateEyeWindowStatus(const QString& data);
    void updateTouchWindowSetting(const QString& data);
    void updateTouchWindowStatus(const QString& data);
    void updateEyePosition(int rawX, int rawY, float degX, float degY);                                                
    void updateEyePosition(const QString& data);
    void updateScreenDimensions(const QVariant& data, const QString& param);
    
    // Size hint overrides for better floating behavior
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    
signals:
    void virtualEyeData(const QByteArray& data);
    void virtualTouchData(const QByteArray& data);
    
private:
    void setupUI();
    void connectSignals();
    void updateConnectionState();
    void sendVirtualEyeData(int adcX, int adcY);
    void sendVirtualTouchData(int screenX, int screenY);
    void handleDatapoint(const QString& name, const QVariant& value, qint64 timestamp);
    
    EssEyeTouchVisualizer* m_visualizer;
    QCheckBox* m_trailsCheck;
    QCheckBox* m_gridCheck;
    QCheckBox* m_labelsCheck;
    QCheckBox* m_virtualCheck;
    QPushButton* m_resetButton;
    QCheckBox* m_continuousCheck;
    QSpinBox* m_rateSpinBox;
    
    // Track initialization state like FLTK version
    bool m_initialized = false;
};
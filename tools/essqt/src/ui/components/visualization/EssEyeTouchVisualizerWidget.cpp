#include "core/EssApplication.h"
#include "core/EssDataProcessor.h"
#include "core/EssCommandInterface.h"
#include "EssEyeTouchVisualizerWidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QSpinBox>
#include <QLabel>
#include <QDataStream>

EssEyeTouchVisualizerWidget::EssEyeTouchVisualizerWidget(QWidget *parent)
    : QWidget(parent)
    , m_visualizer(new EssEyeTouchVisualizer())
{
    setupUI();
    connectSignals();
    
    // Connect directly to data processor
    auto* app = EssApplication::instance();
    if (app && app->dataProcessor()) {
        connect(app->dataProcessor(), &EssDataProcessor::genericDatapointReceived,
                this, &EssEyeTouchVisualizerWidget::handleDatapoint);
    }
    
      // Connect virtual data output to command interface
    if (app && app->commandInterface()) {
        auto* cmdInterface = app->commandInterface();
        
        connect(this, &EssEyeTouchVisualizerWidget::virtualEyeData,
                [cmdInterface](const QByteArray& data) {
                    // Extract values
                    qint16 adcY = (data[0] & 0xFF) | ((data[1] & 0xFF) << 8);
                    qint16 adcX = (data[2] & 0xFF) | ((data[3] & 0xFF) << 8);
                    
                    // Send via ESS command
                    QString cmd = QString("set d [binary format s2 {%1 %2}]; dservSetData ain/vals 0 4 $d; unset d")
                                 .arg(adcY).arg(adcX);
                    cmdInterface->executeEss(cmd);
                });
        
        connect(this, &EssEyeTouchVisualizerWidget::virtualTouchData,
                [cmdInterface](const QByteArray& data) {
                    // Extract values
                    qint16 x = (data[0] & 0xFF) | ((data[1] & 0xFF) << 8);
                    qint16 y = (data[2] & 0xFF) | ((data[3] & 0xFF) << 8);
                    
                    // Send via ESS command
                    QString cmd = QString("set d [binary format s2 {%1 %2}]; dservSetData mtouch/touchvals 0 4 $d; unset d")
                                 .arg(x).arg(y);
                    cmdInterface->executeEss(cmd);
                });
    }
    
// Monitor connection state
    if (app && app->commandInterface()) {
        auto* cmdInterface = app->commandInterface();
        
        // Connect to connection state changes
        connect(cmdInterface, &EssCommandInterface::connected,
                this, [this](const QString&) { updateConnectionState(); });
        connect(cmdInterface, &EssCommandInterface::disconnected,
                this, [this]() { updateConnectionState(); });
        
        // Set initial state
        updateConnectionState();
    }
}

// Add this method to handle all datapoints
void EssEyeTouchVisualizerWidget::handleDatapoint(const QString& name, 
                                                  const QVariant& value, 
                                                  qint64 timestamp) {
    Q_UNUSED(timestamp)
    
    if (name == "ess/em_pos") {
        // Handle QVariantMap format
        if (value.typeId() == QMetaType::QVariantMap) {
            QVariantMap map = value.toMap();
            
            int rawX = map["d1"].toInt();
            int rawY = map["d2"].toInt();
            float degX = map["x"].toFloat();
            float degY = map["y"].toFloat();

            updateEyePosition(rawX, rawY, degX, degY);
            
        } else {
            // Fallback to string parsing if format changes
            updateEyePosition(value.toString());
        }
    }
    else if (name == "mtouch/touchvals") {
        updateTouchPosition(value);
    }
    else if (name == "em/settings") {
        updateEyeTrackingSettings(value);
    }    
    else if (name == "ess/em_region_setting") {
        updateEyeWindowSetting(value.toString());
    }
    else if (name == "ess/em_region_status") {
        updateEyeWindowStatus(value.toString());
    }
    else if (name == "ess/touch_region_setting") {
        updateTouchWindowSetting(value.toString());
    }
    else if (name == "ess/touch_region_status") {
        updateTouchWindowStatus(value.toString());
    }
    else if (name.startsWith("ess/screen_")) {
        updateScreenDimensions(value, name);
    }
}

void EssEyeTouchVisualizerWidget::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    
    // Container for both indicators at the top
    QWidget* indicatorContainer = new QWidget();
    QVBoxLayout* indicatorLayout = new QVBoxLayout(indicatorContainer);
    indicatorLayout->setContentsMargins(0, 0, 0, 0);
    indicatorLayout->setSpacing(0);
    
    // Add both window indicators to the container
    indicatorLayout->addWidget(m_visualizer->eyeWindowIndicator());
    indicatorLayout->addWidget(m_visualizer->touchWindowIndicator());
    
    // Add the indicator container at the top
    mainLayout->addWidget(indicatorContainer);
    
    // Main visualizer
    mainLayout->addWidget(m_visualizer, 1);  // Stretch factor 1
    
    // Control panel with two rows
    QGroupBox* controlGroup = new QGroupBox("Controls");
    QVBoxLayout* controlsVLayout = new QVBoxLayout(controlGroup);
    controlsVLayout->setSpacing(2);
    controlsVLayout->setContentsMargins(5, 5, 5, 5);
    
    // First row - Display options
    QWidget* row1Widget = new QWidget();
    QHBoxLayout* row1Layout = new QHBoxLayout(row1Widget);
    row1Layout->setContentsMargins(0, 0, 0, 0);
    row1Layout->setSpacing(5);
    
    // Display options label (right-aligned, fixed width)
    QLabel* displayLabel = new QLabel("Display:");
    displayLabel->setStyleSheet("font-weight: bold;");
    displayLabel->setFixedWidth(60);
    displayLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row1Layout->addWidget(displayLabel);
    
    m_trailsCheck = new QCheckBox("Trails");
    m_gridCheck = new QCheckBox("Grid");
    m_gridCheck->setChecked(true);
    m_labelsCheck = new QCheckBox("Labels");
    m_labelsCheck->setChecked(true);
    
    row1Layout->addWidget(m_trailsCheck);
    row1Layout->addWidget(m_gridCheck);
    row1Layout->addWidget(m_labelsCheck);
    
    row1Layout->addSpacing(10);
    
    // Trail length spinner
    row1Layout->addWidget(new QLabel("Trail:"));
    QSpinBox* trailSpin = new QSpinBox();
    trailSpin->setRange(10, 200);
    trailSpin->setValue(50);
    trailSpin->setSuffix(" pts");
    trailSpin->setMaximumWidth(80);
    connect(trailSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            m_visualizer, &EssEyeTouchVisualizer::setTrailLength);
    row1Layout->addWidget(trailSpin);
    
    row1Layout->addStretch();
    
    // Second row - Virtual input controls
    QWidget* row2Widget = new QWidget();
    QHBoxLayout* row2Layout = new QHBoxLayout(row2Widget);
    row2Layout->setContentsMargins(0, 0, 0, 0);
    row2Layout->setSpacing(5);
    
    // Virtual input label (right-aligned, fixed width - same as Display)
    QLabel* virtualLabel = new QLabel("Virtual:");
    virtualLabel->setStyleSheet("font-weight: bold;");
    virtualLabel->setFixedWidth(60);
    virtualLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row2Layout->addWidget(virtualLabel);
    
    m_virtualCheck = new QCheckBox("Enable");
    row2Layout->addWidget(m_virtualCheck);
    
    m_resetButton = new QPushButton("Reset");
    m_resetButton->setEnabled(false);
    row2Layout->addWidget(m_resetButton);
    
    row2Layout->addSpacing(10);
    
    m_continuousCheck = new QCheckBox("Cont");
    m_continuousCheck->setEnabled(false);
    row2Layout->addWidget(m_continuousCheck);
    
    row2Layout->addWidget(new QLabel("Rate:"));
    m_rateSpinBox = new QSpinBox();
    m_rateSpinBox->setRange(1, 1000);
    m_rateSpinBox->setValue(250);
    m_rateSpinBox->setSuffix(" Hz");
    m_rateSpinBox->setEnabled(false);
    m_rateSpinBox->setMaximumWidth(80);
    row2Layout->addWidget(m_rateSpinBox);
    
    row2Layout->addStretch();
    
    // Add both rows to control group
    controlsVLayout->addWidget(row1Widget);
    controlsVLayout->addWidget(row2Widget);
    
    mainLayout->addWidget(controlGroup);
}

void EssEyeTouchVisualizerWidget::connectSignals() {
    // Display options
    connect(m_trailsCheck, &QCheckBox::toggled, 
            m_visualizer, &EssEyeTouchVisualizer::setShowTrails);
    connect(m_gridCheck, &QCheckBox::toggled, 
            m_visualizer, &EssEyeTouchVisualizer::setShowGrid);
    connect(m_labelsCheck, &QCheckBox::toggled, 
            m_visualizer, &EssEyeTouchVisualizer::setShowLabels);
    
    // Continuous update controls
    connect(m_continuousCheck, &QCheckBox::toggled, [this](bool checked) {
        m_visualizer->setContinuousUpdateEnabled(checked);
        m_rateSpinBox->setEnabled(checked && m_virtualCheck->isChecked());
    });
    
    connect(m_rateSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            m_visualizer, &EssEyeTouchVisualizer::setUpdateRate);
    
    // Update the virtual check connection to also handle continuous controls
    connect(m_virtualCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) {
            // Check if we're connected to a backend
            auto* app = EssApplication::instance();
            if (!app || !app->commandInterface() || !app->commandInterface()->isConnected()) {
                // Not connected - show message and uncheck
                m_virtualCheck->setChecked(false);
                
                return;
            }
        }
        
        m_visualizer->setVirtualInputEnabled(checked);
        m_resetButton->setEnabled(checked);
        m_continuousCheck->setEnabled(checked);
        m_continuousCheck->setChecked(true);
        m_rateSpinBox->setEnabled(checked && m_continuousCheck->isChecked());
        
        if (checked && m_continuousCheck->isChecked()) {
        	m_visualizer->setContinuousUpdateEnabled(true);
    	}
    });
    
    connect(m_resetButton, &QPushButton::clicked,
            m_visualizer, &EssEyeTouchVisualizer::resetVirtualInput);
    
    // Virtual data output
    connect(m_visualizer, &EssEyeTouchVisualizer::virtualEyePosition,
            this, &EssEyeTouchVisualizerWidget::sendVirtualEyeData);
    connect(m_visualizer, &EssEyeTouchVisualizer::virtualTouchEvent,
            this, &EssEyeTouchVisualizerWidget::sendVirtualTouchData);
}

// Update the connection state check method:
void EssEyeTouchVisualizerWidget::updateConnectionState() {
    auto* app = EssApplication::instance();
    bool connected = app && app->commandInterface() && app->commandInterface()->isConnected();
    
    m_virtualCheck->setEnabled(connected);
    
    // If disconnected while virtual input is active, disable it
    if (!connected && m_virtualCheck->isChecked()) {
        m_virtualCheck->setChecked(false);
    }
    
    // Update tooltip to explain why it's disabled
    if (!connected) {
        m_virtualCheck->setToolTip("Virtual input requires connection to ESS backend");
    } else {
        m_virtualCheck->setToolTip("");
    }
}

void EssEyeTouchVisualizerWidget::updateEyePosition(int rawX, int rawY, 
                                                   float degX, float degY) {
    // Update visualizer with raw ADC values
    m_visualizer->updateEyePosition(rawX, rawY);
    
    // Initialize virtual eye position on first real data
    if (!m_initialized && m_virtualCheck->isChecked()) {
        m_visualizer->setVirtualEyePosition(degX, degY);
        m_initialized = true;
    }
}

void EssEyeTouchVisualizerWidget::updateEyePosition(const QString& data) {
    // Parse: "raw_x raw_y deg_x deg_y"
    QStringList parts = data.split(' ');
    if (parts.size() >= 4) {
        int rawX = parts[0].toInt();
        int rawY = parts[1].toInt();
        float degX = parts[2].toFloat();
        float degY = parts[3].toFloat();
        
        updateEyePosition(rawX, rawY, degX, degY);

    }
}

void EssEyeTouchVisualizerWidget::updateTouchPosition(const QVariant& data) {
    // Handle both array and string formats
    if (data.typeId() == QMetaType::QVariantList) {
        QVariantList list = data.toList();
        if (list.size() >= 2) {
            m_visualizer->updateTouchPosition(list[0].toInt(), list[1].toInt());
        }
    } else {
        QString str = data.toString();
        QStringList parts = str.split(' ');
        if (parts.size() >= 2) {
            m_visualizer->updateTouchPosition(parts[0].toInt(), parts[1].toInt());
        }
    }
}

void EssEyeTouchVisualizerWidget::updateEyeTrackingSettings(const QVariant& value) {
    // Parse the settings dictionary/map
    QVariantMap settings = value.toMap();
    
    if (settings.contains("to_deg_h")) {
        double pointsPerDegX = settings["to_deg_h"].toDouble();
        m_visualizer->setPointsPerDegree(pointsPerDegX, 
                                        m_visualizer->pointsPerDegreeY());
    }
    
    if (settings.contains("to_deg_v")) {
        double pointsPerDegY = settings["to_deg_v"].toDouble();
        m_visualizer->setPointsPerDegree(m_visualizer->pointsPerDegreeX(),
                                        pointsPerDegY);
    }
}

void EssEyeTouchVisualizerWidget::updateEyeWindowSetting(const QString& data) {
    // Parse: "reg active state type cx cy dx dy ..."
    QStringList parts = data.split(' ');
    if (parts.size() < 8) return;
    
    int reg = parts[0].toInt();
    bool active = parts[1].toInt() != 0;
    // int state = parts[2].toInt(); // Not used for display
    int type = parts[3].toInt();
    int cx = parts[4].toInt();  // ADC units
    int cy = parts[5].toInt();  // ADC units
    int dx = parts[6].toInt();  // ADC units (radius/half-width)
    int dy = parts[7].toInt();  // ADC units (radius/half-height)
    
    if (reg >= 0 && reg < 8) {
        auto windows = m_visualizer->eyeWindows();
        if (reg < windows.size()) {
            windows[reg].active = active;
            windows[reg].type = (type == 1) ? EssEyeTouchVisualizer::Ellipse : EssEyeTouchVisualizer::Rectangle;
            
	    // Use dynamic conversion - should match to_deg_h/v from ess
	    float pointsPerDegX = m_visualizer->pointsPerDegreeX();	    
	    float pointsPerDegY = m_visualizer->pointsPerDegreeY();   
            float sx = (cx - 2048) / pointsPerDegX;
            float sy = -1.0f * (cy - 2048) / pointsPerDegY;
            windows[reg].center = QPointF(sx, sy);
            
            // Size in degrees (dx and dy are half-widths/radii)
            windows[reg].size = QSizeF(dx / pointsPerDegX, dy / pointsPerDegY);
            
            // Store raw values too
            windows[reg].centerRaw = QPointF(cx, cy);
            windows[reg].sizeRaw = QSizeF(dx, dy);
            
            m_visualizer->updateEyeWindows(windows);
            
            // Also update the indicator to reflect active state
            bool inside = (m_visualizer->eyeWindowStatus() & (1 << reg)) != 0;
            m_visualizer->eyeWindowIndicator()->setWindowStatus(reg, active, inside);

        }
    }
}

void EssEyeTouchVisualizerWidget::updateEyeWindowStatus(const QString& data) {
    // Parse: "something states ..."
    QStringList parts = data.split(' ');
    if (parts.size() < 2) return;
    
    // Index 1 contains the bitmask
    uint8_t states = parts[1].toUInt();
    m_visualizer->updateEyeWindowStatus(states);
}

void EssEyeTouchVisualizerWidget::updateTouchWindowSetting(const QString& data) {
    // Similar format to eye window but with screen pixel coordinates
    // Parse: "reg active state type cx cy dx dy ..."
    QStringList parts = data.split(' ');
    if (parts.size() < 8) return;
    
    int reg = parts[0].toInt();
    bool active = parts[1].toInt() != 0;
    // int state = parts[2].toInt(); // Not used for display
    int type = parts[3].toInt();
    int cx = parts[4].toInt();  // Screen pixel units
    int cy = parts[5].toInt();  // Screen pixel units
    int dx = parts[6].toInt();  // Width/radius in pixels
    int dy = parts[7].toInt();  // Height/radius in pixels
    
    if (reg >= 0 && reg < 8) {
        auto windows = m_visualizer->touchWindows();
        if (reg < windows.size()) {
            windows[reg].active = active;
            windows[reg].type = (type == 1) ? EssEyeTouchVisualizer::Ellipse : EssEyeTouchVisualizer::Rectangle;
            
            // Store raw pixel values
            windows[reg].centerRaw = QPointF(cx, cy);
            windows[reg].sizeRaw = QSizeF(dx, dy);
            
            // The visualizer will convert to degrees when it has screen dimensions
            m_visualizer->updateTouchWindows(windows);
            
            // Also update the indicator to reflect active state
            bool inside = (m_visualizer->touchWindowStatus() & (1 << reg)) != 0;
            m_visualizer->touchWindowIndicator()->setWindowStatus(reg, active, inside);

        }
    }
}

void EssEyeTouchVisualizerWidget::updateTouchWindowStatus(const QString& data) {
    // Parse: "changes states touch_x touch_y"
    QStringList parts = data.split(' ');
    if (parts.size() < 2) return;
    
    // Index 1 contains the bitmask
    uint8_t states = parts[1].toUInt();
    m_visualizer->updateTouchWindowStatus(states);
    
    // Also update touch position if provided
    if (parts.size() >= 4) {
        int touchX = parts[2].toInt();
        int touchY = parts[3].toInt();
        if (touchX != 0 || touchY != 0) {
            m_visualizer->updateTouchPosition(touchX, touchY);
        }
    }
}

void EssEyeTouchVisualizerWidget::updateScreenDimensions(const QVariant& data, 
                                                        const QString& param) {
    static int width = 800, height = 600;
    static double halfX = 10.0, halfY = 7.5;
    
    if (param == "ess/screen_w") {
        width = data.toInt();
    } else if (param == "ess/screen_h") {
        height = data.toInt();
    } else if (param == "ess/screen_halfx") {
        halfX = data.toDouble();
    } else if (param == "ess/screen_halfy") {
        halfY = data.toDouble();
    }
    
    m_visualizer->updateScreenDimensions(width, height, halfX, halfY);
}

void EssEyeTouchVisualizerWidget::sendVirtualEyeData(int adcX, int adcY) {
    // Format matching your binary protocol
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << (qint16)adcY << (qint16)adcX;  // Note: Y first, matching your format
    
    emit virtualEyeData(data);
}

void EssEyeTouchVisualizerWidget::sendVirtualTouchData(int screenX, int screenY) {
    // Format matching your binary protocol
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << (qint16)screenX << (qint16)screenY;
    
    emit virtualTouchData(data);
}

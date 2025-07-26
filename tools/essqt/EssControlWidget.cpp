#include "EssControlWidget.h"
#include <QGroupBox>
#include <QSplitter>

EssControlWidget::EssControlWidget(QWidget* parent) : QWidget(parent) {
    setupUI();
    forwardSystemSignals();
}

void EssControlWidget::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    
    // Create all sections
    createSubjectSection();
    createControlSection();
    createStatusSection();
    
    // Add system configuration widget
    systemConfigWidget = new SystemConfigWidget(this);
    
    createSettingsSection();
    
    // Add all components to main layout
    mainLayout->addWidget(subjectGroup);
    mainLayout->addWidget(controlGroup);
    mainLayout->addWidget(statusGroup);
    mainLayout->addWidget(systemConfigWidget);
    mainLayout->addWidget(settingsGroup);
    
    // Add stretch to push everything to the top
    mainLayout->addStretch();
}

void EssControlWidget::createSubjectSection() {
    subjectGroup = new QGroupBox("Subject", this);
    auto* layout = new QHBoxLayout(subjectGroup);
    
    auto* label = new QLabel("Subject:");
    subjectCombo = new QComboBox();
    subjectCombo->setMinimumWidth(150);
    
    layout->addWidget(label);
    layout->addWidget(subjectCombo);
    
    connect(subjectCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EssControlWidget::onSubjectChanged);
}

void EssControlWidget::createControlSection() {
    controlGroup = new QGroupBox("Control", this);
    auto* layout = new QHBoxLayout(controlGroup);
    
    goBtn = new QPushButton("Go");
    stopBtn = new QPushButton("Stop");
    resetBtn = new QPushButton("Reset");
    
    // Style the buttons to match your FLTK appearance
    goBtn->setMinimumHeight(40);
    stopBtn->setMinimumHeight(40);
    resetBtn->setMinimumHeight(40);
    
    goBtn->setStyleSheet("QPushButton { font-size: 18px; font-weight: bold; }");
    stopBtn->setStyleSheet("QPushButton { font-size: 18px; font-weight: bold; }");
    resetBtn->setStyleSheet("QPushButton { font-size: 18px; font-weight: bold; }");
    
    layout->addWidget(goBtn);
    layout->addWidget(stopBtn);
    layout->addWidget(resetBtn);
    
    connect(goBtn, &QPushButton::clicked, this, &EssControlWidget::startRequested);
    connect(stopBtn, &QPushButton::clicked, this, &EssControlWidget::stopRequested);
    connect(resetBtn, &QPushButton::clicked, this, &EssControlWidget::resetRequested);
}

void EssControlWidget::createStatusSection() {
    statusGroup = new QGroupBox("Status", this);
    auto* layout = new QVBoxLayout(statusGroup);
    
    // Status display
    auto* statusLayout = new QHBoxLayout();
    statusLayout->addWidget(new QLabel("Status:"));
    systemStatusLabel = new QLabel("Stopped");
    systemStatusLabel->setStyleSheet("QLabel { font-weight: bold; }");
    statusLayout->addWidget(systemStatusLabel);
    statusLayout->addStretch();
    
    // Observation count with indicator
    auto* obsLayout = new QHBoxLayout();
    obsLayout->addWidget(new QLabel("Obs:"));
    obsCountLabel = new QLabel("0/0");
    obsIndicator = new QLabel("●");
    obsIndicator->setFixedSize(16, 16);
    obsIndicator->setStyleSheet("QLabel { color: gray; }");
    
    obsLayout->addWidget(obsCountLabel);
    obsLayout->addWidget(obsIndicator);
    obsLayout->addStretch();
    
    layout->addLayout(statusLayout);
    layout->addLayout(obsLayout);
}

void EssControlWidget::createSettingsSection() {
    settingsGroup = new QGroupBox("Settings", this);
    auto* layout = new QHBoxLayout(settingsGroup);
    
    saveSettingsBtn = new QPushButton("Save Settings");
    resetSettingsBtn = new QPushButton("Reset Settings");
    
    layout->addWidget(saveSettingsBtn);
    layout->addWidget(resetSettingsBtn);
    
    connect(saveSettingsBtn, &QPushButton::clicked, 
            this, &EssControlWidget::saveSettingsRequested);
    connect(resetSettingsBtn, &QPushButton::clicked, 
            this, &EssControlWidget::resetSettingsRequested);
}

void EssControlWidget::forwardSystemSignals() {
    // Forward all system configuration signals
    connect(systemConfigWidget, &SystemConfigWidget::systemChanged,
            this, &EssControlWidget::systemChanged);
    connect(systemConfigWidget, &SystemConfigWidget::protocolChanged,
            this, &EssControlWidget::protocolChanged);
    connect(systemConfigWidget, &SystemConfigWidget::variantChanged,
            this, &EssControlWidget::variantChanged);
    connect(systemConfigWidget, &SystemConfigWidget::reloadSystemRequested,
            this, &EssControlWidget::reloadSystemRequested);
    connect(systemConfigWidget, &SystemConfigWidget::reloadProtocolRequested,
            this, &EssControlWidget::reloadProtocolRequested);
    connect(systemConfigWidget, &SystemConfigWidget::reloadVariantRequested,
            this, &EssControlWidget::reloadVariantRequested);
}

QString EssControlWidget::currentSubject() const {
    return subjectCombo->currentText();
}

void EssControlWidget::setSubjectList(const QStringList& subjects) {
    subjectCombo->clear();
    subjectCombo->addItems(subjects);
}

void EssControlWidget::setCurrentSubject(const QString& subject) {
    int index = subjectCombo->findText(subject);
    if (index >= 0) {
        subjectCombo->setCurrentIndex(index);
    }
}

void EssControlWidget::setSystemStatus(const QString& status) {
    systemStatusLabel->setText(status);
    
    // Update color based on status (matching your FLTK logic)
    if (status == "Stopped") {
        systemStatusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");
    } else if (status == "Running") {
        systemStatusLabel->setStyleSheet("QLabel { color: #28c814; font-weight: bold; }");
    } else {
        systemStatusLabel->setStyleSheet("QLabel { color: black; font-weight: bold; }");
    }
}

void EssControlWidget::setObservationCount(const QString& obsCount) {
    obsCountLabel->setText(obsCount);
}

void EssControlWidget::setObservationActive(bool active) {
    if (active) {
        obsIndicator->setStyleSheet("QLabel { color: red; }");
    } else {
        obsIndicator->setStyleSheet("QLabel { color: gray; }");
    }
}

void EssControlWidget::onSubjectChanged(int index) {
    if (index >= 0) {
        emit subjectChanged(subjectCombo->itemText(index));
    }
}

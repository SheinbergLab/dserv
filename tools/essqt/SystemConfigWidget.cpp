#include "SystemConfigWidget.h"
#include <QGridLayout>

SystemConfigWidget::SystemConfigWidget(QWidget* parent) 
    : QGroupBox("System Configuration", parent) {
    setupUI();
}

void SystemConfigWidget::setupUI() {
    // Create the main layout
    auto* mainLayout = new QVBoxLayout(this);
    
    // Create a grid layout for the dropdowns and reload buttons
    auto* gridLayout = new QGridLayout();
    
    // System dropdown
    auto* systemLabel = new QLabel("System:");
    systemCombo = new QComboBox();
    systemCombo->setMinimumWidth(180);
    reloadSystemBtn = new QPushButton("⟳");
    reloadSystemBtn->setFixedSize(26, 26);
    reloadSystemBtn->setToolTip("Reload System");
    
    gridLayout->addWidget(systemLabel, 0, 0);
    gridLayout->addWidget(systemCombo, 0, 1);
    gridLayout->addWidget(reloadSystemBtn, 0, 2);
    
    // Protocol dropdown
    auto* protocolLabel = new QLabel("Protocol:");
    protocolCombo = new QComboBox();
    protocolCombo->setMinimumWidth(180);
    reloadProtocolBtn = new QPushButton("⟳");
    reloadProtocolBtn->setFixedSize(26, 26);
    reloadProtocolBtn->setToolTip("Reload Protocol");
    
    gridLayout->addWidget(protocolLabel, 1, 0);
    gridLayout->addWidget(protocolCombo, 1, 1);
    gridLayout->addWidget(reloadProtocolBtn, 1, 2);
    
    // Variant dropdown
    auto* variantLabel = new QLabel("Variant:");
    variantCombo = new QComboBox();
    variantCombo->setMinimumWidth(180);
    reloadVariantBtn = new QPushButton("⟳");
    reloadVariantBtn->setFixedSize(26, 26);
    reloadVariantBtn->setToolTip("Reload Variant");
    
    gridLayout->addWidget(variantLabel, 2, 0);
    gridLayout->addWidget(variantCombo, 2, 1);
    gridLayout->addWidget(reloadVariantBtn, 2, 2);
    
    // Set column stretch to make combo boxes expand
    gridLayout->setColumnStretch(1, 1);
    
    mainLayout->addLayout(gridLayout);
    
    // Connect signals
    connect(systemCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SystemConfigWidget::onSystemChanged);
    connect(protocolCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SystemConfigWidget::onProtocolChanged);
    connect(variantCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SystemConfigWidget::onVariantChanged);
    
    connect(reloadSystemBtn, &QPushButton::clicked,
            this, &SystemConfigWidget::reloadSystemRequested);
    connect(reloadProtocolBtn, &QPushButton::clicked,
            this, &SystemConfigWidget::reloadProtocolRequested);
    connect(reloadVariantBtn, &QPushButton::clicked,
            this, &SystemConfigWidget::reloadVariantRequested);
}

QString SystemConfigWidget::currentSystem() const {
    return systemCombo->currentText();
}

QString SystemConfigWidget::currentProtocol() const {
    return protocolCombo->currentText();
}

QString SystemConfigWidget::currentVariant() const {
    return variantCombo->currentText();
}

void SystemConfigWidget::setSystemList(const QStringList& systems) {
    systemCombo->clear();
    systemCombo->addItems(systems);
}

void SystemConfigWidget::setProtocolList(const QStringList& protocols) {
    protocolCombo->clear();
    protocolCombo->addItems(protocols);
}

void SystemConfigWidget::setVariantList(const QStringList& variants) {
    variantCombo->clear();
    variantCombo->addItems(variants);
}

void SystemConfigWidget::setCurrentSystem(const QString& system) {
    int index = systemCombo->findText(system);
    if (index >= 0) {
        systemCombo->setCurrentIndex(index);
    }
}

void SystemConfigWidget::setCurrentProtocol(const QString& protocol) {
    int index = protocolCombo->findText(protocol);
    if (index >= 0) {
        protocolCombo->setCurrentIndex(index);
    }
}

void SystemConfigWidget::setCurrentVariant(const QString& variant) {
    int index = variantCombo->findText(variant);
    if (index >= 0) {
        variantCombo->setCurrentIndex(index);
    }
}

void SystemConfigWidget::clearSystems() {
    systemCombo->clear();
}

void SystemConfigWidget::clearProtocols() {
    protocolCombo->clear();
}

void SystemConfigWidget::clearVariants() {
    variantCombo->clear();
}

void SystemConfigWidget::onSystemChanged(int index) {
    if (index >= 0) {
        emit systemChanged(systemCombo->itemText(index));
    }
}

void SystemConfigWidget::onProtocolChanged(int index) {
    if (index >= 0) {
        emit protocolChanged(protocolCombo->itemText(index));
    }
}

void SystemConfigWidget::onVariantChanged(int index) {
    if (index >= 0) {
        emit variantChanged(variantCombo->itemText(index));
    }
}

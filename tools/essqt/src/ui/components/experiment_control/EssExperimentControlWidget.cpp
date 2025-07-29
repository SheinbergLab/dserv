#include "EssExperimentControlWidget.h"
#include "core/EssApplication.h"
#include "core/EssDataProcessor.h"
#include "core/EssCommandInterface.h"
#include "console/EssOutputConsole.h"

#include <QGroupBox>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QIntValidator>
#include <QDoubleValidator>
#include <QRegularExpressionValidator>
#include <QDebug>

EssExperimentControlWidget::EssExperimentControlWidget(QWidget *parent)
    : QWidget(parent)
    , m_isRunning(false)
    , m_isLoading(false)
    , m_currentObsId(0)
    , m_totalObs(0)
    , m_observationActive(false)
    , m_blockSignals(false)
{
    setupUi();
    connectSignals();
}

EssExperimentControlWidget::~EssExperimentControlWidget()
{
}

void EssExperimentControlWidget::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(6, 6, 6, 6);
    
    createControlSection();
    createStatusSection();
    createSystemConfigSection();
    createParameterSection();
    createVariantOptionsSection();
    createSettingsSection();
    
    mainLayout->addWidget(m_controlGroup);
    mainLayout->addWidget(m_statusGroup);
    mainLayout->addWidget(m_systemConfigGroup);
    mainLayout->addWidget(m_variantOptionsGroup);
    mainLayout->addWidget(m_parameterGroup);
    mainLayout->addWidget(m_settingsGroup);
    mainLayout->addStretch();
    
    // Set minimum width for better appearance
    setMinimumWidth(300);
}

void EssExperimentControlWidget::createControlSection()
{
    m_controlGroup = new QGroupBox("Control", this);
    QHBoxLayout *layout = new QHBoxLayout(m_controlGroup);
    
    m_startBtn = new QPushButton("Start");
    m_stopBtn = new QPushButton("Stop");
    m_resetBtn = new QPushButton("Reset");
    
    // Style buttons with rounded corners and consistent appearance
    QString buttonStyle = R"(
        QPushButton {
            font-size: 14px;
            padding: 8px 16px;
            border-radius: 6px;
        }
    )";
    
    m_startBtn->setMinimumHeight(30);
    m_stopBtn->setMinimumHeight(30);
    m_resetBtn->setMinimumHeight(30);
    
    layout->addWidget(m_startBtn);
    layout->addWidget(m_stopBtn);
    layout->addWidget(m_resetBtn);
    
    // Initial button states
    m_startBtn->setEnabled(true);
    m_stopBtn->setEnabled(false);
    m_resetBtn->setEnabled(true);
}

void EssExperimentControlWidget::createStatusSection()
{
    m_statusGroup = new QGroupBox("Status", this);
    QVBoxLayout *layout = new QVBoxLayout(m_statusGroup);
    
    // Status row
    QHBoxLayout *statusLayout = new QHBoxLayout();
    statusLayout->addWidget(new QLabel("Status:"));
    m_statusLabel = new QLabel("Unknown");
    m_statusLabel->setStyleSheet("QLabel { font-weight: bold; }");
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addStretch();
    
    // Progress row
    QHBoxLayout *progressLayout = new QHBoxLayout();
    progressLayout->addWidget(new QLabel("Progress:"));
    m_progressLabel = new QLabel("");
    m_progressLabel->setVisible(false);  // Hidden by default
    progressLayout->addWidget(m_progressLabel);
    progressLayout->addStretch();
    
    // Observation count row
    QHBoxLayout *obsLayout = new QHBoxLayout();
    obsLayout->addWidget(new QLabel("Observation:"));
    m_obsCountLabel = new QLabel("0/0");
    m_obsIndicator = new QLabel("●");
    m_obsIndicator->setFixedSize(16, 16);
    m_obsIndicator->setStyleSheet("QLabel { color: gray; }");
    
    obsLayout->addWidget(m_obsCountLabel);
    obsLayout->addWidget(m_obsIndicator);
    obsLayout->addStretch();
    
    layout->addLayout(statusLayout);
    layout->addLayout(progressLayout);  // Add progress layout
    layout->addLayout(obsLayout);
}

void EssExperimentControlWidget::createSystemConfigSection()
{
    m_systemConfigGroup = new QGroupBox("System Configuration", this);
    QGridLayout *layout = new QGridLayout(m_systemConfigGroup);
    
    // Reduce spacing between rows
    layout->setVerticalSpacing(4);
    layout->setContentsMargins(9, 15, 9, 9);
    
    // System row
    layout->addWidget(new QLabel("System:"), 0, 0);
    m_systemCombo = new QComboBox();
    m_systemCombo->setMinimumWidth(150);
    layout->addWidget(m_systemCombo, 0, 1);
    m_reloadSystemBtn = new QPushButton("↻");
    m_reloadSystemBtn->setMaximumWidth(30);
    m_reloadSystemBtn->setToolTip("Reload system list");
    layout->addWidget(m_reloadSystemBtn, 0, 2);
    
    // Protocol row
    layout->addWidget(new QLabel("Protocol:"), 1, 0);
    m_protocolCombo = new QComboBox();
    layout->addWidget(m_protocolCombo, 1, 1);
    m_reloadProtocolBtn = new QPushButton("↻");
    m_reloadProtocolBtn->setMaximumWidth(30);
    m_reloadProtocolBtn->setToolTip("Reload protocol list");
    layout->addWidget(m_reloadProtocolBtn, 1, 2);
    
    // Variant row
    layout->addWidget(new QLabel("Variant:"), 2, 0);
    m_variantCombo = new QComboBox();
    layout->addWidget(m_variantCombo, 2, 1);
    m_reloadVariantBtn = new QPushButton("↻");
    m_reloadVariantBtn->setMaximumWidth(30);
    m_reloadVariantBtn->setToolTip("Reload variant list");
    layout->addWidget(m_reloadVariantBtn, 2, 2);
    
    // Set column stretch
    layout->setColumnStretch(1, 1);
}

void EssExperimentControlWidget::createParameterSection()
{
    m_parameterGroup = new QGroupBox("Parameters", this);
    QVBoxLayout *layout = new QVBoxLayout(m_parameterGroup);
    
    // Create scroll area for parameters
    m_parameterScrollArea = new QScrollArea();
    m_parameterScrollArea->setWidgetResizable(true);
    m_parameterScrollArea->setMaximumHeight(200);
    
    m_parameterContainer = new QWidget();
    m_parameterLayout = new QFormLayout(m_parameterContainer);
    m_parameterLayout->setSpacing(4);
    m_parameterLayout->setHorizontalSpacing(20);
 
    m_parameterScrollArea->setWidget(m_parameterContainer);
    layout->addWidget(m_parameterScrollArea);
}

void EssExperimentControlWidget::createVariantOptionsSection()
{
    m_variantOptionsGroup = new QGroupBox("Variant Options", this);
    QVBoxLayout *layout = new QVBoxLayout(m_variantOptionsGroup);
    
    // Create scroll area for variant options
    m_variantOptionsScrollArea = new QScrollArea();
    m_variantOptionsScrollArea->setWidgetResizable(true);
    m_variantOptionsScrollArea->setMaximumHeight(150);
    
    m_variantOptionsContainer = new QWidget();
    m_variantOptionsLayout = new QFormLayout(m_variantOptionsContainer);
    m_variantOptionsLayout->setSpacing(4);
    
    m_variantOptionsScrollArea->setWidget(m_variantOptionsContainer);
    layout->addWidget(m_variantOptionsScrollArea);
}

void EssExperimentControlWidget::createSettingsSection()
{
    m_settingsGroup = new QGroupBox("Settings", this);
    QHBoxLayout *layout = new QHBoxLayout(m_settingsGroup);
    
    m_saveSettingsBtn = new QPushButton("Save Settings");
    m_resetSettingsBtn = new QPushButton("Reset Settings");
    
    layout->addWidget(m_saveSettingsBtn);
    layout->addWidget(m_resetSettingsBtn);
    
    connect(m_saveSettingsBtn, &QPushButton::clicked, 
            this, &EssExperimentControlWidget::onSaveSettingsClicked);
    connect(m_resetSettingsBtn, &QPushButton::clicked,
            this, &EssExperimentControlWidget::onResetSettingsClicked);
}

void EssExperimentControlWidget::connectSignals()
{
    // Connect to data processor
    if (EssApplication::instance() && EssApplication::instance()->dataProcessor()) {
        EssDataProcessor *dataProc = EssApplication::instance()->dataProcessor();
        
        connect(dataProc, &EssDataProcessor::systemStatusUpdated,
                this, &EssExperimentControlWidget::onSystemStatusUpdated);
        connect(dataProc, &EssDataProcessor::experimentStateChanged,
                this, &EssExperimentControlWidget::onExperimentStateChanged);
        connect(dataProc, &EssDataProcessor::genericDatapointReceived,
                this, &EssExperimentControlWidget::onGenericDatapointReceived);
    }
    
    // Connect to command interface for connection state
    if (EssApplication::instance() && EssApplication::instance()->commandInterface()) {
        EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
        
        connect(cmdInterface, &EssCommandInterface::connected,
                this, [this](const QString &host) {
                    // Re-enable controls on connection
                    m_systemCombo->setEnabled(true);
                    m_protocolCombo->setEnabled(true);
                    m_variantCombo->setEnabled(true);
                    m_reloadSystemBtn->setEnabled(true);
                    m_reloadProtocolBtn->setEnabled(true);
                    m_reloadVariantBtn->setEnabled(true);
                    m_saveSettingsBtn->setEnabled(true);
                    m_resetSettingsBtn->setEnabled(true);
                    // Buttons will be enabled based on state
                    updateButtonStates();
                });
                
        connect(cmdInterface, &EssCommandInterface::disconnected,
                this, &EssExperimentControlWidget::resetToDisconnectedState);
    }
    
    // Control buttons
    connect(m_startBtn, &QPushButton::clicked, this, &EssExperimentControlWidget::onStartClicked);
    connect(m_stopBtn, &QPushButton::clicked, this, &EssExperimentControlWidget::onStopClicked);
    connect(m_resetBtn, &QPushButton::clicked, this, &EssExperimentControlWidget::onResetClicked);
    
    // Combo boxes
    connect(m_systemCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EssExperimentControlWidget::onSystemComboChanged);
    connect(m_protocolCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EssExperimentControlWidget::onProtocolComboChanged);
    connect(m_variantCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &EssExperimentControlWidget::onVariantComboChanged);
    
    // Reload buttons
    connect(m_reloadSystemBtn, &QPushButton::clicked, 
            this, &EssExperimentControlWidget::onReloadSystemClicked);
    connect(m_reloadProtocolBtn, &QPushButton::clicked,
            this, &EssExperimentControlWidget::onReloadProtocolClicked);
    connect(m_reloadVariantBtn, &QPushButton::clicked,
            this, &EssExperimentControlWidget::onReloadVariantClicked);
    
    // Start in disconnected state
    resetToDisconnectedState();
}

void EssExperimentControlWidget::onSystemStatusUpdated(const QString &status)
{
    // This is for other status messages, not the running state
    // Could be used for temporary status messages if needed
}

void EssExperimentControlWidget::onExperimentStateChanged(const QString &state)
{
    m_isRunning = (state == "Running");
    updateButtonStates();
    updateStatusDisplay();
}

void EssExperimentControlWidget::onGenericDatapointReceived(const QString &name, 
                                                            const QVariant &value, 
                                                            qint64 timestamp)
{
    Q_UNUSED(timestamp)
    
    // Block signals while processing to prevent loops
    m_blockSignals = true;
    
    if (name.startsWith("system/")) {
        processSystemDatapoint(name, value);
    } else if (name.startsWith("ess/")) {
        processEssDatapoint(name, value);
    }
    
    m_blockSignals = false;
}

void EssExperimentControlWidget::processSystemDatapoint(const QString &name, const QVariant &value)
{
    // Currently we don't have specific system/ datapoints to handle
    // This is here for future expansion
    Q_UNUSED(name)
    Q_UNUSED(value)
}

void EssExperimentControlWidget::processEssDatapoint(const QString &name, const QVariant &value)
{
    // Debug: log all ess/ datapoints we process
    if (!name.startsWith("ess/state") && !name.startsWith("ess/obs")) {
      //        EssConsoleManager::instance()->logInfo(
      //            QString("Processing ESS datapoint: %1").arg(name), 
      //            "ExperimentControl");
    }
    
    if (name == "ess/systems") {
        QString systemsStr = value.toString().trimmed();
        QStringList systems = parseTclList(systemsStr);
        setComboBoxItems(m_systemCombo, systems, m_pendingSystem.isEmpty() ? 
                         currentSystem() : m_pendingSystem);
        m_pendingSystem.clear();
    }
    else if (name == "ess/protocols") {
        QString protocolsStr = value.toString().trimmed();
        QStringList protocols = parseTclList(protocolsStr);
        setComboBoxItems(m_protocolCombo, protocols, m_pendingProtocol.isEmpty() ? 
                         currentProtocol() : m_pendingProtocol);
        m_pendingProtocol.clear();
    }
    else if (name == "ess/variants") {
        QString variantsStr = value.toString().trimmed();
        QStringList variants = parseTclList(variantsStr);
        setComboBoxItems(m_variantCombo, variants, m_pendingVariant.isEmpty() ? 
                         currentVariant() : m_pendingVariant);
        m_pendingVariant.clear();
    }
    else if (name == "ess/system") {
        m_pendingSystem = value.toString();
        setComboBoxValue(m_systemCombo, value.toString());
    }
    else if (name == "ess/protocol") {
        m_pendingProtocol = value.toString();
        setComboBoxValue(m_protocolCombo, value.toString());
    }
    else if (name == "ess/variant") {
        m_pendingVariant = value.toString();
        setComboBoxValue(m_variantCombo, value.toString());
    }
    else if (name == "ess/state") {
        QString state = value.toString();
        m_isRunning = (state == "Running" || state == "RUNNING");
        m_isLoading = (state == "Loading" || state == "LOADING");
        updateButtonStates();
        updateStatusDisplay();
    }
    else if (name == "ess/loading_operation_id") {
        // A loading operation has started
        m_isLoading = true;
        updateButtonStates();
        updateStatusDisplay();
    }
    else if (name == "ess/loading_progress") {
        // Parse JSON progress data
        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(value.toString().toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject obj = doc.object();
            QString stage = obj["stage"].toString();
            QString message = obj["message"].toString();
            int percent = obj["percent"].toInt();
            
            // Update loading state based on stage
            if (stage == "starting") {
                m_isLoading = true;
            } else if (stage == "complete") {
                m_isLoading = false;
                m_loadingProgress.clear();
            }
            
            // Format progress message
            if (percent > 0) {
                m_loadingProgress = QString("%1% - %2").arg(percent).arg(message);
            } else {
                m_loadingProgress = message;
            }
            
            updateButtonStates();
            updateStatusDisplay();
        }
    }
    else if (name == "ess/obs_id") {
        m_currentObsId = value.toInt();
        updateStatusDisplay();
    }
    else if (name == "ess/obs_total") {
        m_totalObs = value.toInt();
        updateStatusDisplay();
    }
    else if (name == "ess/obs_active") {
        m_observationActive = value.toBool();
        updateStatusDisplay();
    }
    else if (name == "ess/param_settings") {
      // Parse Tcl dict format: "name {value type datatype} ..."
      QString data = value.toString();
      
      // Clear existing parameters first
      clearParameterSettings();
      
      // Use our Tcl list parser
      QStringList parts = parseTclList(data);
      for (int i = 0; i < parts.size() - 1; i += 2) {
        QString paramName = parts[i];
        QString paramData = parts[i + 1];
        
        // Parse the value/type/datatype triplet
        QStringList valueTypeParts = parseTclList(paramData);
        if (valueTypeParts.size() >= 3) {
	  QString paramValue = valueTypeParts[0];
	  QString paramType = valueTypeParts[1];  // 1=time, 2=variable
	  QString dataType = valueTypeParts[2];   // int, float, string, bool, ipaddr
          
	  QLineEdit* lineEdit = new QLineEdit();
	  lineEdit->setText(paramValue);
          
	  // Set validator based on datatype
	  if (dataType == "int") {
	    lineEdit->setValidator(new QIntValidator(lineEdit));
	  }
	  else if (dataType == "float") {
	    lineEdit->setValidator(new QDoubleValidator(lineEdit));
	  }
	  else if (dataType == "bool") {
	    // Create a validator that only allows 0 or 1
	    QRegularExpressionValidator* boolValidator = 
	      new QRegularExpressionValidator(QRegularExpression("^[01]$"), lineEdit);
	    lineEdit->setValidator(boolValidator);
	    lineEdit->setToolTip("Enter 0 or 1");
	  }
	  else if (dataType == "ipaddr") {
	    lineEdit->setToolTip("Enter IP address (e.g., 192.168.1.1) or hostname");
	  }
	  // No validator needed for string type
          
	  // Store original value to detect changes
	  lineEdit->setProperty("originalValue", paramValue);
          
	  // Connect for changes - only send when value actually changes
	  connect(lineEdit, &QLineEdit::editingFinished,
		  this, [this, paramName, lineEdit]() {
		    if (m_blockSignals) return;
		    
		    QString original = lineEdit->property("originalValue").toString();
		    if (lineEdit->text() != original) {
		      EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
		      if (cmdInterface && cmdInterface->isConnected()) {
                        QString cmd = QString("::ess::set_param {%1} {%2}")
			  .arg(paramName)
			  .arg(lineEdit->text());
                        cmdInterface->executeEss(cmd);
                        
                        // Update the stored original value
                        lineEdit->setProperty("originalValue", lineEdit->text());
		      }
		    }
		  });
	  
	  // Create a label that shows the parameter type
	  QLabel* label = new QLabel(paramName + ":");
	  
	  // Add to layout
	  m_parameterLayout->addRow(label, lineEdit);
        }
      }
    }
else if (name == "ess/variant_info_json") {
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(value.toString().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        EssConsoleManager::instance()->logError(
            QString("Failed to parse variant_info_json: %1").arg(error.errorString()), 
            "ExperimentControl");
        return;
    }
    
    QJsonObject obj = doc.object();
    QJsonArray loaderArgNames = obj["loader_arg_names"].toArray();
    QJsonArray loaderArgs = obj["loader_args"].toArray();
    QJsonObject options = obj["options"].toObject();
    
    // Always clear and rebuild from scratch
    clearVariantOptions();
    
    // Create combo boxes for each argument that has options
    for (int i = 0; i < loaderArgNames.size(); ++i) {
        QString argName = loaderArgNames[i].toString();
        
        if (options.contains(argName)) {
            QJsonArray argOptions = options[argName].toArray();
            
            QComboBox *combo = new QComboBox();
	    combo->setFixedWidth(90);
	    QStringList displayNames;
            QStringList actualValues;
            
            // Build the option lists
            for (const QJsonValue &optVal : argOptions) {
                QJsonObject option = optVal.toObject();
                displayNames.append(option["label"].toString());
                actualValues.append(option["value"].toString());
            }
            
            combo->addItems(displayNames);
            combo->setProperty("actualValues", actualValues);
            
            // Set current value
            QString currentValue;
            if (i < loaderArgs.size()) {
                QJsonValue argValue = loaderArgs[i];
                if (argValue.isString()) {
                    currentValue = argValue.toString();
                } else if (argValue.isDouble()) {
                    double val = argValue.toDouble();
                    currentValue = (qFloor(val) == val) ? 
                        QString::number(static_cast<int>(val)) : 
                        QString::number(val);
                }
            }
            
            // Set the selection
            if (!currentValue.isEmpty()) {
                QStringList actualValues = combo->property("actualValues").toStringList();
                int index = actualValues.indexOf(currentValue);
                if (index >= 0) {
                    combo->setCurrentIndex(index);
                }
            }
            
            // Connect for changes
// Connect for changes
connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this, argName = argName, combo](int index) {
    // Add debug log to see if this is even called
    EssConsoleManager::instance()->logInfo(
        QString("Combo changed! argName=%1, index=%2, blockSignals=%3")
        .arg(argName).arg(index).arg(m_blockSignals), 
        "ExperimentControl");
    
    if (m_blockSignals || index < 0) return;
    
    QStringList values = combo->property("actualValues").toStringList();
    if (index >= values.size()) {
        EssConsoleManager::instance()->logError(
            QString("Index out of range: %1 >= %2").arg(index).arg(values.size()), 
            "ExperimentControl");
        return;
    }
    
    QString newValue = values[index];
    
    EssConsoleManager::instance()->logInfo(
        QString("Attempting to set %1 to %2").arg(argName).arg(newValue), 
        "ExperimentControl");
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        QString cmd = QString("::ess::set_variant_args {%1 {%2}}")
            .arg(argName)
            .arg(newValue);
        
        EssConsoleManager::instance()->logInfo(
            QString("Executing: %1").arg(cmd), 
            "ExperimentControl");
        
        auto result = cmdInterface->executeEss(cmd);
        if (result.status == EssCommandInterface::StatusSuccess) {
            EssConsoleManager::instance()->logSuccess("Command succeeded, reloading variant", "ExperimentControl");
            // Use async for reload to show progress
            cmdInterface->executeEssAsync("::ess::reload_variant");
        } else {
            EssConsoleManager::instance()->logError(
                QString("Command failed: %1").arg(result.error), 
                "ExperimentControl");
        }
    } else {
        EssConsoleManager::instance()->logError("Not connected!", "ExperimentControl");
    }
});	    
            
            m_variantOptionsLayout->addRow(argName + ":", combo);
        }
    }
}    
}

QStringList EssExperimentControlWidget::parseTclList(const QString &tclList) const
{
    QStringList result;
    QString current;
    int braceLevel = 0;
    bool inBraces = false;
    
    for (int i = 0; i < tclList.length(); ++i) {
        QChar ch = tclList[i];
        
        if (ch == '{') {
            braceLevel++;
            inBraces = true;
            // Don't include the opening brace in the result
        } else if (ch == '}') {
            braceLevel--;
            if (braceLevel == 0) {
                inBraces = false;
                // Add the completed braced item
                if (!current.isEmpty()) {
                    result.append(current.trimmed());
                    current.clear();
                }
            }
        } else if (ch == ' ' && !inBraces) {
            // Space outside braces means end of item
            if (!current.isEmpty()) {
                result.append(current.trimmed());
                current.clear();
            }
        } else {
            current.append(ch);
        }
    }
    
    // Don't forget the last item
    if (!current.isEmpty()) {
        result.append(current.trimmed());
    }
    
    return result;
}

void EssExperimentControlWidget::resetToDisconnectedState()
{
    // Block signals during reset
    m_blockSignals = true;
    
    // Clear all combo boxes
    m_systemCombo->clear();
    m_protocolCombo->clear();
    m_variantCombo->clear();
    
    // Reset state variables
    m_isRunning = false;
    m_isLoading = false;
    m_currentStatus.clear();
    m_loadingProgress.clear();
    m_currentObsId = 0;
    m_totalObs = 0;
    m_observationActive = false;
    
    // Clear pending selections
    m_pendingSystem.clear();
    m_pendingProtocol.clear();
    m_pendingVariant.clear();
    
    // Update display to show disconnected state
    m_statusLabel->setText("Not Connected");
    m_statusLabel->setStyleSheet("QLabel { color: #666; font-weight: bold; }");
    m_obsCountLabel->setText("--/--");
    m_obsIndicator->setStyleSheet("QLabel { color: gray; }");
    m_progressLabel->setVisible(false);
    
    // Disable all controls
    m_startBtn->setEnabled(false);
    m_stopBtn->setEnabled(false);
    m_resetBtn->setEnabled(false);
    m_systemCombo->setEnabled(false);
    m_protocolCombo->setEnabled(false);
    m_variantCombo->setEnabled(false);
    m_reloadSystemBtn->setEnabled(false);
    m_reloadProtocolBtn->setEnabled(false);
    m_reloadVariantBtn->setEnabled(false);
    m_saveSettingsBtn->setEnabled(false);
    m_resetSettingsBtn->setEnabled(false);
    
    // Clear parameters and variant options
    clearParameterSettings();
    clearVariantOptions();
    
    m_blockSignals = false;
}

void EssExperimentControlWidget::setComboBoxValue(QComboBox *combo, const QString &value)
{
    combo->blockSignals(true);
    int index = combo->findText(value);
    if (index >= 0) {
        combo->setCurrentIndex(index);
    }
    combo->blockSignals(false);
}

void EssExperimentControlWidget::setComboBoxItems(QComboBox *combo, const QStringList &items, 
                                                   const QString &currentValue)
{
    combo->blockSignals(true);
    combo->clear();
    combo->addItems(items);
    
    if (!currentValue.isEmpty()) {
        int index = combo->findText(currentValue);
        if (index >= 0) {
            combo->setCurrentIndex(index);
        }
    }
    combo->blockSignals(false);
}

void EssExperimentControlWidget::updateButtonStates()
{
    // During loading, disable all controls except progress indication
    if (m_isLoading) {
        m_startBtn->setEnabled(false);
        m_stopBtn->setEnabled(false);
        m_resetBtn->setEnabled(false);
        m_systemCombo->setEnabled(false);
        m_protocolCombo->setEnabled(false);
        m_variantCombo->setEnabled(false);
        m_reloadSystemBtn->setEnabled(false);
        m_reloadProtocolBtn->setEnabled(false);
        m_reloadVariantBtn->setEnabled(false);
        m_saveSettingsBtn->setEnabled(false);
        m_resetSettingsBtn->setEnabled(false);
        
        // Also disable all parameter inputs
        for (int i = 0; i < m_parameterLayout->rowCount(); ++i) {
            QLayoutItem* item = m_parameterLayout->itemAt(i, QFormLayout::FieldRole);
            if (item && item->widget()) {
                item->widget()->setEnabled(false);
            }
        }
        
        // And disable all variant option combos
        for (int i = 0; i < m_variantOptionsLayout->rowCount(); ++i) {
            QLayoutItem* item = m_variantOptionsLayout->itemAt(i, QFormLayout::FieldRole);
            if (item && item->widget()) {
                item->widget()->setEnabled(false);
            }
        }
    }
    else {
        // Normal state handling
        m_startBtn->setEnabled(!m_isRunning);
        m_stopBtn->setEnabled(m_isRunning);
        m_resetBtn->setEnabled(!m_isRunning);
        
        // Re-enable config controls when not running and not loading
        bool configEnabled = !m_isRunning && !m_isLoading;
        m_systemCombo->setEnabled(configEnabled);
        m_protocolCombo->setEnabled(configEnabled);
        m_variantCombo->setEnabled(configEnabled);
        m_reloadSystemBtn->setEnabled(configEnabled);
        m_reloadProtocolBtn->setEnabled(configEnabled);
        m_reloadVariantBtn->setEnabled(configEnabled);
        m_saveSettingsBtn->setEnabled(configEnabled);
        m_resetSettingsBtn->setEnabled(configEnabled);
        
        // Re-enable all parameter inputs when not running
        for (int i = 0; i < m_parameterLayout->rowCount(); ++i) {
            QLayoutItem* item = m_parameterLayout->itemAt(i, QFormLayout::FieldRole);
            if (item && item->widget()) {
                item->widget()->setEnabled(!m_isRunning);
            }
        }
        
        // Re-enable all variant option combos when not running
        for (int i = 0; i < m_variantOptionsLayout->rowCount(); ++i) {
            QLayoutItem* item = m_variantOptionsLayout->itemAt(i, QFormLayout::FieldRole);
            if (item && item->widget()) {
                item->widget()->setEnabled(!m_isRunning);
            }
        }
    }
}

void EssExperimentControlWidget::updateStatusDisplay()
{
    // Update status label based on state
    if (m_isLoading) {
        m_statusLabel->setText("Loading...");
        m_statusLabel->setStyleSheet("QLabel { color: #f39c12; font-weight: bold; }");  // Orange
        
        // Show progress
        m_progressLabel->setVisible(true);
        m_progressLabel->setText(m_loadingProgress);
    }
    else if (m_isRunning) {
        m_statusLabel->setText("Running");
        m_statusLabel->setStyleSheet("QLabel { color: #28c814; font-weight: bold; }");
        m_progressLabel->setVisible(false);
    } 
    else {
        m_statusLabel->setText("Stopped");
        m_statusLabel->setStyleSheet("QLabel { color: red; font-weight: bold; }");
        m_progressLabel->setVisible(false);
    }
    
    // Update observation count (show 1-based index)
    if (m_totalObs > 0) {
        m_obsCountLabel->setText(QString("%1/%2").arg(m_currentObsId + 1).arg(m_totalObs));
    } else {
        m_obsCountLabel->setText("0/0");
    }
    
    // Update observation indicator
    if (m_observationActive) {
        m_obsIndicator->setStyleSheet("QLabel { color: red; }");
    } else {
        m_obsIndicator->setStyleSheet("QLabel { color: gray; }");
    }
}

QString EssExperimentControlWidget::currentSystem() const
{
    return m_systemCombo->currentText();
}

QString EssExperimentControlWidget::currentProtocol() const
{
    return m_protocolCombo->currentText();
}

QString EssExperimentControlWidget::currentVariant() const
{
    return m_variantCombo->currentText();
}

void EssExperimentControlWidget::onStartClicked()
{
    if (m_blockSignals) return;
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        auto result = cmdInterface->executeEss("::ess::start");
        if (result.status == EssCommandInterface::StatusSuccess) {
            emit experimentStarted();
            EssConsoleManager::instance()->logSuccess("Experiment started", "ExperimentControl");
        } else {
            EssConsoleManager::instance()->logError("Failed to start experiment: " + result.error, 
                                                   "ExperimentControl");
        }
    }
}

void EssExperimentControlWidget::onStopClicked()
{
    if (m_blockSignals) return;
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        auto result = cmdInterface->executeEss("::ess::stop");
        if (result.status == EssCommandInterface::StatusSuccess) {
            emit experimentStopped();
            EssConsoleManager::instance()->logSuccess("Experiment stopped", "ExperimentControl");
        } else {
            EssConsoleManager::instance()->logError("Failed to stop experiment: " + result.error,
                                                   "ExperimentControl");
        }
    }
}

void EssExperimentControlWidget::onResetClicked()
{
    if (m_blockSignals) return;
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        auto result = cmdInterface->executeEss("::ess::reset");
        if (result.status == EssCommandInterface::StatusSuccess) {
            emit experimentReset();
            EssConsoleManager::instance()->logSuccess("Experiment reset", "ExperimentControl");
        } else {
            EssConsoleManager::instance()->logError("Failed to reset experiment: " + result.error,
                                                   "ExperimentControl");
        }
    }
}

void EssExperimentControlWidget::onSystemComboChanged(int index)
{
    if (m_blockSignals || index < 0) return;
    
    QString newSystem = m_systemCombo->itemText(index);
    if (newSystem.isEmpty()) return;
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        // When system changes, we need to reload with the new system
        QString cmd = QString("::ess::load_system {%1}").arg(newSystem);
        
        // Use async execution for loading
        cmdInterface->executeEssAsync(cmd);
        
        // The async call returns immediately
        // Loading state will be tracked via ess/loading_operation_id and ess/loading_progress
        emit systemChanged(newSystem);
        EssConsoleManager::instance()->logInfo(
            QString("Loading system: %1").arg(newSystem), "ExperimentControl");
    }
}

void EssExperimentControlWidget::onProtocolComboChanged(int index)
{
    if (m_blockSignals || index < 0) return;
    
    QString newProtocol = m_protocolCombo->itemText(index);
    if (newProtocol.isEmpty()) return;
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        QString cmd = QString("::ess::load_system {%1} {%2}")
                      .arg(currentSystem())
                      .arg(newProtocol);
        
        // Use async execution for loading
        cmdInterface->executeEssAsync(cmd);
        
        emit protocolChanged(newProtocol);
        EssConsoleManager::instance()->logInfo(
            QString("Loading protocol: %1").arg(newProtocol), "ExperimentControl");
    }
}

void EssExperimentControlWidget::onVariantComboChanged(int index)
{
    if (m_blockSignals || index < 0) return;
    
    QString newVariant = m_variantCombo->itemText(index);
    if (newVariant.isEmpty()) return;
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        QString cmd = QString("::ess::load_system {%1} {%2} {%3}")
                      .arg(currentSystem())
                      .arg(currentProtocol()) 
                      .arg(newVariant);
        
        // Use async execution for loading
        cmdInterface->executeEssAsync(cmd);
        
        emit variantChanged(newVariant);
        EssConsoleManager::instance()->logInfo(
            QString("Loading variant: %1").arg(newVariant), "ExperimentControl");
    }
}

void EssExperimentControlWidget::onReloadSystemClicked()
{
    if (m_blockSignals) return;
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        // Touch the datapoint to force a refresh
        cmdInterface->executeDserv("%touch ess/systems");
        emit reloadSystemRequested();
        EssConsoleManager::instance()->logInfo("Reloading system list", "ExperimentControl");
    }
}

void EssExperimentControlWidget::onReloadProtocolClicked()
{
    if (m_blockSignals) return;
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        cmdInterface->executeDserv("%touch ess/protocols");
        emit reloadProtocolRequested();
        EssConsoleManager::instance()->logInfo("Reloading protocol list", "ExperimentControl");
    }
}

void EssExperimentControlWidget::onReloadVariantClicked()
{
    if (m_blockSignals) return;
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        cmdInterface->executeDserv("%touch ess/variants");
        emit reloadVariantRequested();
        EssConsoleManager::instance()->logInfo("Reloading variant list", "ExperimentControl");
    }
}

void EssExperimentControlWidget::updateParameterSettings(const QJsonObject &params)
{
    // Clear existing parameters
    clearParameterSettings();
    
    EssConsoleManager::instance()->logInfo(
        QString("Updating parameter settings with %1 parameters").arg(params.count()), 
        "ExperimentControl");
    
    if (params.isEmpty()) {
        return;
    }
    
    // Add each parameter
    for (auto it = params.begin(); it != params.end(); ++it) {
        QString name = it.key();
        QJsonValue value = it.value();
        
        QString valueStr;
        QLineEdit *input = new QLineEdit();
        
        // Handle different value types
        if (value.isString()) {
            valueStr = value.toString();
            input->setText(valueStr);
        } else if (value.isDouble()) {
            valueStr = QString::number(value.toDouble());
            input->setText(valueStr);
        } else if (value.isArray()) {
            // For array values (like [value, type, datatype])
            QJsonArray arr = value.toArray();
            if (arr.size() > 0) {
                valueStr = arr[0].toString();
                input->setText(valueStr);
            }
            EssConsoleManager::instance()->logInfo(
                QString("Parameter %1: array with %2 elements").arg(name).arg(arr.size()), 
                "ExperimentControl");
        } else if (value.isObject()) {
            EssConsoleManager::instance()->logWarning(
                QString("Parameter %1: unexpected object type").arg(name), 
                "ExperimentControl");
        }
        
        EssConsoleManager::instance()->logInfo(
            QString("Added parameter %1 = %2").arg(name).arg(valueStr), 
            "ExperimentControl");
        
        // Connect for parameter changes
        connect(input, &QLineEdit::editingFinished, this, [this, name, input]() {
            if (m_blockSignals) return;
            
            EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
            if (cmdInterface && cmdInterface->isConnected()) {
                QString cmd = QString("::ess::set_param {%1} {%2}")
                    .arg(name)
                    .arg(input->text());
                cmdInterface->executeEss(cmd);
            }
        });
        
        m_parameterLayout->addRow(name + ":", input);
    }
}

void EssExperimentControlWidget::updateVariantOptions(const QJsonObject &variantInfo)
{
    // Clear existing options
    clearVariantOptions();
    
    EssConsoleManager::instance()->logInfo(
        QString("Updating variant options, keys: %1").arg(QStringList(variantInfo.keys()).join(", ")), 
        "ExperimentControl");
    
    // Get loader_arg_options and loader_args
    QJsonObject options = variantInfo["loader_arg_options"].toObject();
    QJsonArray args = variantInfo["loader_args"].toArray();
    
    EssConsoleManager::instance()->logInfo(
        QString("Found %1 options and %2 args").arg(options.count()).arg(args.size()), 
        "ExperimentControl");
    
    if (options.isEmpty()) {
        return;
    }
    
    int argIndex = 0;
    for (auto it = options.begin(); it != options.end(); ++it, ++argIndex) {
        QString optionName = it.key();
        QJsonArray optionValues = it.value().toArray();
        
        EssConsoleManager::instance()->logInfo(
            QString("Option %1 has %2 choices").arg(optionName).arg(optionValues.size()), 
            "ExperimentControl");
        
        QComboBox *combo = new QComboBox();
        
        // Add option values to combo
        QStringList displayNames;
        QStringList actualValues;
        
        for (const QJsonValue &val : optionValues) {
            QJsonArray pair = val.toArray();
            if (pair.size() == 2) {
                displayNames.append(pair[0].toString());
                actualValues.append(pair[1].toString());
            }
        }
        
        combo->addItems(displayNames);
        combo->setProperty("actualValues", actualValues);
        
        // Set current value from loader_args
        if (argIndex < args.size()) {
            QString currentValue = args[argIndex].toString();
            int index = actualValues.indexOf(currentValue);
            if (index >= 0) {
                combo->setCurrentIndex(index);
                EssConsoleManager::instance()->logInfo(
                    QString("Set %1 to %2").arg(optionName).arg(displayNames[index]), 
                    "ExperimentControl");
            }
        }
        
        // Connect for option changes
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, optionName, combo](int index) {
            if (m_blockSignals || index < 0) return;
            
            QStringList values = combo->property("actualValues").toStringList();
            if (index >= values.size()) return;
            
            EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
            if (cmdInterface && cmdInterface->isConnected()) {
                QString cmd = QString("::ess::set_variant_args {%1 {%2}}")
                    .arg(optionName)
                    .arg(values[index]);
                cmdInterface->executeEss(cmd);
                
                // Auto-reload variant if enabled using async
                // (you could add a checkbox for this feature)
                cmdInterface->executeEssAsync("::ess::reload_variant");
            }
        });
        
        m_variantOptionsLayout->addRow(optionName + ":", combo);
    }
}

void EssExperimentControlWidget::clearParameterSettings()
{
    // Remove all widgets from parameter layout
    while (m_parameterLayout->count() > 0) {
        QLayoutItem *item = m_parameterLayout->takeAt(0);
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
}

void EssExperimentControlWidget::clearVariantOptions()
{
    // Remove all widgets from variant options layout
    while (m_variantOptionsLayout->count() > 0) {
        QLayoutItem *item = m_variantOptionsLayout->takeAt(0);
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
}

void EssExperimentControlWidget::onSaveSettingsClicked()
{
    if (m_blockSignals) return;
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        auto result = cmdInterface->executeEss("::ess::save_settings");
        if (result.status == EssCommandInterface::StatusSuccess) {
            emit saveSettingsRequested();
            EssConsoleManager::instance()->logSuccess("Settings saved", "ExperimentControl");
        } else {
            EssConsoleManager::instance()->logError("Failed to save settings: " + result.error,
                                                   "ExperimentControl");
        }
    }
}

void EssExperimentControlWidget::onResetSettingsClicked()
{
    if (m_blockSignals) return;
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        auto result = cmdInterface->executeEss("::ess::reset_settings");
        if (result.status == EssCommandInterface::StatusSuccess) {
            emit resetSettingsRequested();
            EssConsoleManager::instance()->logSuccess("Settings reset", "ExperimentControl");
            // Reload variant to refresh parameters using async
            cmdInterface->executeEssAsync("::ess::reload_variant");
        } else {
            EssConsoleManager::instance()->logError("Failed to reset settings: " + result.error,
                                                   "ExperimentControl");
        }
    }
}

// EssEventTableWidget.cpp
#include "EssEventTableWidget.h"
#include "EssApplication.h"
#include "EssCommandInterface.h"
#include "EssDataProcessor.h"
#include "EssEventProcessor.h"
#include "core/EssEvent.h"
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QRegularExpression>
#include <QFont>

EssEventTableWidget::EssEventTableWidget(QWidget *parent)
    : QWidget(parent)
    , m_maxEvents(1000)
    , m_currentObsStart(0)
    , m_eventProcessor(nullptr)
    , m_obsCount(0)
    , m_obsTotal(0)
    , m_currentObsIndex(-1)
{
    setupUi();
    connectToEventProcessor();
}

EssEventTableWidget::~EssEventTableWidget()
{
}

void EssEventTableWidget::setupUi()
{
    // Create main layout
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);  // Reduce margins
    mainLayout->setSpacing(3);  // Reduce spacing
    
    // Status bar
    QHBoxLayout *statusLayout = new QHBoxLayout();
    statusLayout->setSpacing(10);
    
    m_statusLabel = new QLabel("System: Stopped", this);
    m_statusLabel->setStyleSheet("QLabel { font-weight: bold; }");
    
    m_obsLabel = new QLabel("No observation", this);
    
    // Navigation controls
    m_prevObsButton = new QPushButton("<", this);
    m_prevObsButton->setFixedWidth(30);
    m_prevObsButton->setEnabled(false);
    connect(m_prevObsButton, &QPushButton::clicked, [this]() {
        if (m_currentObsIndex > 0) {
            showObservation(m_currentObsIndex - 1);
        }
    });
    
    m_obsNavigationLabel = new QLabel("", this);
    
    m_nextObsButton = new QPushButton(">", this);
    m_nextObsButton->setFixedWidth(30);
    m_nextObsButton->setEnabled(false);
    connect(m_nextObsButton, &QPushButton::clicked, [this]() {
        if (m_currentObsIndex < m_observationHistory.size() - 1) {
            showObservation(m_currentObsIndex + 1);
        }
    });
    
    m_clearButton = new QPushButton("Clear All", this);
    connect(m_clearButton, &QPushButton::clicked, this, &EssEventTableWidget::onClearClicked);
    
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addWidget(m_obsLabel);
    statusLayout->addStretch();
    statusLayout->addWidget(m_prevObsButton);
    statusLayout->addWidget(m_obsNavigationLabel);
    statusLayout->addWidget(m_nextObsButton);
    statusLayout->addSpacing(10);
    statusLayout->addWidget(m_clearButton);
    
    mainLayout->addLayout(statusLayout);
    
    // Event table with compact settings
    m_tableWidget = new QTableWidget(0, 5, this);
    QStringList headers = {"Time", "Δt", "Type", "Subtype", "Parameters"};
    m_tableWidget->setHorizontalHeaderLabels(headers);
    m_tableWidget->setAlternatingRowColors(true);
    m_tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableWidget->setSortingEnabled(false);
    m_tableWidget->verticalHeader()->setVisible(false);
    
    // Make the table more compact
    QFont tableFont = m_tableWidget->font();
    tableFont.setPointSize(tableFont.pointSize() - 1);  // Smaller font
    m_tableWidget->setFont(tableFont);
    
    // Reduce row height
    m_tableWidget->verticalHeader()->setDefaultSectionSize(20);  // Smaller row height
    m_tableWidget->verticalHeader()->setMinimumSectionSize(16);
    
    // Compact header
    QHeaderView *vHeader = m_tableWidget->verticalHeader();
    vHeader->setSectionResizeMode(QHeaderView::Fixed);
    
    // Column sizing
    QHeaderView *header = m_tableWidget->horizontalHeader();
    header->setDefaultSectionSize(60);  // Default smaller width
    header->setSectionResizeMode(0, QHeaderView::Fixed);  // Time
    header->setSectionResizeMode(1, QHeaderView::Fixed);  // Elapsed
    header->setSectionResizeMode(2, QHeaderView::Interactive);  // Type
    header->setSectionResizeMode(3, QHeaderView::Interactive);  // Subtype
    header->setSectionResizeMode(4, QHeaderView::Stretch);  // Parameters
    
    // Set specific column widths
    m_tableWidget->setColumnWidth(0, 60);  // Time
    m_tableWidget->setColumnWidth(1, 50);  // Elapsed
    m_tableWidget->setColumnWidth(2, 100); // Type
    m_tableWidget->setColumnWidth(3, 100); // Subtype
    
    mainLayout->addWidget(m_tableWidget);
    
    setLayout(mainLayout);
    resize(700, 400);
}

void EssEventTableWidget::connectToEventProcessor()
{
    // Get event processor through data processor
    if (EssApplication::instance() && EssApplication::instance()->dataProcessor()) {
        EssDataProcessor *dataProc = EssApplication::instance()->dataProcessor();
        m_eventProcessor = dataProc->eventProcessor();
        
        if (m_eventProcessor) {
            connect(m_eventProcessor, &EssEventProcessor::eventReceived,
                    this, &EssEventTableWidget::onEventReceived);
            connect(m_eventProcessor, &EssEventProcessor::systemStateChanged,
                    this, &EssEventTableWidget::onSystemStateChanged);
            connect(dataProc, &EssDataProcessor::experimentStateChanged,
                    this, &EssEventTableWidget::onExperimentStateChanged);
            connect(m_eventProcessor, &EssEventProcessor::observationStarted,
                    this, &EssEventTableWidget::onObservationStarted);
            connect(m_eventProcessor, &EssEventProcessor::observationReset,
                    this, &EssEventTableWidget::onObservationReset);
        }
    }
    
    // Connect to command interface
    if (EssApplication::instance()) {
        auto* cmdInterface = EssApplication::instance()->commandInterface();
        if (cmdInterface) {
            connect(cmdInterface, &EssCommandInterface::connected,
                    this, &EssEventTableWidget::onHostConnected);
            connect(cmdInterface, &EssCommandInterface::disconnected,
                    this, &EssEventTableWidget::onHostDisconnected);
        }
    }
}

void EssEventTableWidget::onHostConnected(const QString &host)
{
    // Clear all observation history
    m_observationHistory.clear();
    m_currentObsIndex = -1;
    clearEvents();
    
    // Reset observation state
    m_currentObsStart = 0;
    m_obsLabel->setText("No observation");
    updateNavigationControls();
    
    // Update system state display
    m_statusLabel->setText("System: Stopped");
    m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: red; }");
}

void EssEventTableWidget::onHostDisconnected()
{
    // Clear all events and history
    m_observationHistory.clear();
    m_currentObsIndex = -1;
    clearEvents();
    
    // Reset observation state
    m_currentObsStart = 0;
    m_obsLabel->setText("No observation");
    updateNavigationControls();
    
    // Reset system state display
    m_statusLabel->setText("System: Disconnected");
    m_statusLabel->setStyleSheet("QLabel { font-weight: bold; }");
}

bool EssEventTableWidget::shouldDisplayEvent(const EssEvent &event) const
{
    switch (event.type) {
        case EVT_USER:
        case EVT_NAMESET:
        case EVT_PARAM:
        case EVT_FILEIO:
        case EVT_SYSTEM_CHANGES:
        case EVT_SUBTYPE_NAMES:
            return false;
        default:
            return true;
    }
}

void EssEventTableWidget::onEventReceived(const EssEvent &event)
{
    if (event.type == EVT_SYSTEM_CHANGES) {
        // Clear all history on system changes
        m_observationHistory.clear();
        m_currentObsIndex = -1;
        clearEvents();
        updateNavigationControls();
        return;
    }

    // Handle EVT_BEGINOBS to extract observation count and total
    if (event.type == EVT_BEGINOBS) {
        extractObservationParams(event);
    }
    
    // Store event if we're in an active observation
    if (m_currentObsIndex >= 0 && m_currentObsIndex < m_observationHistory.size()) {
        m_observationHistory[m_currentObsIndex].events.append(event);
    }
    
    // Display event if we should and we're viewing the current observation
    if (shouldDisplayEvent(event) && m_currentObsIndex == m_observationHistory.size() - 1) {
        addEventRow(event);
    }
}

void EssEventTableWidget::extractObservationParams(const EssEvent &event)
{
    m_obsCount = 0;
    m_obsTotal = 0;
    
    if (event.params.isArray()) {
        QJsonArray array = event.params.toArray();
        if (array.size() >= 2) {
            if (array[0].isDouble()) {
                m_obsCount = static_cast<int>(array[0].toDouble());
            }
            if (array[1].isDouble()) {
                m_obsTotal = static_cast<int>(array[1].toDouble());
            }
        }
    }
    else if (event.params.isObject()) {
        QJsonObject obj = event.params.toObject();
        if (obj.contains("count") && obj["count"].isDouble()) {
            m_obsCount = static_cast<int>(obj["count"].toDouble());
        }
        if (obj.contains("total") && obj["total"].isDouble()) {
            m_obsTotal = static_cast<int>(obj["total"].toDouble());
        }
    }
    else if (event.params.isString()) {
        QString paramStr = event.params.toString();
        QStringList parts = paramStr.split(',');
        if (parts.size() >= 2) {
            bool ok;
            m_obsCount = parts[0].trimmed().toInt(&ok);
            if (!ok) m_obsCount = 0;
            
            m_obsTotal = parts[1].trimmed().toInt(&ok);
            if (!ok) m_obsTotal = 0;
        }
    }
    
    updateObservationLabel();
}

void EssEventTableWidget::updateObservationLabel()
{
    if (m_obsTotal > 0) {
        m_obsLabel->setText(QString("[Obs %1/%2]").arg(m_obsCount+1).arg(m_obsTotal));
    }
}

QString EssEventTableWidget::formatEventParams(const EssEvent &event) const
{
    QString paramStr = event.paramsAsString();
    
    // Check for empty parameters
    if (paramStr.isEmpty() || 
        paramStr == "[]" || 
        paramStr == "{}" || 
        paramStr == "null" ||
        paramStr == "\"\"") {
        return "";
    }
    
    // Format single numbers
    if (event.params.isDouble()) {
        double value = event.params.toDouble();
        if (value == floor(value)) {
            return QString::number(static_cast<int>(value));
        } else {
            QString formatted = QString::number(value, 'f', 3);
            formatted.remove(QRegularExpression("\\.?0+$"));
            return formatted;
        }
    }
    
    // Format arrays compactly
    if (event.params.isArray()) {
        QJsonArray array = event.params.toArray();
        QStringList formattedParts;
        
        for (const QJsonValue &val : array) {
            if (val.isDouble()) {
                double value = val.toDouble();
                if (value == floor(value)) {
                    formattedParts.append(QString::number(static_cast<int>(value)));
                } else {
                    QString formatted = QString::number(value, 'f', 2);  // Less precision for compactness
                    formatted.remove(QRegularExpression("\\.?0+$"));
                    formattedParts.append(formatted);
                }
            } else if (val.isString()) {
                formattedParts.append(val.toString());
            } else {
                formattedParts.append(QJsonDocument(QJsonArray{val}).toJson(QJsonDocument::Compact));
            }
        }
        
        return formattedParts.join(",");  // No spaces for compactness
    }
    
    return paramStr;
}

void EssEventTableWidget::addEventRow(const EssEvent &event)
{
    int row = m_tableWidget->rowCount();
    m_tableWidget->insertRow(row);
    
    uint64_t currentTimestamp = event.timestamp;
    
    // Timestamp - more compact format
    QString timeStr;
    if (m_currentObsStart > 0 && event.timestamp >= m_currentObsStart) {
        uint64_t relativeTime = event.timestamp - m_currentObsStart;
        timeStr = QString::number(relativeTime / 1000);  // ms
    } else if (event.type == EVT_BEGINOBS) {
        timeStr = "0";
    } else {
        timeStr = QString::number(event.timestamp / 1000000);  // seconds
    }
    
    QTableWidgetItem *timeItem = new QTableWidgetItem(timeStr);
    timeItem->setFlags(timeItem->flags() & ~Qt::ItemIsEditable);
    timeItem->setData(Qt::UserRole, QVariant::fromValue(currentTimestamp));
    m_tableWidget->setItem(row, 0, timeItem);
    
    // Elapsed time - compact format
    QString elapsedStr = "";
    if (row > 0) {
        QTableWidgetItem *prevTimeItem = m_tableWidget->item(row - 1, 0);
        if (prevTimeItem) {
            uint64_t prevTimestamp = prevTimeItem->data(Qt::UserRole).toULongLong();
            if (prevTimestamp > 0 && currentTimestamp > prevTimestamp) {
                uint64_t elapsed = currentTimestamp - prevTimestamp;
                if (elapsed < 1000000) {  // Less than 1 second
                    elapsedStr = QString::number(elapsed / 1000.0, 'f', 1);
                } else {
                    elapsedStr = QString::number(elapsed / 1000);
                }
            }
        }
    }
    
    QTableWidgetItem *elapsedItem = new QTableWidgetItem(elapsedStr);
    elapsedItem->setFlags(elapsedItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 1, elapsedItem);
    
    // Type
    QString typeName;
    if (m_eventProcessor) {
        typeName = m_eventProcessor->getEventTypeName(event.type);
    } else {
        typeName = QString("Type_%1").arg(event.type);
    }
    QTableWidgetItem *typeItem = new QTableWidgetItem(typeName);
    typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 2, typeItem);
    
    // Subtype
    QString subtypeName;
    if (m_eventProcessor) {
        subtypeName = m_eventProcessor->getEventSubtypeName(event.type, event.subtype);
    } else {
        subtypeName = QString::number(event.subtype);
    }
    QTableWidgetItem *subtypeItem = new QTableWidgetItem(subtypeName);
    subtypeItem->setFlags(subtypeItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 3, subtypeItem);
    
    // Parameters
    QString formattedParams = formatEventParams(event);
    QTableWidgetItem *paramsItem = new QTableWidgetItem(formattedParams);
    paramsItem->setFlags(paramsItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 4, paramsItem);
    
    // Limit rows
    while (m_tableWidget->rowCount() > m_maxEvents) {
        m_tableWidget->removeRow(0);
    }
    
    m_tableWidget->scrollToBottom();
}

void EssEventTableWidget::onSystemStateChanged(SystemState state)
{
    m_statusLabel->setText(QString("System: %1")
        .arg(state == SYSTEM_RUNNING ? "Running" : "Stopped"));
    
    if (state == SYSTEM_RUNNING) {
        m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: green; }");
    } else {
        m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: red; }");
        if (m_currentObsStart > 0) {
            m_currentObsStart = 0;
            m_obsLabel->setText("");
        }
    }
}

void EssEventTableWidget::onExperimentStateChanged(const QString &newstate)
{
    m_statusLabel->setText(QString("System: %1").arg(newstate));

    if (newstate == "Stopped") {
        m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: red; }");
        m_obsLabel->setText("");
    }
}

void EssEventTableWidget::onObservationStarted(uint64_t timestamp)
{
    m_currentObsStart = timestamp;
    
    // Create new observation data
    ObservationData newObs;
    newObs.startTime = timestamp;
    newObs.obsCount = m_obsCount;
    newObs.obsTotal = m_obsTotal;
    
    m_observationHistory.append(newObs);
    m_currentObsIndex = m_observationHistory.size() - 1;
    
    clearEvents();  // Clear display for new observation
    updateNavigationControls();
}

void EssEventTableWidget::onObservationReset()
{
    m_currentObsStart = 0;
    // Don't clear history, just stop recording
}

void EssEventTableWidget::onClearClicked()
{
    m_observationHistory.clear();
    m_currentObsIndex = -1;
    clearEvents();
    updateNavigationControls();
}

void EssEventTableWidget::clearEvents()
{
    m_tableWidget->setRowCount(0);
}

void EssEventTableWidget::showObservation(int index)
{
    if (index < 0 || index >= m_observationHistory.size()) {
        return;
    }
    
    m_currentObsIndex = index;
    clearEvents();
    
    // Display all events for this observation
    const ObservationData &obs = m_observationHistory[index];
    m_currentObsStart = obs.startTime;
    m_obsCount = obs.obsCount;
    m_obsTotal = obs.obsTotal;
    
    updateObservationLabel();
    
    for (const EssEvent &event : obs.events) {
        if (shouldDisplayEvent(event)) {
            addEventRow(event);
        }
    }
    
    updateNavigationControls();
}

void EssEventTableWidget::updateNavigationControls()
{
    m_prevObsButton->setEnabled(m_currentObsIndex > 0);
    m_nextObsButton->setEnabled(m_currentObsIndex < m_observationHistory.size() - 1);
    
    if (m_observationHistory.isEmpty()) {
        m_obsNavigationLabel->setText("");
    } else {
        m_obsNavigationLabel->setText(QString("%1/%2")
            .arg(m_currentObsIndex + 1)
            .arg(m_observationHistory.size()));
    }
}

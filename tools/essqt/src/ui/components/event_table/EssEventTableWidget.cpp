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


// In EssEventTableWidget constructor, add instance tracking:
EssEventTableWidget::EssEventTableWidget(QWidget *parent)
    : QWidget(parent)
    , m_maxEvents(1000)
    , m_currentObsStart(0)
    , m_eventProcessor(nullptr)
{
    static int instanceCount = 0;
    instanceCount++;
    
    setupUi();
    connectToEventProcessor();
}

// Also add to destructor:
EssEventTableWidget::~EssEventTableWidget()
{
    static int destroyCount = 0;
    destroyCount++;
}

void EssEventTableWidget::setupUi()
{
    // Create main layout directly (no central widget needed for QWidget)
    QVBoxLayout *mainLayout = new QVBoxLayout(this);  // Pass 'this' as parent
    
    // Status bar
    QHBoxLayout *statusLayout = new QHBoxLayout();
    
    m_statusLabel = new QLabel("System: Stopped", this);
    m_statusLabel->setStyleSheet("QLabel { font-weight: bold; }");
    
    m_obsLabel = new QLabel("No observation", this);
    
    m_clearButton = new QPushButton("Clear", this);
    connect(m_clearButton, &QPushButton::clicked, this, &EssEventTableWidget::onClearClicked);
    
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addWidget(m_obsLabel);
    statusLayout->addStretch();
    statusLayout->addWidget(m_clearButton);
    
    mainLayout->addLayout(statusLayout);
    
    // Event table - NOW WITH 5 COLUMNS
    m_tableWidget = new QTableWidget(0, 5, this);
    QStringList headers = {"Timestamp", "Elapsed", "Type", "Subtype", "Parameters"};
    m_tableWidget->setHorizontalHeaderLabels(headers);
    m_tableWidget->setAlternatingRowColors(true);
    m_tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableWidget->setSortingEnabled(false);
    m_tableWidget->verticalHeader()->setVisible(false);
    
    // Column sizing
    QHeaderView *header = m_tableWidget->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents); // Timestamp
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents); // Elapsed
    header->setSectionResizeMode(2, QHeaderView::Interactive);      // Type
    header->setSectionResizeMode(3, QHeaderView::Interactive);      // Subtype
    header->setSectionResizeMode(4, QHeaderView::Stretch);          // Parameters
    
    mainLayout->addWidget(m_tableWidget);
    
    setLayout(mainLayout);  // For QWidget, use setLayout instead of setWidget
    
    // Set reasonable default size
    resize(700, 400);  // Slightly wider for the extra column
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
            connect(m_eventProcessor, &EssEventProcessor::observationStarted,
                    this, &EssEventTableWidget::onObservationStarted);
            connect(m_eventProcessor, &EssEventProcessor::observationReset,
                    this, &EssEventTableWidget::onObservationReset);
        } else {
            qDebug() << "ERROR: Event processor is null!";
        }
    } else {
        qDebug() << "ERROR: Cannot access data processor!";
    }
    
    // Connect to command interface for disconnect notifications
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
    // Clear any old data
    clearEvents();
    
    // Reset observation state
    m_currentObsStart = 0;
    m_obsLabel->setText("No observation");
    
    // Update system state display to show we're connected but system is stopped
    m_statusLabel->setText("System: Stopped");
    m_statusLabel->setStyleSheet("QLabel { font-weight: bold; color: red; }");
}

void EssEventTableWidget::onHostDisconnected()
{
    // Clear all events
    clearEvents();
    
    // Reset observation state
    m_currentObsStart = 0;
    m_obsLabel->setText("No observation");
    
    // Reset system state display
    m_statusLabel->setText("System: Disconnected");
    m_statusLabel->setStyleSheet("QLabel { font-weight: bold; }");
    
    // Note: We don't need to disconnect signals here because the event processor
    // will stop sending events when disconnected
}

bool EssEventTableWidget::shouldDisplayEvent(const EssEvent &event) const
{
    // Filter out events we don't want to display
    switch (event.type) {
        case EVT_USER:           // Type 3 - USER events (START/STOP/RESET)      
        case EVT_NAMESET:        // Type 17 - Event naming
		case EVT_PARAM:
		case EVT_FILEIO:
		case EVT_SYSTEM_CHANGES:
        case EVT_SUBTYPE_NAMES:  // Type 18 - Subtype naming
            return false;
            
        // Add more cases here as needed:
        // case EVT_SOME_OTHER_TYPE:
        //     return false;
            
        default:
            return true;  // Display all other events
    }
}

void EssEventTableWidget::onEventReceived(const EssEvent &event)
{
    if (event.type == EVT_SYSTEM_CHANGES) {
        clearEvents();
    }
    
    // Check if we should display this event
    if (shouldDisplayEvent(event)) {
        addEventRow(event);
    }
}
QString EssEventTableWidget::formatEventParams(const EssEvent &event) const
{
    // Get the default string representation first
    QString paramStr = event.paramsAsString();
    
    // Check for empty parameters - common patterns
    if (paramStr.isEmpty() || 
        paramStr == "[]" || 
        paramStr == "{}" || 
        paramStr == "null" ||
        paramStr == "\"\"") {
        return "";  // Return empty string for cleaner display
    }
    
    // If params is a single number, format it nicely
    if (event.params.isDouble()) {
        double value = event.params.toDouble();
        // Check if it's effectively an integer
        if (value == floor(value)) {
            return QString::number(static_cast<int>(value));
        } else {
            // Format with up to 3 decimal places, removing trailing zeros
            QString formatted = QString::number(value, 'f', 3);
            // Remove trailing zeros and decimal point if not needed
            formatted.remove(QRegularExpression("\\.?0+$"));
            return formatted;
        }
    }
    
    // If params is an array, format each element
    if (event.params.isArray()) {
        QJsonArray array = event.params.toArray();
        QStringList formattedParts;
        
        for (const QJsonValue &val : array) {
            if (val.isDouble()) {
                double value = val.toDouble();
                if (value == floor(value)) {
                    formattedParts.append(QString::number(static_cast<int>(value)));
                } else {
                    QString formatted = QString::number(value, 'f', 3);
                    formatted.remove(QRegularExpression("\\.?0+$"));
                    formattedParts.append(formatted);
                }
            } else if (val.isString()) {
                formattedParts.append(val.toString());
            } else {
                // For other types, use the JSON representation
                formattedParts.append(QJsonDocument(QJsonArray{val}).toJson(QJsonDocument::Compact));
            }
        }
        
        return "[" + formattedParts.join(", ") + "]";
    }
    
    // Check if it's a comma-separated list of numbers (keeping for backward compatibility)
    if (paramStr.contains(',') && !paramStr.startsWith('[')) {
        QStringList parts = paramStr.split(',');
        QStringList formattedParts;
        bool allNumbers = true;
        
        for (const QString &part : parts) {
            QString trimmed = part.trimmed();
            bool ok;
            double value = trimmed.toDouble(&ok);
            
            if (ok) {
                // Format this number
                if (value == floor(value)) {
                    formattedParts.append(QString::number(static_cast<int>(value)));
                } else {
                    QString formatted = QString::number(value, 'f', 3);
                    formatted.remove(QRegularExpression("\\.?0+$"));
                    formattedParts.append(formatted);
                }
            } else {
                // Not a number, keep original
                formattedParts.append(trimmed);
                allNumbers = false;
            }
        }
        
        // If we processed at least some numbers, return the formatted version
        if (allNumbers || formattedParts.size() == parts.size()) {
            return formattedParts.join(", ");
        }
    }
    
    // For all other cases, return the string representation
    return paramStr;
}

void EssEventTableWidget::addEventRow(const EssEvent &event)
{
    int row = m_tableWidget->rowCount();
    m_tableWidget->insertRow(row);
    
    // Store the current event timestamp for next elapsed calculation
    uint64_t currentTimestamp = event.timestamp;
    
    // Timestamp handling
    QString timeStr;
    if (m_currentObsStart > 0 && event.timestamp >= m_currentObsStart) {
        // During observation: show relative time in milliseconds
        uint64_t relativeTime = event.timestamp - m_currentObsStart;
        timeStr = QString::number(relativeTime / 1000); // Convert to ms
    } else if (event.type == EVT_BEGINOBS) {
        // For observation start, just show "0"
        timeStr = "0";
    } else {
        // Outside observation: show absolute time in seconds
        timeStr = QString::number(event.timestamp / 1000000); // Convert microseconds to seconds
    }
    
    QTableWidgetItem *timeItem = new QTableWidgetItem(timeStr);
    timeItem->setFlags(timeItem->flags() & ~Qt::ItemIsEditable);
    timeItem->setData(Qt::UserRole, QVariant::fromValue(currentTimestamp)); // Store raw timestamp
    m_tableWidget->setItem(row, 0, timeItem);
    
    // Elapsed time since previous event
    QString elapsedStr = "";
    if (row > 0) {
        // Get previous event's timestamp
        QTableWidgetItem *prevTimeItem = m_tableWidget->item(row - 1, 0);
        if (prevTimeItem) {
            uint64_t prevTimestamp = prevTimeItem->data(Qt::UserRole).toULongLong();
            if (prevTimestamp > 0 && currentTimestamp > prevTimestamp) {
                uint64_t elapsed = currentTimestamp - prevTimestamp;
                // Show in ms with 1 decimal place if < 1000ms, otherwise whole ms
                if (elapsed < 1000000) { // Less than 1 second
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
    
    // Type - get the actual name from event processor
    QString typeName;
    if (m_eventProcessor) {
        typeName = m_eventProcessor->getEventTypeName(event.type);
    } else {
        typeName = QString("Type_%1").arg(event.type);
    }
    QTableWidgetItem *typeItem = new QTableWidgetItem(typeName);
    typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 2, typeItem);
    
    // Subtype - get the actual name from event processor
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
    
    // Auto-scroll to bottom
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

void EssEventTableWidget::onObservationStarted(uint64_t timestamp)
{
    m_currentObsStart = timestamp;
    // Just show "Observation started" without the confusing timestamp
    m_obsLabel->setText("Observation period in progress");
    clearEvents(); // Clear table for new observation
}

void EssEventTableWidget::onObservationReset()
{
    m_currentObsStart = 0;
    m_obsLabel->setText("Observation period ended");
    clearEvents();
}

void EssEventTableWidget::onClearClicked()
{
    clearEvents();
}

void EssEventTableWidget::clearEvents()
{
    m_tableWidget->setRowCount(0);
}
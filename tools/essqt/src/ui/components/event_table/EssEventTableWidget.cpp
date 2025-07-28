// EssEventTableWidget.cpp
#include "EssEventTableWidget.h"
#include "EssApplication.h"
#include "EssDataProcessor.h"
#include "EssEventProcessor.h"
#include "core/EssEvent.h"
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>


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
    
    // Event table
    m_tableWidget = new QTableWidget(0, 4, this);
    QStringList headers = {"Timestamp", "Type", "Subtype", "Parameters"};
    m_tableWidget->setHorizontalHeaderLabels(headers);
    m_tableWidget->setAlternatingRowColors(true);
    m_tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableWidget->setSortingEnabled(false);
    m_tableWidget->verticalHeader()->setVisible(false);
    
    // Column sizing
    QHeaderView *header = m_tableWidget->horizontalHeader();
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents); // Timestamp
    header->setSectionResizeMode(1, QHeaderView::Interactive);      // Type
    header->setSectionResizeMode(2, QHeaderView::Interactive);      // Subtype
    header->setSectionResizeMode(3, QHeaderView::Stretch);          // Parameters
    
    mainLayout->addWidget(m_tableWidget);
    
    setLayout(mainLayout);  // For QWidget, use setLayout instead of setWidget
    
    // Set reasonable default size
    resize(600, 400);
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
}

void EssEventTableWidget::onEventReceived(const EssEvent &event)
{
    addEventRow(event);
}

void EssEventTableWidget::addEventRow(const EssEvent &event)
{
    int row = m_tableWidget->rowCount();
    m_tableWidget->insertRow(row);
    
    // Timestamp handling
    QString timeStr;
    if (m_currentObsStart > 0 && event.timestamp >= m_currentObsStart) {
        // During observation: show relative time in milliseconds
        uint64_t relativeTime = event.timestamp - m_currentObsStart;
        timeStr = QString::number(relativeTime / 1000); // Convert to ms
    } else if (event.type == EVT_BEGINOBS) {
        // For observation start, just show "0" instead of huge number
        timeStr = "0";
    } else {
        // Outside observation: show absolute time in seconds
        timeStr = QString::number(event.timestamp / 1000000); // Convert microseconds to seconds
    }
    
    QTableWidgetItem *timeItem = new QTableWidgetItem(timeStr);
    timeItem->setFlags(timeItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 0, timeItem);
    
    // Type - get the actual name from event processor
    QString typeName;
    if (m_eventProcessor) {
        typeName = m_eventProcessor->getEventTypeName(event.type);
    } else {
        typeName = QString("Type_%1").arg(event.type);
    }
    QTableWidgetItem *typeItem = new QTableWidgetItem(typeName);
    typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 1, typeItem);
    
    // Subtype - get the actual name from event processor
    QString subtypeName;
    if (m_eventProcessor) {
        subtypeName = m_eventProcessor->getEventSubtypeName(event.type, event.subtype);
    } else {
        subtypeName = QString::number(event.subtype);
    }
    QTableWidgetItem *subtypeItem = new QTableWidgetItem(subtypeName);
    subtypeItem->setFlags(subtypeItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 2, subtypeItem);
    
    // Parameters
    QTableWidgetItem *paramsItem = new QTableWidgetItem(event.paramsAsString());
    paramsItem->setFlags(paramsItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 3, paramsItem);
    
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
    }
}

void EssEventTableWidget::onObservationStarted(uint64_t timestamp)
{
    m_currentObsStart = timestamp;
    m_obsLabel->setText(QString("Observation started at %1").arg(timestamp));
    clearEvents(); // Clear table for new observation
}

void EssEventTableWidget::onObservationReset()
{
    m_currentObsStart = 0;
    m_obsLabel->setText("No observation");
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

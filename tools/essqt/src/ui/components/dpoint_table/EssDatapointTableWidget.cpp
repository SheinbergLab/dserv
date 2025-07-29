// EssDatapointTableWidget.cpp
#include "EssDatapointTableWidget.h"
#include "EssApplication.h"
#include "EssDataProcessor.h"
#include "EssCommandInterface.h"
#include <QTableWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QHeaderView>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

EssDatapointTableWidget::EssDatapointTableWidget(QWidget *parent)
    : QWidget(parent)
    , m_paused(true)
    , m_autoScroll(true)
    , m_maxRows(1000)
    , m_updateCounter(0)
{
    setupUi();
    connectSignals();
}

EssDatapointTableWidget::~EssDatapointTableWidget()
{
// Ensure we're disconnected before destruction
    if (!m_paused && EssApplication::instance() && EssApplication::instance()->dataProcessor()) {
        disconnect(EssApplication::instance()->dataProcessor(), 
                  &EssDataProcessor::genericDatapointReceived,
                  this, &EssDatapointTableWidget::onGenericDatapointReceived);
    }
}

void EssDatapointTableWidget::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    
    // Control bar
    QHBoxLayout *controlLayout = new QHBoxLayout();
    
    // Filter
    QLabel *filterLabel = new QLabel("Filter:", this);
    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText("e.g., ess/.* or ain/eye_.*");
    m_filterEdit->setToolTip("Regular expression to filter datapoint names");
    
    // Pause button
    m_pauseButton = new QPushButton("Start", this);
    m_pauseButton->setCheckable(true);
    m_pauseButton->setChecked(true); 
    m_pauseButton->setToolTip("Pause/resume datapoint updates");
    
    // Clear button
    m_clearButton = new QPushButton("Clear", this);
    m_clearButton->setToolTip("Clear all entries from the table");
    
    // Auto-scroll checkbox
    m_autoScrollCheck = new QCheckBox("Auto-scroll", this);
    m_autoScrollCheck->setChecked(m_autoScroll);
    m_autoScrollCheck->setToolTip("Automatically scroll to show new entries");
    
    // Max rows
    QLabel *maxRowsLabel = new QLabel("Max rows:", this);
    m_maxRowsSpinBox = new QSpinBox(this);
    m_maxRowsSpinBox->setRange(100, 10000);
    m_maxRowsSpinBox->setSingleStep(100);
    m_maxRowsSpinBox->setValue(m_maxRows);
    m_maxRowsSpinBox->setToolTip("Maximum number of rows to display");
    
    controlLayout->addWidget(filterLabel);
    controlLayout->addWidget(m_filterEdit, 1);
    controlLayout->addWidget(m_pauseButton);
    controlLayout->addWidget(m_clearButton);
    controlLayout->addWidget(m_autoScrollCheck);
    controlLayout->addWidget(maxRowsLabel);
    controlLayout->addWidget(m_maxRowsSpinBox);
    controlLayout->addStretch();
    
    mainLayout->addLayout(controlLayout);
    
    // Table widget
    m_tableWidget = new QTableWidget(0, 4, this);
    QStringList headers = {"Timestamp", "Name", "Type", "Value"};
    m_tableWidget->setHorizontalHeaderLabels(headers);
    m_tableWidget->setAlternatingRowColors(true);
    m_tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableWidget->setSortingEnabled(false); // Disable for performance
    m_tableWidget->verticalHeader()->setVisible(false);
    
    // Column sizing
    QHeaderView *header = m_tableWidget->horizontalHeader();
    header->setStretchLastSection(true);
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents); // Timestamp
    header->setSectionResizeMode(1, QHeaderView::Interactive); // Name
    header->setSectionResizeMode(2, QHeaderView::ResizeToContents); // Type
    header->setSectionResizeMode(3, QHeaderView::Stretch); // Value
    
    mainLayout->addWidget(m_tableWidget);
    
    setLayout(mainLayout);

    // Set initial size
    resize(800, 400);
}

void EssDatapointTableWidget::connectSignals()
{   
    if (EssApplication::instance()) {
        auto* cmdInterface = EssApplication::instance()->commandInterface();
        if (cmdInterface) {
            connect(cmdInterface, &EssCommandInterface::disconnected,
                    this, &EssDatapointTableWidget::onHostDisconnected);
        }
    }
    
    // UI connections
    connect(m_pauseButton, &QPushButton::toggled, 
            this, &EssDatapointTableWidget::onPauseToggled);
    connect(m_clearButton, &QPushButton::clicked, 
            this, &EssDatapointTableWidget::onClearClicked);
    connect(m_filterEdit, &QLineEdit::textChanged, 
            this, &EssDatapointTableWidget::onFilterChanged);
    connect(m_maxRowsSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &EssDatapointTableWidget::onMaxRowsChanged);
    connect(m_autoScrollCheck, &QCheckBox::toggled,
            this, &EssDatapointTableWidget::onAutoScrollToggled);
}

void EssDatapointTableWidget::onGenericDatapointReceived(const QString &name, 
                                                         const QVariant &value, 
                                                         qint64 timestamp)
{
    if (m_paused) {
        return;
    }
    
    if (!matchesFilter(name)) {
        return;
    }
    
    addDatapointRow(name, value, timestamp);
}

void EssDatapointTableWidget::addDatapointRow(const QString &name, 
                                              const QVariant &value, 
                                              qint64 timestamp)
{
    // Batch updates for performance
    if (++m_updateCounter % BATCH_UPDATE_SIZE == 0) {
        m_tableWidget->setUpdatesEnabled(false);
    }
    
    int row = m_tableWidget->rowCount();
    m_tableWidget->insertRow(row);
    
    // Timestamp
    QTableWidgetItem *timestampItem = new QTableWidgetItem(formatTimestamp(timestamp));
    timestampItem->setFlags(timestampItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 0, timestampItem);
    
    // Name
    QTableWidgetItem *nameItem = new QTableWidgetItem(name);
    nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 1, nameItem);
    
    // Type
    QTableWidgetItem *typeItem = new QTableWidgetItem(getDataTypeString(value));
    typeItem->setFlags(typeItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 2, typeItem);
    
    // Value
    QTableWidgetItem *valueItem = new QTableWidgetItem(formatValue(value));
    valueItem->setFlags(valueItem->flags() & ~Qt::ItemIsEditable);
    m_tableWidget->setItem(row, 3, valueItem);
    
    // Trim excess rows
    trimTableRows();
    
    // Re-enable updates after batch
    if (m_updateCounter % BATCH_UPDATE_SIZE == 0) {
        m_tableWidget->setUpdatesEnabled(true);
    }
    
    // Auto-scroll
    if (m_autoScroll) {
        m_tableWidget->scrollToBottom();
    }
}

QString EssDatapointTableWidget::formatTimestamp(qint64 timestamp) const
{
    return QDateTime::fromMSecsSinceEpoch(timestamp).toString("hh:mm:ss.zzz");
}

QString EssDatapointTableWidget::formatValue(const QVariant &value) const
{
    if (value.userType() == QMetaType::QByteArray) {
        QByteArray ba = value.toByteArray();
        if (ba.size() > 50) {
            return QString("Binary data (%1 bytes)").arg(ba.size());
        }
        // Try to detect if it's text
        bool isText = true;
        for (char c : ba) {
            if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
                isText = false;
                break;
            }
        }
        if (isText) {
            return QString::fromUtf8(ba);
        } else {
            return QString("Binary data (%1 bytes)").arg(ba.size());
        }
    } else if (value.userType() == QMetaType::QJsonDocument) {
        QJsonDocument doc = value.toJsonDocument();
        return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
    } else if (value.userType() == QMetaType::QPointF) {
        QPointF pt = value.toPointF();
        return QString("(%1, %2)").arg(pt.x(), 0, 'f', 2).arg(pt.y(), 0, 'f', 2);
    } else {
        return value.toString();
    }
}

QString EssDatapointTableWidget::getDataTypeString(const QVariant &value) const
{
    QMetaType metaType(value.userType());
    const char *typeName = metaType.name();
    if (typeName) {
        QString type(typeName);
        // Simplify Qt type names
        type.remove("Q");
        return type;
    }
    return "Unknown";
}

bool EssDatapointTableWidget::matchesFilter(const QString &name) const
{
    if (m_filterRegex.pattern().isEmpty()) {
        return true;
    }
    
    QRegularExpressionMatch match = m_filterRegex.match(name);
    return match.hasMatch();
}

void EssDatapointTableWidget::trimTableRows()
{
    while (m_tableWidget->rowCount() > m_maxRows) {
        m_tableWidget->removeRow(0);
    }
}

void EssDatapointTableWidget::onPauseToggled(bool checked)
{
   m_paused = checked;
    m_pauseButton->setText(checked ? "Start" : "Stop");
    
    // Connect/disconnect from data stream based on state
    if (EssApplication::instance() && EssApplication::instance()->dataProcessor()) {
        if (checked) {
            // Disconnect when paused (checked = true means button says "Start")
            disconnect(EssApplication::instance()->dataProcessor(), 
                      &EssDataProcessor::genericDatapointReceived,
                      this, &EssDatapointTableWidget::onGenericDatapointReceived);
        } else {
            // Connect when running
            connect(EssApplication::instance()->dataProcessor(), 
                    &EssDataProcessor::genericDatapointReceived,
                    this, &EssDatapointTableWidget::onGenericDatapointReceived);
        }
    }
    
    // Add visual indicator when running
    if (!checked) {
        m_pauseButton->setStyleSheet("QPushButton { background-color: #90EE90; }"); // Light green
    } else {
        m_pauseButton->setStyleSheet(""); // Default
    }
}

void EssDatapointTableWidget::onClearClicked()
{
    m_tableWidget->setRowCount(0);
    m_updateCounter = 0;
}

void EssDatapointTableWidget::onFilterChanged(const QString &text)
{
    setFilterPattern(text);
    // Don't clear the table - just update the display with the new filter
}

void EssDatapointTableWidget::onMaxRowsChanged(int value)
{
    m_maxRows = value;
    trimTableRows();
}

void EssDatapointTableWidget::onAutoScrollToggled(bool checked)
{
    m_autoScroll = checked;
}

void EssDatapointTableWidget::onHostDisconnected()
{
    // 1. Ensure we're paused and disconnected from data stream
    if (!m_paused) {
        // Force to paused state without triggering button events
        m_paused = true;
        m_pauseButton->setChecked(true);
        m_pauseButton->setText("Start");
        m_pauseButton->setStyleSheet(""); // Remove green background
        
        // Disconnect from data stream
        if (EssApplication::instance() && EssApplication::instance()->dataProcessor()) {
            disconnect(EssApplication::instance()->dataProcessor(), 
                      &EssDataProcessor::genericDatapointReceived,
                      this, &EssDatapointTableWidget::onGenericDatapointReceived);
        }
    }
    
    // 2. Clear all data from the table
    m_tableWidget->setRowCount(0);
    m_updateCounter = 0;
    
    // 3. Reset filter
    m_filterEdit->clear();
    m_filterRegex = QRegularExpression();
}

void EssDatapointTableWidget::setPaused(bool paused)
{
    m_paused = paused;
    m_pauseButton->setChecked(paused);
    onPauseToggled(paused);
}

void EssDatapointTableWidget::setFilterPattern(const QString &pattern)
{
    if (pattern.isEmpty()) {
        m_filterRegex = QRegularExpression();
        m_filterEdit->clear();  // Sync the UI
        m_filterEdit->setStyleSheet("");  // Clear any error styling
    } else {
        m_filterRegex = QRegularExpression(pattern, 
                                           QRegularExpression::CaseInsensitiveOption);
        if (!m_filterRegex.isValid()) {
            // Invalid regex, clear it
            m_filterRegex = QRegularExpression();
            m_filterEdit->setStyleSheet("QLineEdit { background-color: #ffcccc; }");
        } else {
            m_filterEdit->setStyleSheet("");  // Clear error styling
            m_filterEdit->setText(pattern);    // Sync the UI
        }
    }
    
    // Apply filter to existing rows
    applyFilterToExistingRows();
}

QString EssDatapointTableWidget::filterPattern() const
{
    return m_filterRegex.pattern();
}

void EssDatapointTableWidget::clearTable()
{
    onClearClicked();
}

void EssDatapointTableWidget::applyFilterToExistingRows()
{
    // Hide/show rows based on current filter
    for (int row = 0; row < m_tableWidget->rowCount(); ++row) {
        QTableWidgetItem *nameItem = m_tableWidget->item(row, 1); // Name column
        if (nameItem) {
            QString name = nameItem->text();
            bool shouldShow = matchesFilter(name);
            m_tableWidget->setRowHidden(row, !shouldShow);
        }
    }
}

void EssDatapointTableWidget::setMaxRows(int maxRows)
{
    m_maxRows = maxRows;
    m_maxRowsSpinBox->setValue(maxRows);
    trimTableRows();
}

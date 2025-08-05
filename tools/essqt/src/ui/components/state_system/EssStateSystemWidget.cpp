// EssStateSystemWidget.cpp - Single trace view implementation
#include "EssStateSystemWidget.h"
#include "TclUtils.h"
#include "EssApplication.h"
#include "EssDataProcessor.h"
#include "EssEventProcessor.h"
#include "EssCommandInterface.h"
#include "core/EssEvent.h"
#include <QDebug>
#include <QTime>
#include <QGroupBox>
#include <QDateTime>
#include <QTableWidget>
#include <QHeaderView>
#include <algorithm>

EssStateSystemWidget::EssStateSystemWidget(QWidget *parent)
    : QWidget(parent)
    , m_systemRunning(false)
    , m_connected(false)
    , m_backendDebugEnabled(false)
    , m_viewingObsIndex(-1) // -1 = current/live view
    , m_eventProcessor(nullptr)
    , m_dataProcessor(nullptr)
{
    m_debugSession = std::make_unique<StateDebugSession>();
    
    setupUi();
    connectToDataProcessor();
    updateStatusLabel();
}

EssStateSystemWidget::~EssStateSystemWidget() = default;

void EssStateSystemWidget::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(5);
    
    // Status header
    QHBoxLayout *headerLayout = new QHBoxLayout();
    
    m_statusLabel = new QLabel("Disconnected", this);
    m_statusLabel->setStyleSheet("QLabel { font-weight: bold; padding: 5px; }");
    
    m_refreshButton = new QPushButton("Refresh", this);
    m_refreshButton->setEnabled(false);
    connect(m_refreshButton, &QPushButton::clicked, this, &EssStateSystemWidget::onRefreshClicked);
    
    headerLayout->addWidget(m_statusLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(m_refreshButton);
    
    // Backend debug control
    QHBoxLayout *debugLayout = new QHBoxLayout();
    m_backendDebugCheckbox = new QCheckBox("Enable Backend Debug", this);
    m_backendDebugCheckbox->setEnabled(false);
    m_backendDebugCheckbox->setToolTip("Enable debug event collection in the backend");
    connect(m_backendDebugCheckbox, &QCheckBox::toggled, this, &EssStateSystemWidget::onBackendDebugToggled);
    
    debugLayout->addWidget(m_backendDebugCheckbox);
    debugLayout->addStretch();
    
    mainLayout->addLayout(headerLayout);
    mainLayout->addLayout(debugLayout);
    
    // Observation navigation
    m_obsNavigationPanel = new QWidget(this);
    QHBoxLayout *navLayout = new QHBoxLayout(m_obsNavigationPanel);
    navLayout->setContentsMargins(0, 0, 0, 0);
    
    m_prevObsButton = new QPushButton("◀", this);
    m_prevObsButton->setFixedWidth(30);
    m_prevObsButton->setEnabled(false);
    connect(m_prevObsButton, &QPushButton::clicked, this, &EssStateSystemWidget::onPrevObservation);
    
    m_obsSpinBox = new QSpinBox(this);
    m_obsSpinBox->setMinimum(1);
    m_obsSpinBox->setPrefix("Obs ");
    m_obsSpinBox->setEnabled(false);
    connect(m_obsSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), 
            this, &EssStateSystemWidget::onObservationChanged);
    
    m_nextObsButton = new QPushButton("▶", this);
    m_nextObsButton->setFixedWidth(30);
    m_nextObsButton->setEnabled(false);
    connect(m_nextObsButton, &QPushButton::clicked, this, &EssStateSystemWidget::onNextObservation);
    
    m_obsInfoLabel = new QLabel("Live", this);
    m_obsInfoLabel->setStyleSheet("QLabel { font-weight: bold; color: green; }");
    
    navLayout->addWidget(new QLabel("Observation:", this));
    navLayout->addWidget(m_prevObsButton);
    navLayout->addWidget(m_obsSpinBox);
    navLayout->addWidget(m_nextObsButton);
    navLayout->addWidget(m_obsInfoLabel);
    navLayout->addStretch();
    
    mainLayout->addWidget(m_obsNavigationPanel);
    
    // Trace table
    m_traceTable = new QTableWidget(this);
    m_traceTable->setColumnCount(7);
    m_traceTable->setHorizontalHeaderLabels(QStringList() 
        << "#" << "State/Event" << "Time (ms)" << "Duration (ms)" << "Exit To" << "Type" << "Details");
    m_traceTable->setAlternatingRowColors(true);
    m_traceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_traceTable->horizontalHeader()->setStretchLastSection(true);
    m_traceTable->verticalHeader()->setVisible(false);
    
    // Set column widths
    m_traceTable->setColumnWidth(0, 40);   // #
    m_traceTable->setColumnWidth(1, 150);  // State/Event
    m_traceTable->setColumnWidth(2, 80);   // Time
    m_traceTable->setColumnWidth(3, 90);   // Duration
    m_traceTable->setColumnWidth(4, 100);  // Exit To
    m_traceTable->setColumnWidth(5, 60);   // Type
    m_traceTable->setColumnWidth(6, 200);  // Details
    
    // Set compact row height
    m_traceTable->verticalHeader()->setDefaultSectionSize(24);
    
    mainLayout->addWidget(m_traceTable, 1);
    
    setLayout(mainLayout);
    resize(600, 500);
}

void EssStateSystemWidget::connectToDataProcessor()
{
    if (EssApplication::instance() && EssApplication::instance()->dataProcessor()) {
        m_dataProcessor = EssApplication::instance()->dataProcessor();
        m_eventProcessor = m_dataProcessor->eventProcessor();
        
        if (m_eventProcessor) {
            connect(m_eventProcessor, &EssEventProcessor::systemStateChanged,
                    this, &EssStateSystemWidget::onSystemStateChanged);
            connect(m_eventProcessor, &EssEventProcessor::eventReceived,
                    this, &EssStateSystemWidget::onEventReceived);
            connect(m_eventProcessor, &EssEventProcessor::observationStarted,
                    this, &EssStateSystemWidget::onObservationStarted);
            connect(m_eventProcessor, &EssEventProcessor::observationReset,
                    this, &EssStateSystemWidget::onObservationEnded);
        }
        
        if (m_dataProcessor) {
            connect(m_dataProcessor, &EssDataProcessor::experimentStateChanged,
                    this, &EssStateSystemWidget::onExperimentStateChanged);
            connect(m_dataProcessor, &EssDataProcessor::genericDatapointReceived,
                    this, &EssStateSystemWidget::onDatapointUpdate);
        }
    }
    
    // Connect to command interface for connection status
    if (EssApplication::instance()) {
        auto* cmdInterface = EssApplication::instance()->commandInterface();
        if (cmdInterface) {
            connect(cmdInterface, &EssCommandInterface::connected,
                    this, &EssStateSystemWidget::onHostConnected);
            connect(cmdInterface, &EssCommandInterface::disconnected,
                    this, &EssStateSystemWidget::onHostDisconnected);
        }
    }
}

void EssStateSystemWidget::updateStatusLabel()
{
    QString statusText;
    QString styleSheet = "QLabel { font-weight: bold; padding: 5px; ";
    
    if (!m_connected) {
        statusText = "Disconnected";
        styleSheet += "color: gray; }";
    } else if (m_systemRunning) {
        statusText = "System: Running";
        styleSheet += "color: green; }";
    } else {
        statusText = "System: Stopped";
        styleSheet += "color: red; }";
    }
    
    // Add current state if available
    if (m_connected && !m_currentState.isEmpty()) {
        statusText += QString(" [%1]").arg(m_currentState);
    }
    
    // Add debug indicator
    if (m_backendDebugEnabled) {
        statusText += " 🐛";
    }
    
    m_statusLabel->setText(statusText);
    m_statusLabel->setStyleSheet(styleSheet);
}

void EssStateSystemWidget::updateTraceTable()
{
    m_traceTable->setRowCount(0);
    
    const ObservationDebugData* obsData = nullptr;
    qint64 obsStartTime = 0;
    
    if (m_viewingObsIndex == -1) {
        // Live view - build from current observation
        obsData = m_debugSession->currentObservation();
    } else {
        // Historical view
        const auto& observations = m_debugSession->observations();
        if (m_viewingObsIndex < observations.size()) {
            obsData = &observations[m_viewingObsIndex];
        }
    }
    
    if (!obsData) return;
    
    obsStartTime = obsData->startTime;
    
    // Build a combined list of all events (state entries and debug events)
    struct TraceRow {
        enum Type { StateEntry, DebugEvent };
        Type type;
        qint64 timestamp;
        QString stateName;
        const StateTraceEntry* stateEntry;
        const StateDebugEvent* debugEvent;
    };
    
    QList<TraceRow> rows;
    
    // Add state entries
    for (const auto& entry : obsData->trace) {
        TraceRow row;
        row.type = TraceRow::StateEntry;
        row.timestamp = entry.enterTime;
        row.stateName = entry.stateName;
        row.stateEntry = &entry;
        row.debugEvent = nullptr;
        rows.append(row);
    }
    
    // Add debug events (except Enter/Exit which are already shown as state entries)
    if (m_backendDebugEnabled) {
        for (const auto& eventPtr : obsData->events) {
            if (eventPtr && eventPtr->type != StateDebugType::Enter && eventPtr->type != StateDebugType::Exit) {
                TraceRow row;
                row.type = TraceRow::DebugEvent;
                row.timestamp = eventPtr->timestamp;
                row.stateName = eventPtr->stateName;
                row.stateEntry = nullptr;
                row.debugEvent = eventPtr.get();
                rows.append(row);
            }
        }
    }
    
    // Sort by timestamp
    std::sort(rows.begin(), rows.end(), [](const TraceRow& a, const TraceRow& b) {
        return a.timestamp < b.timestamp;
    });
    
    // Populate trace table
    int rowNum = 0;
    int stateNum = 0;
    for (const auto& row : rows) {
        m_traceTable->insertRow(rowNum);
        
        if (row.type == TraceRow::StateEntry) {
            stateNum++;
            const StateTraceEntry& entry = *row.stateEntry;
            
            // # column
            auto* numItem = new QTableWidgetItem(QString::number(stateNum));
            numItem->setTextAlignment(Qt::AlignCenter);
            m_traceTable->setItem(rowNum, 0, numItem);
            
            // State/Event column (with visit number if > 1)
            QString stateName = entry.stateName;
            if (entry.visitNumber > 1) {
                stateName += QString(" (%1)").arg(entry.visitNumber);
            }
            auto* stateItem = new QTableWidgetItem(stateName);
            stateItem->setFont(QFont("", -1, QFont::Bold));
            m_traceTable->setItem(rowNum, 1, stateItem);
            
            // Time column (relative to obs start)
            qint64 relativeEnter = (entry.enterTime - obsStartTime) / 1000; // to ms
            m_traceTable->setItem(rowNum, 2, new QTableWidgetItem(QString::number(relativeEnter)));
            
            // Duration column
            if (entry.exitTime > 0) {
                m_traceTable->setItem(rowNum, 3, new QTableWidgetItem(QString::number(entry.duration() / 1000)));
            } else if (m_viewingObsIndex == -1 && &entry == &obsData->trace.last()) {
                // Current state in live view
                qint64 currentDuration = (QDateTime::currentMSecsSinceEpoch() * 1000 - entry.enterTime) / 1000;
                QTableWidgetItem *durationItem = new QTableWidgetItem(QString::number(currentDuration));
                durationItem->setForeground(QColor(0, 150, 0)); // Green for active
                m_traceTable->setItem(rowNum, 3, durationItem);
            } else {
                m_traceTable->setItem(rowNum, 3, new QTableWidgetItem("-"));
            }
            
            // Exit To column
            if (!entry.exitTo.isEmpty()) {
                m_traceTable->setItem(rowNum, 4, new QTableWidgetItem(entry.exitTo));
            } else if (m_viewingObsIndex == -1 && &entry == &obsData->trace.last()) {
                QTableWidgetItem *activeItem = new QTableWidgetItem("(active)");
                activeItem->setForeground(QColor(0, 150, 0));
                m_traceTable->setItem(rowNum, 4, activeItem);
            }
            
            // Type column
            m_traceTable->setItem(rowNum, 5, new QTableWidgetItem("State"));
            
            // Details column - summary of debug events if backend debug enabled
            if (m_backendDebugEnabled) {
                QStringList details;
                if (entry.checks.size() > 0) {
                    details.append(QString("%1 checks").arg(entry.checks.size()));
                }
                if (entry.variableChanges.size() > 0) {
                    details.append(QString("%1 var changes").arg(entry.variableChanges.size()));
                }
                if (entry.timerStarts > 0) {
                    details.append(QString("%1 timers").arg(entry.timerStarts));
                }
                if (entry.methodCalls > 0) {
                    details.append(QString("%1 methods").arg(entry.methodCalls));
                }
                m_traceTable->setItem(rowNum, 6, new QTableWidgetItem(details.join(", ")));
            }
            
        } else {
            // Debug event row
            const StateDebugEvent& event = *row.debugEvent;
            
            // # column - empty for debug events
            m_traceTable->setItem(rowNum, 0, new QTableWidgetItem(""));
            
            // State/Event column - indent and show event type
            QString eventText = "  → " + event.getTypeString();
            auto* eventItem = new QTableWidgetItem(eventText);
            eventItem->setForeground(QColor(60, 60, 60)); // Dark gray text
            m_traceTable->setItem(rowNum, 1, eventItem);
            
            // Time column
            qint64 relativeTime = (event.timestamp - obsStartTime) / 1000; // to ms
            auto* timeItem = new QTableWidgetItem(QString::number(relativeTime));
            m_traceTable->setItem(rowNum, 2, timeItem);
            
            // Duration - empty for debug events
            m_traceTable->setItem(rowNum, 3, new QTableWidgetItem(""));
            
            // Exit to - empty for debug events
            m_traceTable->setItem(rowNum, 4, new QTableWidgetItem(""));
            
            // Type column - show icon
            QString icon;
            switch(event.type) {
                case StateDebugType::Check: icon = "Check"; break;
                case StateDebugType::Var: icon = "Var"; break;
                case StateDebugType::Timer: icon = "Timer"; break;
                case StateDebugType::Method: icon = "Method"; break;
                default: icon = "Other"; break;
            }
            m_traceTable->setItem(rowNum, 5, new QTableWidgetItem(icon));
            
            // Details column
            QString details;
            switch(event.type) {
                case StateDebugType::Check:
                    details = event.details;
                    if (!event.result.isEmpty()) {
                        details += " = " + event.result;
                    }
                    break;
                case StateDebugType::Var:
                    details = event.details; // var_name value
                    break;
                case StateDebugType::Timer:
                    details = event.details;
                    break;
                case StateDebugType::Method:
                    details = event.details;
                    break;
                default:
                    details = event.details;
                    break;
            }
            
            auto* detailsItem = new QTableWidgetItem(details);
            m_traceTable->setItem(rowNum, 6, detailsItem);
            
            // Set subtle background color for all debug rows
            QColor debugRowColor(255, 240, 245); // Very light pink
            
            // Apply background to all columns
            for (int col = 0; col < m_traceTable->columnCount(); ++col) {
                if (m_traceTable->item(rowNum, col)) {
                    m_traceTable->item(rowNum, col)->setBackground(debugRowColor);
                }
            }
        }
        
        rowNum++;
    }
    
    // Auto-scroll to bottom for live view
    if (m_viewingObsIndex == -1 && m_traceTable->rowCount() > 0) {
        m_traceTable->scrollToBottom();
    }
}

QString EssStateSystemWidget::formatDuration(qint64 milliseconds) const
{
    // For trace table, just show milliseconds as a number
    return QString::number(milliseconds);
}

// Observation navigation methods
void EssStateSystemWidget::updateObservationNavigation()
{
    int totalObs = m_debugSession->observations().size();
    bool hasObs = totalObs > 0;
    
    m_obsSpinBox->setEnabled(hasObs);
    m_prevObsButton->setEnabled(hasObs);
    m_nextObsButton->setEnabled(hasObs);
    
    if (hasObs) {
        m_obsSpinBox->setMaximum(totalObs);
        
        if (m_viewingObsIndex == -1) {
            // Live view
            m_obsInfoLabel->setText("Live");
            m_obsInfoLabel->setStyleSheet("QLabel { font-weight: bold; color: green; }");
            m_obsSpinBox->setValue(totalObs); // Show highest obs number
        } else {
            // Historical view
            m_obsInfoLabel->setText(QString("Historical"));
            m_obsInfoLabel->setStyleSheet("QLabel { font-weight: bold; color: blue; }");
            m_obsSpinBox->setValue(m_viewingObsIndex + 1); // Convert to 1-based
        }
    } else {
        m_obsInfoLabel->setText("No data");
        m_obsInfoLabel->setStyleSheet("QLabel { font-weight: bold; color: gray; }");
    }
}

void EssStateSystemWidget::showObservation(int obsIndex)
{
    m_viewingObsIndex = obsIndex;
    updateObservationNavigation();
    updateTraceTable();
}

void EssStateSystemWidget::onPrevObservation()
{
    int totalObs = m_debugSession->observations().size();
    if (totalObs == 0) return;
    
    if (m_viewingObsIndex == -1) {
        // From live to last historical observation
        showObservation(totalObs - 1);
    } else if (m_viewingObsIndex > 0) {
        // Previous historical observation
        showObservation(m_viewingObsIndex - 1);
    }
}

void EssStateSystemWidget::onNextObservation()
{
    int totalObs = m_debugSession->observations().size();
    if (totalObs == 0) return;
    
    if (m_viewingObsIndex == -1) {
        // Already at live view
        return;
    } else if (m_viewingObsIndex < totalObs - 1) {
        // Next historical observation
        showObservation(m_viewingObsIndex + 1);
    } else {
        // From last historical to live
        showObservation(-1);
    }
}

void EssStateSystemWidget::onObservationChanged(int obsNumber)
{
    // Convert from 1-based to 0-based, but -1 for live view
    int totalObs = m_debugSession->observations().size();
    if (obsNumber == totalObs && m_debugSession->currentObservation() && 
        m_debugSession->currentObservation()->isActive()) {
        // Latest observation and it's active = live view
        showObservation(-1);
    } else {
        // Historical observation
        showObservation(obsNumber - 1);
    }
}

void EssStateSystemWidget::processDebugEvent(const EssEvent &event)
{
    if (event.type != 10) return; // EVT_STATE_DEBUG = 10
    
    StateDebugEvent debugEvent = parseDebugEvent(event);
    if (debugEvent.stateName.isEmpty()) return;
    
    m_debugSession->addDebugEvent(debugEvent);
    
    // Update current state tracking
    if (debugEvent.type == StateDebugType::Enter) {
        setCurrentState(debugEvent.stateName);
    } else if (debugEvent.type == StateDebugType::Exit && !debugEvent.result.isEmpty()) {
        setCurrentState(debugEvent.result);
    }
    
    // Update display (only if viewing live)
    if (m_viewingObsIndex == -1) {
        updateObservationNavigation();
        updateTraceTable();
    }
}

StateDebugEvent EssStateSystemWidget::parseDebugEvent(const EssEvent &event)
{
    QString params = event.paramsAsString();
    QStringList parts = params.split(' ', Qt::SkipEmptyParts);
    
    if (parts.isEmpty()) {
        return StateDebugEvent(static_cast<StateDebugType>(event.subtype), "", event.timestamp);
    }
    
    QString stateName = parts[0];
    QString details, result;
    
    // Handle different event types based on subtype
    switch(static_cast<StateDebugType>(event.subtype)) {
        case StateDebugType::Enter:
            // Enter events: just "state_name"
            break;
            
        case StateDebugType::Exit:
            // Exit events: "state_name next_state"
            if (parts.size() > 1) result = parts[1];
            break;
            
        case StateDebugType::Check:
            // Check events: "state_name condition_expr result"
            if (parts.size() > 1) details = parts[1];
            if (parts.size() > 2) result = parts[2];
            break;
            
        case StateDebugType::Var:
            // Var events: "state_name var_name value"
            if (parts.size() > 1) {
                details = parts.mid(1).join(" "); // var_name value
            }
            break;
            
        case StateDebugType::Timer:
            // Timer events: "state_name action value"
            if (parts.size() > 1) {
                details = parts.mid(1).join(" ");
            }
            break;
            
        case StateDebugType::Method:
            // Method events: "state_name method_name"
            if (parts.size() > 1) details = parts[1];
            break;
            
        case StateDebugType::Transition:
            // Transition events
            if (parts.size() > 1) details = parts[1];
            if (parts.size() > 2) result = parts[2];
            break;
    }
    
    return StateDebugEvent(static_cast<StateDebugType>(event.subtype), 
                          stateName, event.timestamp, details, result);
}

void EssStateSystemWidget::enableBackendDebug(bool enable)
{
    if (!m_connected) return;
    
    if (EssApplication::instance()) {
        auto* cmdInterface = EssApplication::instance()->commandInterface();
        if (cmdInterface) {
            QString cmd = enable ? 
                "set ::ess::debug::enabled 1" : 
                "set ::ess::debug::enabled 0";
            cmdInterface->executeEss(cmd);
        }
    }
}

void EssStateSystemWidget::setCurrentState(const QString &stateName)
{
    if (m_currentState != stateName) {
        m_currentState = stateName;
        updateStatusLabel();
    }
}

void EssStateSystemWidget::loadStateTable(const QString &stateTableStr)
{
    // Use the shared TclUtils instance and ESS helper functions
    QStringList newStates = EssTclHelpers::extractStateNames(stateTableStr);
    
    if (newStates != m_allStates) {
        m_allStates = newStates;
        qDebug() << "Loaded" << m_allStates.size() << "states:" << m_allStates;
    }
}

void EssStateSystemWidget::clear()
{
    m_allStates.clear();
    m_currentState.clear();
    m_systemRunning = false;
    
    // Clear debug data
    m_debugSession->clear();
    m_viewingObsIndex = -1; // Reset to live view
    
    // Clear trace table
    m_traceTable->setRowCount(0);
    
    updateObservationNavigation();
    updateStatusLabel();
}

// Event slots
void EssStateSystemWidget::onSystemStateChanged(int state)
{
    // Map system state enum to boolean (assuming 1 = running, 0 = stopped)
    bool wasRunning = m_systemRunning;
    m_systemRunning = (state == 1); // Adjust based on your SystemState enum
    
    if (wasRunning != m_systemRunning) {
        updateStatusLabel();
        
        // Clear current state when system stops
        if (!m_systemRunning) {
            setCurrentState("");
            
            // Clear debug session for new run but keep state table
            m_debugSession->clear();
            m_viewingObsIndex = -1;
            m_traceTable->setRowCount(0);
            updateObservationNavigation();
        }
    }
}

void EssStateSystemWidget::onExperimentStateChanged(const QString &newState)
{
    // This might be more detailed state information
    qDebug() << "Experiment state changed to:" << newState;
    
    // Check for states that indicate system changes
    if (newState == "Loading" || newState == "Unloading") {
        // System is being reloaded
        clear();
        qDebug() << "System loading/unloading - clearing state widget";
        return;
    }
    
    // Update running status based on state name
    bool isRunning = (newState != "Stopped" && newState != "Disconnected");
    if (isRunning != m_systemRunning) {
        m_systemRunning = isRunning;
        updateStatusLabel();
        
        if (!m_systemRunning) {
            setCurrentState("");
        }
    }
}

void EssStateSystemWidget::onDatapointUpdate(const QString &name, const QVariant &value, qint64 timestamp)
{
    Q_UNUSED(timestamp)
    
    if (name == "ess/state_table") {
        // Load the state table when it's updated
        loadStateTable(value.toString());
    }
    else if (name == "ess/action_state") {
        // Extract state name from action state (remove _a suffix)
        QString actionState = value.toString();
        if (actionState.endsWith("_a")) {
            setCurrentState(actionState.left(actionState.length() - 2));
        }
    }
    else if (name == "ess/transition_state") {
        // Extract state name from transition state (remove _t suffix)
        QString transitionState = value.toString();
        if (transitionState.endsWith("_t")) {
            setCurrentState(transitionState.left(transitionState.length() - 2));
        }
    }
    else if (name == "ess/system" || name == "ess/protocol" || name == "ess/variant") {
        // System change detected - clear everything
        clear();
        qDebug() << "System change detected, clearing state widget";
    }
    else if (name == "ess/user_reset") {
        // User reset - only clear debug data and trace, keep state table
        m_debugSession->clear();
        m_viewingObsIndex = -1;
        m_traceTable->setRowCount(0);
        updateObservationNavigation();
        qDebug() << "User reset - cleared debug data";
        
        // Clear current state
        setCurrentState("");
    }
}

void EssStateSystemWidget::onEventReceived(const EssEvent &event)
{
    if (event.type == 10) { // EVT_STATE_DEBUG
        processDebugEvent(event);
    }
}

void EssStateSystemWidget::onBackendDebugToggled(bool enabled)
{
    m_backendDebugEnabled = enabled;
    enableBackendDebug(enabled);
    updateStatusLabel();
    updateTraceTable(); // Refresh view to show/hide debug events
}

void EssStateSystemWidget::onObservationStarted(uint64_t timestamp)
{
    // Get the actual observation number if possible
    int obsNum = m_debugSession->observations().size();
    m_debugSession->startObservation(obsNum, timestamp);
    
    // Stay on live view when new observation starts
    showObservation(-1);
}

void EssStateSystemWidget::onObservationEnded()
{
    m_debugSession->endObservation(QDateTime::currentMSecsSinceEpoch() * 1000);
    updateObservationNavigation();
    
    // Update display if viewing live
    if (m_viewingObsIndex == -1) {
        updateTraceTable();
    }
}

void EssStateSystemWidget::onHostConnected(const QString &host)
{
    Q_UNUSED(host)
    
    m_connected = true;
    m_refreshButton->setEnabled(true);
    m_backendDebugCheckbox->setEnabled(true);
    updateStatusLabel();
    
    // Check if debug is already enabled on backend
    if (EssApplication::instance()) {
        auto* cmdInterface = EssApplication::instance()->commandInterface();
        if (cmdInterface) {
            auto result = cmdInterface->executeEss("set ::ess::debug::enabled");
            if (result.status == EssCommandInterface::StatusSuccess) {
                bool debugEnabled = (result.response.trimmed() == "1");
                m_backendDebugCheckbox->setChecked(debugEnabled);
                m_backendDebugEnabled = debugEnabled;
            }
        }
    }
    
    qDebug() << "StateSystemWidget: Connected to host";
}

void EssStateSystemWidget::onHostDisconnected()
{
    m_connected = false;
    m_refreshButton->setEnabled(false);
    m_backendDebugCheckbox->setEnabled(false);
    m_backendDebugCheckbox->setChecked(false);
    m_systemRunning = false;
    m_backendDebugEnabled = false;
    
    clear();
    
    qDebug() << "StateSystemWidget: Disconnected from host";
}

void EssStateSystemWidget::onRefreshClicked()
{
    if (!m_connected) {
        return;
    }
    
    // Request current state table from backend
    if (EssApplication::instance()) {
        auto* cmdInterface = EssApplication::instance()->commandInterface();
        if (cmdInterface) {
            // Request state table update
            auto result = cmdInterface->executeEss("dservTouch ess/state_table");
            if (result.status != EssCommandInterface::StatusSuccess) {
                qWarning() << "Failed to refresh state table:" << result.response;
            } else {
                qDebug() << "Requested state table refresh";
            }
        }
    }
}
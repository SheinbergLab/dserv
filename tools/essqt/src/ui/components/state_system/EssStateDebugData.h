// EssStateDebugData.h - Enhanced with full debug event support
#pragma once

#include <QString>
#include <QStringList>
#include <QMap>
#include <QDateTime>
#include <memory>

/**
 * @brief Data structures for ESS state debugging
 * 
 * These structures capture debug information from the ESS backend
 * debug system (::ess::debug namespace in ess-2.0.tm)
 */

// STATE_DEBUG subtypes from ess-2.0.tm
enum class StateDebugType {
    Enter = 0,      // State entered
    Exit = 1,       // State exited  
    Check = 2,      // Condition check
    Transition = 3, // Transition evaluation
    Var = 4,        // Variable update
    Timer = 5,      // Timer operation
    Method = 6      // Method call
};

/**
 * @brief Individual debug event from a state
 */
struct StateDebugEvent {
    StateDebugType type;
    QString stateName;
    qint64 timestamp;
    QString details;
    QString result;         // For checks: "true"/"false", for exits: next state
    
    StateDebugEvent(StateDebugType t, const QString& state, qint64 ts, 
                   const QString& det = "", const QString& res = "")
        : type(t), stateName(state), timestamp(ts), details(det), result(res) {}
        
    QString getTypeString() const {
        switch(type) {
            case StateDebugType::Enter: return "Enter";
            case StateDebugType::Exit: return "Exit";
            case StateDebugType::Check: return "Check";
            case StateDebugType::Transition: return "Transition";
            case StateDebugType::Var: return "Var";
            case StateDebugType::Timer: return "Timer";
            case StateDebugType::Method: return "Method";
            default: return "Unknown";
        }
    }
    
    QString getDisplayText() const {
        switch(type) {
            case StateDebugType::Enter:
                return QString("→ %1").arg(stateName);
            case StateDebugType::Exit:
                return QString("← %1%2").arg(stateName)
                    .arg(result.isEmpty() ? "" : QString(" → %1").arg(result));
            case StateDebugType::Check:
                return QString("? %1: %2 = %3").arg(stateName).arg(details).arg(result);
            case StateDebugType::Var:
                return QString("= %1: %2").arg(stateName).arg(details);
            case StateDebugType::Timer:
                return QString("⏱ %1: %2").arg(stateName).arg(details);
            case StateDebugType::Method:
                return QString("() %1: %2").arg(stateName).arg(details);
            default:
                return QString("%1: %2").arg(stateName).arg(details);
        }
    }
};

/**
 * @brief Enhanced trace entry with full debug info
 */
struct StateTraceEntry {
    QString stateName;
    qint64 enterTime;
    qint64 exitTime;
    QString exitTo;         // Next state
    int visitNumber;        // Which visit to this state in this obs
    
    // Additional debug info collected during this state visit
    QList<StateDebugEvent> debugEvents;
    QMap<QString, QString> variableChanges;  // Variable name -> final value
    QStringList checks;                      // All condition checks performed
    int timerStarts = 0;
    int methodCalls = 0;
    
    qint64 duration() const { 
        return (exitTime > 0) ? (exitTime - enterTime) : 0; 
    }
    
    void addDebugEvent(const StateDebugEvent& event) {
        debugEvents.append(event);
        
        switch(event.type) {
            case StateDebugType::Var:
                // Parse "var_name value" format
                {
                    QStringList parts = event.details.split(' ', Qt::SkipEmptyParts);
                    if (parts.size() >= 2) {
                        variableChanges[parts[0]] = parts[1];
                    }
                }
                break;
            case StateDebugType::Check:
                checks.append(QString("%1 = %2").arg(event.details).arg(event.result));
                break;
            case StateDebugType::Timer:
                if (event.details.contains("start")) {
                    timerStarts++;
                }
                break;
            case StateDebugType::Method:
                methodCalls++;
                break;
            default:
                break;
        }
    }
};

/**
 * @brief Aggregated statistics for a state
 */
struct StateStats {
    QString stateName;
    
    // Current observation stats
    int currentObsVisits = 0;
    qint64 currentObsTotalTime = 0;
    qint64 currentObsLastEnter = 0;
    bool currentlyActive = false;
    
    // Historical stats (across all observations)
    int totalVisits = 0;
    qint64 totalTimeMs = 0;
    qint64 minTimeMs = LLONG_MAX;
    qint64 maxTimeMs = 0;
    qint64 avgTimeMs = 0;
    
    // Exit destinations and their counts (current observation)
    QMap<QString, int> currentObsExits;
    
    // Condition checks performed (current observation)
    QMap<QString, QString> currentObsChecks;  // condition -> result
    
    // Debug event counts for current observation
    int currentObsTimerCount = 0;
    int currentObsVarChangeCount = 0;
    int currentObsMethodCallCount = 0;
    
    void updateAverages() {
        if (totalVisits > 0) {
            avgTimeMs = totalTimeMs / totalVisits;
        }
    }
    
    void startNewObservation() {
        // Move current obs stats to historical
        totalVisits += currentObsVisits;
        totalTimeMs += currentObsTotalTime;
        
        // Reset current observation stats
        currentObsVisits = 0;
        currentObsTotalTime = 0;
        currentObsLastEnter = 0;
        currentlyActive = false;
        currentObsExits.clear();
        currentObsChecks.clear();
        currentObsTimerCount = 0;
        currentObsVarChangeCount = 0;
        currentObsMethodCallCount = 0;
        
        updateAverages();
    }
    
    QString getDisplayText(bool debugMode) const {
        if (!debugMode) {
            return stateName;
        }
        
        // Show current obs stats primarily, with historical context
        if (currentObsVisits == 0 && totalVisits == 0) {
            return QString("%1 (not visited)").arg(stateName);
        }
        
        QString text = stateName;
        
        if (currentObsVisits > 0) {
            text += QString(" [%1x").arg(currentObsVisits);
            if (currentObsTotalTime > 0) {
                qint64 avgTime = currentObsTotalTime / currentObsVisits;
                text += QString(", %1ms").arg(avgTime);
            }
            
            // Add debug info if available
            if (currentObsChecks.size() > 0) {
                text += QString(", %1✓").arg(currentObsChecks.size());
            }
            if (currentObsTimerCount > 0) {
                text += QString(", %1⏱").arg(currentObsTimerCount);
            }
            
            text += "]";
        }
        
        if (totalVisits > 0) {
            text += QString(" (hist: %1x)").arg(totalVisits);
        }
        
        return text;
    }
};

/**
 * @brief Collection of debug events for a single observation/trial
 */
struct ObservationDebugData {
    int observationNumber = 0;
    qint64 startTime = 0;
    qint64 endTime = 0;
    
    // All events in chronological order
    QList<std::shared_ptr<StateDebugEvent>> events;
    
    // State trace with enriched debug info
    QList<StateTraceEntry> trace;
    
    // Snapshot of state statistics when observation ended
    QMap<QString, StateStats> finalStateStats;
    
    bool isActive() const { return endTime == 0; }
    qint64 duration() const { 
        qint64 currentTime = QDateTime::currentMSecsSinceEpoch() * 1000;
        return isActive() ? (currentTime - startTime) : (endTime - startTime); 
    }
    
    void addTraceEntry(const QString& state, qint64 enterTime) {
        StateTraceEntry entry;
        entry.stateName = state;
        entry.enterTime = enterTime;
        entry.exitTime = 0;
        
        // Count previous visits to this state
        entry.visitNumber = 1;
        for (const auto& prev : trace) {
            if (prev.stateName == state) {
                entry.visitNumber++;
            }
        }
        
        trace.append(entry);
    }
    
    void completeTraceEntry(const QString& state, qint64 exitTime, const QString& nextState) {
        // Find the most recent uncompleted entry for this state
        for (int i = trace.size() - 1; i >= 0; --i) {
            if (trace[i].stateName == state && trace[i].exitTime == 0) {
                trace[i].exitTime = exitTime;
                trace[i].exitTo = nextState;
                break;
            }
        }
    }
    
    void addDebugEventToTrace(const StateDebugEvent& event) {
        // Find the current active state in trace
        for (int i = trace.size() - 1; i >= 0; --i) {
            if (trace[i].stateName == event.stateName && trace[i].exitTime == 0) {
                trace[i].addDebugEvent(event);
                break;
            }
        }
    }
};

/**
 * @brief Complete debug session data with observation-aware state tracking
 */
class StateDebugSession {
public:
    StateDebugSession() = default;
    
    void startObservation(int obsNum, qint64 timestamp);
    void endObservation(qint64 timestamp);
    void addDebugEvent(const StateDebugEvent& event);
    
    // Getters for current state
    const QMap<QString, StateStats>& getCurrentStateStats() const { return m_currentStateStats; }
    const StateStats* getStateStats(const QString& stateName) const;
    QStringList getVisitedStates() const;
    QString getCurrentState() const { return m_currentState; }
    
    // Observation history
    const QList<ObservationDebugData>& observations() const { return m_observations; }
    const ObservationDebugData* currentObservation() const;
    ObservationDebugData* currentObservation();
    
    // Management
    void clear();
    void setMaxObservations(int max) { m_maxObservations = max; }
    
private:
    QList<ObservationDebugData> m_observations;
    QMap<QString, StateStats> m_currentStateStats; // Current running stats
    QString m_currentState;
    int m_maxObservations = 100;
    
    void processStateEvent(const StateDebugEvent& event);
    void cleanupOldObservations();
};
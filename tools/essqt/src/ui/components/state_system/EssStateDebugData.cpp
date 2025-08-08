// EssStateDebugData.cpp - Enhanced implementation with full debug support
#include "EssStateDebugData.h"
#include <QDebug>

void StateDebugSession::addDebugEvent(const StateDebugEvent& event)
{
    if (m_observations.isEmpty()) {
        // Create a default observation if none exists
        startObservation(0, event.timestamp);
    }
    
    ObservationDebugData& currentObs = m_observations.last();
    
    // Store the event
    currentObs.events.append(std::make_shared<StateDebugEvent>(event));
    
    // Add to trace if applicable
    currentObs.addDebugEventToTrace(event);
    
    // Process the event for statistics
    processStateEvent(event);
}

ObservationDebugData* StateDebugSession::currentObservation()
{
    if (m_observations.isEmpty()) return nullptr;
    return &m_observations.last();
}

const ObservationDebugData* StateDebugSession::currentObservation() const
{
    if (m_observations.isEmpty()) return nullptr;
    return &m_observations.last();
}

void StateDebugSession::startObservation(int obsNum, qint64 timestamp)
{
    // End current observation if active and save final state
    if (!m_observations.isEmpty() && m_observations.last().isActive()) {
        endObservation(timestamp);
    }
    
    // Start all states' new observation period
    for (auto& stats : m_currentStateStats) {
        stats.startNewObservation();
    }
    
    ObservationDebugData newObs;
    newObs.observationNumber = obsNum;
    newObs.startTime = timestamp;
    
    m_observations.append(newObs);
    
    // Clear current state tracking
    m_currentState.clear();
}

void StateDebugSession::endObservation(qint64 timestamp)
{
    if (m_observations.isEmpty()) return;
    
    ObservationDebugData& currentObs = m_observations.last();
    if (currentObs.isActive()) {
        currentObs.endTime = timestamp;
        
        // Save snapshot of current state stats
        currentObs.finalStateStats = m_currentStateStats;
        
        // Mark all states as inactive
        for (auto& stats : m_currentStateStats) {
            stats.currentlyActive = false;
        }
    }
    
    cleanupOldObservations();
}

const StateStats* StateDebugSession::getStateStats(const QString& stateName) const
{
    auto it = m_currentStateStats.find(stateName);
    return (it != m_currentStateStats.end()) ? &it.value() : nullptr;
}

QStringList StateDebugSession::getVisitedStates() const
{
    QStringList visited;
    for (auto it = m_currentStateStats.constBegin(); it != m_currentStateStats.constEnd(); ++it) {
        if (it.value().currentObsVisits > 0 || it.value().totalVisits > 0) {
            visited.append(it.key());
        }
    }
    
    visited.sort();
    return visited;
}

void StateDebugSession::clear()
{
    m_observations.clear();
    m_currentStateStats.clear();
    m_currentState.clear();
}

void StateDebugSession::processStateEvent(const StateDebugEvent& event)
{
    StateStats& stats = m_currentStateStats[event.stateName];
    stats.stateName = event.stateName;
    
    ObservationDebugData* currentObs = currentObservation();
    
    switch (event.type) {
        case StateDebugType::Enter:
            stats.currentObsVisits++;
            stats.currentObsLastEnter = event.timestamp;
            stats.currentlyActive = true;
            m_currentState = event.stateName;
            
            // Add to trace
            if (currentObs) {
                currentObs->addTraceEntry(event.stateName, event.timestamp);
            }
            
            // Mark other states as inactive
            for (auto& otherStats : m_currentStateStats) {
                if (otherStats.stateName != event.stateName) {
                    otherStats.currentlyActive = false;
                }
            }
            break;
            
        case StateDebugType::Exit:
            stats.currentlyActive = false;
            
            if (!event.result.isEmpty()) {
                stats.currentObsExits[event.result]++;
            }
            
            // Calculate time spent in state for current observation
            if (stats.currentObsLastEnter > 0) {
                qint64 duration = (event.timestamp - stats.currentObsLastEnter) / 1000; // Convert to ms
                stats.currentObsTotalTime += duration;
                
                // Update global min/max across all observations
                stats.minTimeMs = qMin(stats.minTimeMs, duration);
                stats.maxTimeMs = qMax(stats.maxTimeMs, duration);
            }
            
            // Complete trace entry
            if (currentObs) {
                currentObs->completeTraceEntry(event.stateName, event.timestamp, event.result);
            }
            
            if (m_currentState == event.stateName) {
                m_currentState = event.result; // Next state
            }
            break;
            
        case StateDebugType::Check:
            // Simply record the check and its result
            stats.currentObsChecks[event.details] = event.result;
            break;
            
        case StateDebugType::Var:
            stats.currentObsVarChangeCount++;
            break;
            
        case StateDebugType::Timer:
            stats.currentObsTimerCount++;
            break;
            
        case StateDebugType::Method:
            stats.currentObsMethodCallCount++;
            break;
            
        case StateDebugType::Transition:
            // Transition events are informational
            break;
    }
}

void StateDebugSession::cleanupOldObservations()
{
    while (m_observations.size() > m_maxObservations) {
        m_observations.removeFirst();
    }
}
#include "qtcgmanager.hpp"
#include "qtcgraph.hpp"
#include <QDebug>
#include <QPainter>

QtCGManager& QtCGManager::getInstance()
{
    static QtCGManager instance;
    return instance;
}

QString QtCGManager::registerGraph(const QString& name, QtCGraph* graph)
{
    if (!graph) return QString();
    
    QString graphName = name;
    
    // Ensure unique name
    if (m_graphs.contains(graphName)) {
        graphName = generateUniqueName(name);
    }
    
    m_graphs[graphName] = graph;
    
    // Connect to destroyed signal to auto-cleanup
    connect(graph, &QObject::destroyed, this, [this, graphName]() {
        unregisterGraph(graphName);
    });
    
    emit graphRegistered(graphName, graph);
    return graphName;
}

bool QtCGManager::unregisterGraph(const QString& name)
{
    auto it = m_graphs.find(name);
    if (it == m_graphs.end()) {
        return false;
    }
    
    QtCGraph* graph = it.value();
    
    // If this was current, clear it
    if (graph == m_currentGraph) {
        setCurrentGraph(nullptr);
    }
    
    // Remove from all groups
    for (auto& groupList : m_groups) {
        groupList.removeAll(name);
    }
    
    m_graphs.erase(it);
    emit graphUnregistered(name);
    return true;
}

QtCGraph* QtCGManager::getGraph(const QString& name) const
{
    return m_graphs.value(name, nullptr);
}

QList<QString> QtCGManager::getAllGraphNames() const
{
    return m_graphs.keys();
}

QList<QtCGraph*> QtCGManager::getAllGraphs() const
{
    QList<QtCGraph*> result;
    for (auto graph : m_graphs) {
        if (graph) {
            result.append(graph);
        }
    }
    return result;
}

int QtCGManager::send(const QString& graphName, const QString& command)
{
    QtCGraph* graph = getGraph(graphName);
    if (!graph) {
        qWarning() << "Graph not found:" << graphName;
        return TCL_ERROR;
    }
    
    int result = graph->eval(command);
    emit commandSent(graphName, command, result);
    return result;
}

int QtCGManager::broadcast(const QString& command)
{
    int failures = 0;
    for (auto it = m_graphs.begin(); it != m_graphs.end(); ++it) {
        if (it.value()) {
            int result = it.value()->eval(command);
            emit commandSent(it.key(), command, result);
            if (result != TCL_OK) {
                failures++;
            }
        }
    }
    return failures > 0 ? TCL_ERROR : TCL_OK;
}

int QtCGManager::sendToGroup(const QString& groupTag, const QString& command)
{
    QStringList members = m_groups.value(groupTag);
    if (members.isEmpty()) {
        qWarning() << "No graphs in group:" << groupTag;
        return TCL_ERROR;
    }
    
    int failures = 0;
    for (const QString& graphName : members) {
        if (send(graphName, command) != TCL_OK) {
            failures++;
        }
    }
    return failures > 0 ? TCL_ERROR : TCL_OK;
}

void QtCGManager::addToGroup(const QString& graphName, const QString& groupTag)
{
    if (!m_graphs.contains(graphName)) {
        qWarning() << "Graph not found:" << graphName;
        return;
    }
    
    if (!m_groups[groupTag].contains(graphName)) {
        m_groups[groupTag].append(graphName);
    }
}

void QtCGManager::removeFromGroup(const QString& graphName, const QString& groupTag)
{
    m_groups[groupTag].removeAll(graphName);
    
    // Clean up empty groups
    if (m_groups[groupTag].isEmpty()) {
        m_groups.remove(groupTag);
    }
}

QStringList QtCGManager::getGroupMembers(const QString& groupTag) const
{
    return m_groups.value(groupTag);
}

void QtCGManager::setSharedData(const QString& key, const QVariant& value)
{
    m_sharedData[key] = value;
}

QVariant QtCGManager::getSharedData(const QString& key) const
{
    return m_sharedData.value(key);
}

QString QtCGManager::generateUniqueName(const QString& prefix)
{
    return QString("%1_%2").arg(prefix).arg(++m_nameCounter);
}
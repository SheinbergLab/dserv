#ifndef QTCGMANAGER_HPP
#define QTCGMANAGER_HPP

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QPainter>

// Forward declarations
class QtCGraph;

class QtCGManager : public QObject
{
    Q_OBJECT

public:
    static QtCGManager& getInstance();
    
    // Graph registration
    QString registerGraph(const QString& name, QtCGraph* graph);
    bool unregisterGraph(const QString& name);
    
    // Graph access
    QtCGraph* getGraph(const QString& name) const;
    QList<QString> getAllGraphNames() const;
    QList<QtCGraph*> getAllGraphs() const;
    
    // Current graph tracking (for cgraph callbacks)
    void setCurrentGraph(QtCGraph* graph) { m_currentGraph = graph; }
    QtCGraph* getCurrentGraph() const { return m_currentGraph; }
    
    // Painting state (for static callbacks)
    void setCurrentPainter(QPainter* painter) { m_currentPainter = painter; }
    QPainter* getCurrentPainter() const { return m_currentPainter; }
    
    // Command routing
    int send(const QString& graphName, const QString& command);
    int broadcast(const QString& command);
    int sendToGroup(const QString& groupTag, const QString& command);
    
    // Group management
    void addToGroup(const QString& graphName, const QString& groupTag);
    void removeFromGroup(const QString& graphName, const QString& groupTag);
    QStringList getGroupMembers(const QString& groupTag) const;
    
    // Shared data
    void setSharedData(const QString& key, const QVariant& value);
    QVariant getSharedData(const QString& key) const;
    
signals:
    void graphRegistered(const QString& name, QtCGraph* graph);
    void graphUnregistered(const QString& name);
    void currentChanged(QtCGraph* graph);
    void commandSent(const QString& graphName, const QString& command, int result);
    void graphRemoved(const QString& name);
    
private:
    QtCGManager() = default;
    ~QtCGManager() = default;
    QtCGManager(const QtCGManager&) = delete;
    QtCGManager& operator=(const QtCGManager&) = delete;
    
    QMap<QString, QtCGraph*> m_graphs;
    QMap<QString, QStringList> m_groups;  // group -> list of graph names
    QMap<QString, QVariant> m_sharedData;
    
    QtCGraph* m_currentGraph = nullptr;
    QPainter* m_currentPainter = nullptr;
    
    QString generateUniqueName(const QString& prefix = "graph");
    int m_nameCounter = 0;
};

#endif // QTCGMANAGER_HPP
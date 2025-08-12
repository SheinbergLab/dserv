#pragma once

#include <QObject>
#include <QMap>
#include <QList>
#include <QMenu>

QT_BEGIN_NAMESPACE
class QMainWindow;
class QDockWidget;
class QAction;
class QWidget;
QT_END_NAMESPACE

class EssTerminalWidget;
class EssOutputConsole;
class EssDatapointTableWidget;
class EssEventTableWidget;
class EssHostDiscoveryWidget;
class EssExperimentControlWidget;
class EssScriptEditorWidget;
class EssStimDgWidget;
class EssEyeTouchVisualizerWidget;
class EssStateSystemWidget;
class DraggableTabWidget;
class EssGraphicsWidget;

class EssWorkspaceManager : public QObject
{
    Q_OBJECT

public:
    explicit EssWorkspaceManager(QMainWindow *mainWindow, QObject *parent = nullptr);
    ~EssWorkspaceManager();

    // Main setup
    void setupWorkspace();
    
    // Layout management
    void resetToDefaultLayout();
    void saveLayout();
    bool restoreLayout();
    
	void createCGraphWidget(const QString& name, const QString& title);
    void createGraphicsWidgetWithTemplate(const QString& name, 
    	const QString& templateName);

    void sendScriptToCurrentGraphicsWidget(const QString& script);
    
    // Dock visibility
    void setDockVisible(const QString &dockName, bool visible);
    bool isDockVisible(const QString &dockName) const;
    
    // Menu actions
    QList<QAction*> viewMenuActions() const;
     
signals:
    void statusMessage(const QString &message, int timeout = 0);
    
private:
    // Setup methods
    void createDocks();
    void applyDefaultLayout();
    void connectSignals();
    
    // Helper methods
    QWidget* getWidget(const QString &id) const;
    QWidget* createControlPanel();
    
	// CGraph support
	QDockWidget* m_cgraphDock;
	QMap<QString, EssGraphicsWidget*> m_cgraphs;
	QMenu* m_cgraphMenu;
    void updateCGraphMenu();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    
private slots:
	void onCGraphRemoved(const QString& name);
    void onCGraphTabDetached(QWidget* widget, const QString& title, const QPoint& globalPos);
    void onCGraphTabCloseRequested(int index);
    void detachCGraphTab(int index);
    void returnCGraphToTabs(QDockWidget* dock, const QString& name);

private:
    QMainWindow *m_mainWindow;
    
    // Created docks
    QMap<QString, QDockWidget*> m_docks;
    
    // Direct widget pointers
    EssTerminalWidget *m_terminal;
    EssOutputConsole *m_console;
    EssDatapointTableWidget *m_datapointTable;
    EssEventTableWidget *m_eventTable;
    EssHostDiscoveryWidget *m_hostDiscovery;
    EssExperimentControlWidget *m_experimentControl;
    EssScriptEditorWidget *m_scriptEditor;
    EssStimDgWidget *m_stimDgViewer;
    EssEyeTouchVisualizerWidget *m_eyeTouchVisualizer;
    EssStateSystemWidget *m_stateSystemWidget;
    DraggableTabWidget* m_cgraphTabWidget; 
};
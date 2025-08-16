#pragma once

#include <QObject>
#include <QMap>
#include <QList>
#include <QMenu>

#include "EssStandaloneWindow.h"

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
class EssBehavmonWidget;
class EssDgViewerWidget;

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
    void saveStandaloneWindows();
    void restoreStandaloneWindows();
    
    EssExperimentControlWidget* experimentControlWidget() const;    
    QWidget* getWidget(const QString &id) const;
     
	void createCGraphWidget(const QString& name, const QString& title);
    void createGraphicsWidgetWithTemplate(const QString& name, 
    	const QString& templateName);

    void sendScriptToCurrentGraphicsWidget(const QString& script);
    
    // Dock visibility
    void setDockVisible(const QString &dockName, bool visible);
    bool isDockVisible(const QString &dockName) const;
    
    // Menu actions
    QList<QAction*> viewMenuActions();

    // Stand alone window support
    void detachToStandalone(const QString& dockName, 
                           EssStandaloneWindow::WindowBehavior behavior = EssStandaloneWindow::UtilityWindow);
    void detachToStandalone(QDockWidget* dock, 
                           EssStandaloneWindow::WindowBehavior behavior = EssStandaloneWindow::UtilityWindow,
                           bool activateWindow = true);
    void returnFromStandalone(EssStandaloneWindow* window);
    
    // Convenience methods
    void detachEyeTouchVisualizer(EssStandaloneWindow::WindowBehavior behavior = EssStandaloneWindow::UtilityWindow);
    QList<EssStandaloneWindow*> standaloneWindows() const { return m_standaloneWindows; }
         
signals:
    void statusMessage(const QString &message, int timeout = 0);
    void standaloneStateChanged();    
private:
    // Setup methods
    void createDocks();
    void applyDefaultLayout();
    void clearCurrentLayout();
    bool validateAllDocks();
    void applySizeConstraints();
    void applyFloatingConstraints();    
    void connectSignals();
	void resetDockConstraints();    
	
    // Helper methods
    QWidget* createControlPanel();
    
	// CGraph support
	QDockWidget* m_cgraphDock;
	QMap<QString, EssGraphicsWidget*> m_cgraphs;
	QMenu* m_cgraphMenu;
    void updateCGraphMenu();

    // Stand alone window support
    QList<EssStandaloneWindow*> m_standaloneWindows;
    QMap<EssStandaloneWindow*, QString> m_standaloneToOriginalDock; // Track original dock names


protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    
private slots:
	void onCGraphRemoved(const QString& name);
    void onCGraphTabDetached(QWidget* widget, const QString& title, const QPoint& globalPos);
    void onCGraphTabCloseRequested(int index);
    void detachCGraphTab(int index);
    void returnCGraphToTabs(QDockWidget* dock, const QString& name);
 	void handleGraphicsFloatingRequest(const QString& name, bool floating);
 	
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
    EssBehavmonWidget *m_behavmonWidget; 
    EssDgViewerWidget *m_dgViewerWidget;
};
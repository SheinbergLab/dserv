// EssDgViewerWidget.h
#pragma once
#include <QWidget>
#include <QTabWidget>
#include <QToolBar>
#include <QHash>

class EssDynGroupViewer;
class QAction;
class QInputDialog;

extern "C" {
#include <df.h>
#include <dynio.h>
}

class EssDgViewerWidget : public QWidget {
    Q_OBJECT
    
public:
    explicit EssDgViewerWidget(QWidget* parent = nullptr);
    ~EssDgViewerWidget();
    
    // Main interface - show a DG in a new tab
    void showDynGroup(DYN_GROUP* dg, const QString& name);
    void showDynGroupFromTcl(const QString& dgName);
    
    // Tab management
    void closeCurrentTab();
    void closeAllTabs();
    
signals:
    void tabCountChanged(int count);
    
private slots:
    void onTabCloseRequested(int index);
    void onCurrentTabChanged(int index);
    void onShowDgRequested();
    void onShowDgFromTclRequested();
    void onCloseCurrentRequested();
    void onCloseAllRequested();
    void onExportCurrentRequested();
    
private:
    void setupUi();
    void setupToolbar();
    void updateToolbarState();
    void addPlaceholderTab();
    void removePlaceholderTab();
    QStringList getAvailableDgNames() const;
    
private:
    QTabWidget* m_tabWidget;
    QToolBar* m_toolbar;
    
    // Actions
    QAction* m_showDgAction;
    QAction* m_showDgFromTclAction;
    QAction* m_closeCurrentAction;
    QAction* m_closeAllAction;
    QAction* m_exportCurrentAction;
    
    int m_tabCounter;
    bool m_hasPlaceholder;
};

#pragma once

#include <QWidget>
#include <QMap>
#include <QString>

class EssCodeEditor;
class QTabWidget;
class QLabel;
class QToolBar;
class QAction;
class QComboBox;

/**
 * @brief Specialized script editor for ESS system scripts
 * 
 * Manages multiple script types (system, protocol, loaders, variants, stim)
 * in a tabbed interface. Automatically loads scripts from datapoints and
 * provides save functionality back to the ESS backend.
 */
class EssScriptEditorWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EssScriptEditorWidget(QWidget *parent = nullptr);
    ~EssScriptEditorWidget();

    // Script types matching ESS datapoints
    enum ScriptType {
        SystemScript,
        ProtocolScript,
        LoadersScript,
        VariantsScript,
        StimScript
    };
    
    // Load script content from datapoint
    void loadScript(ScriptType type, const QString &content);
    
    // Get script content
    QString getScriptContent(ScriptType type) const;
    
    // Check if any scripts are modified
    bool hasModifiedScripts() const;
    
    // Get list of modified scripts
    QList<ScriptType> modifiedScripts() const;
    
    // Save a specific script
    void saveScript(ScriptType type);
    
    // Script type utilities
    QString scriptTypeToString(ScriptType type) const;
    static QString scriptTypeToDatapoint(ScriptType type);
    
    // Disconnect handling
    bool confirmDisconnectWithUnsavedChanges();

signals:
    void scriptModified(ScriptType type, bool modified);
    void scriptSaved(ScriptType type);
    void statusMessage(const QString &message, int timeout = 0);
    void saveAllRequested();

public slots:
    void onDatapointReceived(const QString &name, const QString &content);
    void saveCurrentScript();
    void saveAllScripts();
    void reloadCurrentScript();
    void onSaveAllTriggered() { saveAllScripts(); }
    void clearAllScripts();
    void onDisconnected();
    
private slots:
    void onTabChanged(int index);
    void onScriptModified(bool modified);
    void onEditorSaveRequested();
    void onCursorPositionChanged(int line, int column);
    void updateGlobalActions();

    void onPushClicked();
    void onPullClicked();
    void onBranchChanged(int index);
    void updateGitStatus();
  
private:
    struct ScriptEditor {
        EssCodeEditor *editor;
        ScriptType type;
        QString datapointName;
        bool loaded;
    };

    void setupUi();
    void createGlobalToolbar();
    void createScriptTab(ScriptType type, const QString &tabName, const QString &datapointName);
    void connectSignals();
    void updateTabTitle(ScriptType type);
    void updateStatusBar();
    ScriptType scriptTypeFromDatapoint(const QString &name) const;
    ScriptType currentScriptType() const;
    
    // UI components
    QTabWidget *m_tabWidget;
    QToolBar *m_globalToolbar;
    QLabel *m_statusLabel;
    
    // Script editors
    QMap<ScriptType, ScriptEditor> m_scriptEditors;
    
    // Global actions
    QAction *m_saveAction;
    QAction *m_saveAllAction;
    QAction *m_reloadAction;

    QAction *m_pushAction;
    QAction *m_pullAction;
    QComboBox *m_branchCombo;
   
    // State
    int m_pendingSaves;

    // Git state
    QString m_currentBranch;
    QStringList m_availableBranches;
    bool m_isGitBusy;  
};
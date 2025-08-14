#pragma once
#include "EssDynGroupViewer.h"

class QLabel;

class EssStimDgWidget : public EssDynGroupViewer {
    Q_OBJECT
    
public:
    explicit EssStimDgWidget(QWidget *parent = nullptr);
    ~EssStimDgWidget();
    
    // Highlight specific columns important for stimulus
    void setHighlightColumns(const QStringList& columnNames);
    
    // Get current trial index if available
    int currentTrialIndex() const;

signals:
    void stimulusDataUpdated();
    void trialSelected(int trialIndex);

private:
    void customizeForStimulus();
    void connectToDataProcessor();
    void highlightImportantColumns();
    void updateStatistics();
    void applyColumnVisibility();
    void updateRowDetails(int row);
    
    // Position preservation for development mode
    struct ViewPosition {
        int currentRow = -1;
        int currentColumn = -1;
        int scrollX = 0;
        int scrollY = 0;
        bool isValid() const { return currentRow >= 0; }
        void reset() { currentRow = -1; currentColumn = -1; scrollX = 0; scrollY = 0; }
    };
    ViewPosition m_savedPosition;
    void saveCurrentPosition();
    void restorePosition();
    
private slots:
    void onStimDgReceived();
    void refreshStimDg();
    void showStatistics();
    void configureHighlighting();
    void showTrialContextMenu(const QPoint& pos);
    void findSimilarTrials(int referenceRow);
    void toggleFocusMode(bool enabled);
    void onHeaderClicked(int logicalIndex);
    
private:
    QStringList m_highlightColumns;
    QStringList m_allColumns;
    bool m_focusMode;
    QAction* m_focusModeAction;
    QLabel* m_statusLabel;
};
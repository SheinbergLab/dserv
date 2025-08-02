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
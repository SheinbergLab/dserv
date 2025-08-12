#ifndef ESSBEHAVMONWIDGET_H
#define ESSBEHAVMONWIDGET_H

#include "EssScriptableWidget.h"
#include <QTableWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QHeaderView>
#include <QGroupBox>
#include <QProgressBar>
#include <QPushButton>

/**
 * @brief Behavior monitoring widget - UI layout only
 * 
 * Provides visual layout for behavior monitoring with:
 * - General performance display (% correct, % complete, trial count)
 * - Sortable performance table
 * - Sort selection controls
 * - All business logic handled via Tcl scripts
 */
class EssBehavmonWidget : public EssScriptableWidget
{
    Q_OBJECT

public:
    explicit EssBehavmonWidget(const QString& name = QString(), QWidget* parent = nullptr);
    virtual ~EssBehavmonWidget();

    QString getWidgetTypeName() const override { return "EssBehavmonWidget"; }

protected:
    void registerCustomCommands() override;
    QWidget* createMainWidget() override;

private:
    void setupGeneralPerformanceArea();
    void setupDetailedPerformanceArea();
    void setupControlsArea();
    
private:
    // UI Components
    QWidget* m_mainWidget;
    
    // General performance area
    QGroupBox* m_generalGroup;
    QLabel* m_percentCorrectLabel;
    QLabel* m_percentCompleteLabel;
    QLabel* m_totalTrialsLabel;
    QProgressBar* m_correctProgress;
    QProgressBar* m_completeProgress;
    
    // Detailed performance area
    QGroupBox* m_detailedGroup;
    QTableWidget* m_detailedTable;
    QComboBox* m_primarySortCombo;
    QComboBox* m_secondarySortCombo;
    QLabel* m_sortLabel;
    
    // Controls
    QGroupBox* m_controlsGroup;
    QPushButton* m_resetButton;
    QPushButton* m_exportButton;
    
    // Tcl command implementations - just UI updates
    static int tcl_set_general_performance(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_set_performance_table(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_set_sort_options(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_get_sort_selection(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_clear_table(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);

signals:
    void sortSelectionChanged(const QString& primary, const QString& secondary);
    void resetRequested();
    void exportRequested();
};

#endif // ESSBEHAVMONWIDGET_H
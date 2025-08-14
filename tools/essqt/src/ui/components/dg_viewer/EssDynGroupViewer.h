#pragma once
#include <QWidget>
#include <QTreeWidget>
#include <QTableWidget>
#include <QStackedWidget>
#include <QSplitter>
#include <QToolBar>
#include <memory>

extern "C" {
#include <df.h>
#include <dynio.h>
}

class QLabel;

class EssDynGroupViewer : public QWidget {
    Q_OBJECT

public:
    enum ViewMode {
        TableView,
        TreeView
    };

    explicit EssDynGroupViewer(QWidget *parent = nullptr);
    ~EssDynGroupViewer();
    
    void setDynGroup(DYN_GROUP* dg, const QString& name = QString());
    void clear();
    
    // View mode control
    void setViewMode(ViewMode mode);
    ViewMode viewMode() const { return m_viewMode; }
    
    // Row details control
    void setShowRowDetails(bool show);
    bool isShowingRowDetails() const { return m_showRowDetails; }

    QTableWidget* tableWidget() { return m_tableWidget; }
    QTreeWidget* treeWidget() { return m_treeWidget; }
    
signals:
    void cellDoubleClicked(int row, int col, const QString& listName);
    void itemSelected(const QString& path, const QVariant& value);

protected:
    // Protected so subclasses can access
    QToolBar* toolbar() { return m_toolbar; }

    
    // Const versions for const methods
    const QTableWidget* tableWidget() const { return m_tableWidget; }
    const QTreeWidget* treeWidget() const { return m_treeWidget; }
    
    // Protected for subclass access
    void updateRowDetails(int row);

private slots:
    void onTableCellDoubleClicked(int row, int column);
    void onViewModeChanged();
    void onTreeItemClicked(QTreeWidgetItem* item, int column);
    void onTreeItemExpanded(QTreeWidgetItem* item);
    void exportTableToCSV();
    void onTableRowChanged(int currentRow, int currentColumn, int previousRow, int previousColumn);

private:
    void setupUi();
    void populateTable();
    void populateTree();
    void populateListItem(QTreeWidgetItem* parent, DYN_LIST* dl, int row);
    QString formatCellValue(DYN_LIST* dl, int row) const;
    QString getDataTypeString(int dataType) const;
    QIcon getTypeIcon(int dataType) const;
    void showNestedListDialog(DYN_LIST* dl, const QString& title);
    
    // Row details methods
    void setupRowDetailsPane();
    void populateRowDetailsTree(int row);
    
    // UI elements
    QStackedWidget* m_stackedWidget;
    QTableWidget* m_tableWidget;
    QTreeWidget* m_treeWidget;
    QToolBar* m_toolbar;
    QAction* m_tableViewAction;
    QAction* m_treeViewAction;
    QAction* m_rowDetailsAction;
    
    // Row details view
    QSplitter* m_tableSplitter;
    QTreeWidget* m_rowDetailsTree;
    QWidget* m_rowDetailsPane;
    QLabel* m_rowDetailsLabel;
    bool m_showRowDetails;
    int m_currentDetailRow;
    
    // Data
    DYN_GROUP* m_dynGroup;
    QString m_groupName;
    bool m_ownsDynGroup;
    ViewMode m_viewMode;
};
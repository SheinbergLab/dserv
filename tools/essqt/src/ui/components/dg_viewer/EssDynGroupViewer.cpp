#include "EssDynGroupViewer.h"
#include "core/EssApplication.h"
#include "core/EssCommandInterface.h"
#include "console/EssOutputConsole.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QActionGroup>
#include <QHeaderView>
#include <QMenu>
#include <QDialog>
#include <QDialogButtonBox>
#include <QClipboard>
#include <QApplication>
#include <QMessageBox>
#include <QFileDialog>
#include <QTextStream>
#include <QPushButton>
#include <QLabel>
#include <QSplitter>
#include <algorithm>
#include <functional>

extern "C" {
#include "dlfuncs.h"
}

EssDynGroupViewer::EssDynGroupViewer(QWidget *parent)
    : QWidget(parent)
    , m_dynGroup(nullptr)
    , m_ownsDynGroup(false)
    , m_liveUpdate(false)
    , m_viewMode(TableView)
    , m_showRowDetails(false)
    , m_currentDetailRow(-1)
    , m_updateTimer(new QTimer(this))
{
    setupUi();
    m_updateTimer->setInterval(1000);
    connect(m_updateTimer, &QTimer::timeout, this, &EssDynGroupViewer::refreshFromTcl);
}

EssDynGroupViewer::~EssDynGroupViewer()
{
    if (m_dynGroup && m_ownsDynGroup) {
        dfuFreeDynGroup(m_dynGroup);
    }
}

void EssDynGroupViewer::setupUi()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    
    // Toolbar
    m_toolbar = new QToolBar();
    m_toolbar->setIconSize(QSize(16, 16));
    
    // View mode toggle
    QActionGroup* viewGroup = new QActionGroup(this);
    
    m_tableViewAction = m_toolbar->addAction(QIcon::fromTheme("view-form-table"), "Table View");
    m_tableViewAction->setCheckable(true);
    m_tableViewAction->setChecked(true);
    m_tableViewAction->setToolTip("Show as 2D table (rows × columns)");
    viewGroup->addAction(m_tableViewAction);
    
    m_treeViewAction = m_toolbar->addAction(QIcon::fromTheme("view-list-tree"), "Tree View");
    m_treeViewAction->setCheckable(true);
    m_treeViewAction->setToolTip("Show as expandable tree");
    viewGroup->addAction(m_treeViewAction);
    
    connect(viewGroup, &QActionGroup::triggered, this, &EssDynGroupViewer::onViewModeChanged);
    
    m_toolbar->addSeparator();
    
    // Row details toggle (only visible in table view)
    m_rowDetailsAction = m_toolbar->addAction(QIcon::fromTheme("view-split-top-bottom"), "Show Row Details");
    m_rowDetailsAction->setCheckable(true);
    m_rowDetailsAction->setChecked(false);
    m_rowDetailsAction->setToolTip("Show detailed view of selected row");
    connect(m_rowDetailsAction, &QAction::toggled, this, &EssDynGroupViewer::setShowRowDetails);
    
    m_toolbar->addSeparator();
    
    // Other actions
    m_toolbar->addAction(QIcon::fromTheme("view-refresh"), "Refresh", this, &EssDynGroupViewer::refreshFromTcl);
    
    QAction* liveAction = m_toolbar->addAction("Live Update");
    liveAction->setCheckable(true);
    connect(liveAction, &QAction::toggled, this, &EssDynGroupViewer::setLiveUpdate);
    
    m_toolbar->addSeparator();
    
    // Export action
    m_toolbar->addAction(QIcon::fromTheme("document-save"), "Export", this, [this]() {
        exportTableToCSV();
    });
    
    layout->addWidget(m_toolbar);
    
    // Stacked widget for views
    m_stackedWidget = new QStackedWidget();
    
    // Table view with optional row details
    QWidget* tableContainer = new QWidget();
    QVBoxLayout* tableLayout = new QVBoxLayout(tableContainer);
    tableLayout->setContentsMargins(0, 0, 0, 0);
    
    m_tableSplitter = new QSplitter(Qt::Vertical);
    
    // Main table
    m_tableWidget = new QTableWidget();
    m_tableWidget->setAlternatingRowColors(true);
    m_tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableWidget->setSortingEnabled(false); // Don't sort trial data by default
    m_tableWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    
    connect(m_tableWidget, &QTableWidget::cellDoubleClicked, 
            this, &EssDynGroupViewer::onTableCellDoubleClicked);
    
    connect(m_tableWidget, &QTableWidget::currentCellChanged,
            this, &EssDynGroupViewer::onTableRowChanged);
    
    connect(m_tableWidget, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTableWidgetItem* item = m_tableWidget->itemAt(pos);
        if (!item) return;
        
        QMenu menu;
        menu.addAction(QIcon::fromTheme("edit-copy"), "Copy Cell", [this, item]() {
            QApplication::clipboard()->setText(item->text());
        });
        
        menu.addAction("Copy Row", [this, item]() {
            int row = item->row();
            QStringList values;
            for (int col = 0; col < m_tableWidget->columnCount(); col++) {
                if (auto* cellItem = m_tableWidget->item(row, col)) {
                    values << cellItem->text();
                }
            }
            QApplication::clipboard()->setText(values.join("\t"));
        });
        
        menu.addAction("Copy Column", [this, item]() {
            int col = item->column();
            QStringList values;
            for (int row = 0; row < m_tableWidget->rowCount(); row++) {
                if (auto* cellItem = m_tableWidget->item(row, col)) {
                    values << cellItem->text();
                }
            }
            QApplication::clipboard()->setText(values.join("\n"));
        });
        
        menu.addSeparator();
        
        // If it's a nested list, offer to view it
        void* listPtr = item->data(Qt::UserRole).value<void*>();
        if (listPtr) {
            menu.addAction(QIcon::fromTheme("zoom-in"), "View Nested List", [this, item]() {
                onTableCellDoubleClicked(item->row(), item->column());
            });
        }
        
        menu.exec(m_tableWidget->mapToGlobal(pos));
    });
    
    m_tableSplitter->addWidget(m_tableWidget);
    
    // Row details pane
    setupRowDetailsPane();
    m_tableSplitter->addWidget(m_rowDetailsPane);
    m_tableSplitter->setStretchFactor(0, 3); // Table gets 3/4 of space
    m_tableSplitter->setStretchFactor(1, 1); // Details gets 1/4 of space
    
    // Hide details pane initially
    m_rowDetailsPane->setVisible(false);
    
    tableLayout->addWidget(m_tableSplitter);
    m_stackedWidget->addWidget(tableContainer);
    
    // Tree view setup
    m_treeWidget = new QTreeWidget();
    m_treeWidget->setColumnCount(3);
    m_treeWidget->setHeaderLabels({"Name/Index", "Type", "Value"});
    m_treeWidget->setAlternatingRowColors(true);
    m_treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    
    // Tree view column sizing
    m_treeWidget->header()->setStretchLastSection(true);
    m_treeWidget->setColumnWidth(0, 200);
    m_treeWidget->setColumnWidth(1, 100);
    
    connect(m_treeWidget, &QTreeWidget::itemClicked, this, &EssDynGroupViewer::onTreeItemClicked);
    connect(m_treeWidget, &QTreeWidget::itemExpanded, this, &EssDynGroupViewer::onTreeItemExpanded);
    
    connect(m_treeWidget, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = m_treeWidget->itemAt(pos);
        if (!item) return;
        
        QMenu menu;
        menu.addAction(QIcon::fromTheme("edit-copy"), "Copy Value", [this, item]() {
            QApplication::clipboard()->setText(item->text(2));
        });
        menu.addAction("Copy Path", [this, item]() {
            QString path = item->data(0, Qt::UserRole).toString();
            QApplication::clipboard()->setText(path);
        });
        menu.exec(m_treeWidget->mapToGlobal(pos));
    });
    
    m_stackedWidget->addWidget(m_treeWidget);
    
    layout->addWidget(m_stackedWidget);
}

void EssDynGroupViewer::setupRowDetailsPane()
{
    m_rowDetailsPane = new QWidget();
    QVBoxLayout* detailsLayout = new QVBoxLayout(m_rowDetailsPane);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(0);
    
    // Header bar with label and controls
    QWidget* headerBar = new QWidget();
    headerBar->setStyleSheet("QWidget { background-color: #f0f0f0; }");
    QHBoxLayout* headerLayout = new QHBoxLayout(headerBar);
    headerLayout->setContentsMargins(5, 5, 5, 5);
    
    // Header label
    m_rowDetailsLabel = new QLabel("Row Details");
    m_rowDetailsLabel->setStyleSheet("QLabel { font-weight: bold; }");
    headerLayout->addWidget(m_rowDetailsLabel);
    
    headerLayout->addStretch();
    
    // Expand/Collapse buttons
    QPushButton* expandAllBtn = new QPushButton("Expand All");
    expandAllBtn->setMaximumHeight(22);
    expandAllBtn->setToolTip("Expand all nested items");
    connect(expandAllBtn, &QPushButton::clicked, [this]() {
        m_rowDetailsTree->expandAll();
    });
    headerLayout->addWidget(expandAllBtn);
    
    QPushButton* collapseAllBtn = new QPushButton("Collapse All");
    collapseAllBtn->setMaximumHeight(22);
    collapseAllBtn->setToolTip("Collapse all nested items");
    connect(collapseAllBtn, &QPushButton::clicked, [this]() {
        m_rowDetailsTree->collapseAll();
    });
    headerLayout->addWidget(collapseAllBtn);
    
    detailsLayout->addWidget(headerBar);
    
    // Tree widget for row details
    m_rowDetailsTree = new QTreeWidget();
    m_rowDetailsTree->setHeaderLabels({"Column", "Type", "Value"});
    m_rowDetailsTree->setAlternatingRowColors(true);
    m_rowDetailsTree->header()->setStretchLastSection(true);
    m_rowDetailsTree->setRootIsDecorated(true);
    
    // Context menu for row details tree
    m_rowDetailsTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_rowDetailsTree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = m_rowDetailsTree->itemAt(pos);
        if (!item) return;
        
        QMenu menu;
        menu.addAction(QIcon::fromTheme("edit-copy"), "Copy Value", [this, item]() {
            QApplication::clipboard()->setText(item->text(2));
        });
        
        menu.addSeparator();
        
        // Add expand/collapse options for items with children
        if (item->childCount() > 0) {
            menu.addAction("Expand This", [item]() {
                item->setExpanded(true);
            });
            menu.addAction("Collapse This", [item]() {
                item->setExpanded(false);
            });
            
            menu.addSeparator();
            
            menu.addAction("Expand All Children", [item]() {
                // Recursively expand this item and all its children
                std::function<void(QTreeWidgetItem*)> expandRecursive;
                expandRecursive = [&expandRecursive](QTreeWidgetItem* node) {
                    node->setExpanded(true);
                    for (int i = 0; i < node->childCount(); i++) {
                        expandRecursive(node->child(i));
                    }
                };
                expandRecursive(item);
            });
            
            menu.addAction("Collapse All Children", [item]() {
                // Recursively collapse this item and all its children
                std::function<void(QTreeWidgetItem*)> collapseRecursive;
                collapseRecursive = [&collapseRecursive](QTreeWidgetItem* node) {
                    for (int i = 0; i < node->childCount(); i++) {
                        collapseRecursive(node->child(i));
                    }
                    node->setExpanded(false);
                };
                collapseRecursive(item);
            });
        }
        
        menu.exec(m_rowDetailsTree->mapToGlobal(pos));
    });
    
    detailsLayout->addWidget(m_rowDetailsTree);
}

void EssDynGroupViewer::setViewMode(ViewMode mode)
{
    m_viewMode = mode;
    m_stackedWidget->setCurrentIndex(mode);
    m_tableViewAction->setChecked(mode == TableView);
    m_treeViewAction->setChecked(mode == TreeView);
    
    // Row details action only enabled in table view
    m_rowDetailsAction->setVisible(mode == TableView);
    if (mode != TableView) {
        setShowRowDetails(false);
    }
    
    // Re-populate the view when switching modes
    if (m_dynGroup) {
        if (mode == TableView) {
            populateTable();
        } else {
            populateTree();
        }
    }
}

void EssDynGroupViewer::onViewModeChanged()
{
    if (m_tableViewAction->isChecked()) {
        setViewMode(TableView);
    } else {
        setViewMode(TreeView);
    }
}

void EssDynGroupViewer::setDynGroup(DYN_GROUP* dg, const QString& name)
{
    if (m_dynGroup && m_ownsDynGroup) {
        dfuFreeDynGroup(m_dynGroup);
    }
    
    if (dg) {
        // Always create a deep copy for safety
        m_dynGroup = dfuCopyDynGroup(dg, const_cast<char*>(name.toUtf8().constData()));
        m_ownsDynGroup = true;
    } else {
        m_dynGroup = nullptr;
        m_ownsDynGroup = false;
    }
    
    m_groupName = name.isEmpty() ? 
        (m_dynGroup && DYN_GROUP_NAME(m_dynGroup) ? QString::fromUtf8(DYN_GROUP_NAME(m_dynGroup)) : "Unnamed") : 
        name;
    
    // Debug output
    if (m_dynGroup) {
        EssConsoleManager::instance()->logDebug(
            QString("Setting DynGroup '%1' with %2 lists").arg(m_groupName).arg(DYN_GROUP_N(m_dynGroup)),
            "DynGroupViewer"
        );
    }
    
    // Clear row details
    m_currentDetailRow = -1;
    if (m_rowDetailsTree) {
        m_rowDetailsTree->clear();
    }
    
    // Populate appropriate view
    if (m_viewMode == TableView) {
        populateTable();
    } else {
        populateTree();
    }
}

void EssDynGroupViewer::clear()
{
    if (m_dynGroup && m_ownsDynGroup) {
        dfuFreeDynGroup(m_dynGroup);
    }
    m_dynGroup = nullptr;
    m_groupName.clear();
    m_ownsDynGroup = false;
    
    m_tableWidget->clear();
    m_tableWidget->setRowCount(0);
    m_tableWidget->setColumnCount(0);
    
    m_treeWidget->clear();
    
    if (m_rowDetailsTree) {
        m_rowDetailsTree->clear();
        m_rowDetailsLabel->setText("Row Details");
    }
    m_currentDetailRow = -1;
}

void EssDynGroupViewer::populateTable()
{
    m_tableWidget->clear();
    m_tableWidget->setRowCount(0);
    m_tableWidget->setColumnCount(0);
    
    if (!m_dynGroup) return;
    
    int numLists = DYN_GROUP_N(m_dynGroup);
    if (numLists == 0) return;
    
    // Find the maximum number of rows across all lists
    int maxRows = 0;
    for (int i = 0; i < numLists; i++) {
        DYN_LIST* dl = DYN_GROUP_LIST(m_dynGroup, i);
        maxRows = std::max(maxRows, DYN_LIST_N(dl));
    }
    
    // Setup table
    m_tableWidget->setRowCount(maxRows);
    m_tableWidget->setColumnCount(numLists);
    
    // Column headers from list names
    QStringList headers;
    for (int col = 0; col < numLists; col++) {
        DYN_LIST* dl = DYN_GROUP_LIST(m_dynGroup, col);
        QString listName = QString::fromUtf8(DYN_LIST_NAME(dl));
        headers << listName;
    }
    m_tableWidget->setHorizontalHeaderLabels(headers);
    
    // Populate cells
    for (int col = 0; col < numLists; col++) {
        DYN_LIST* dl = DYN_GROUP_LIST(m_dynGroup, col);
        int listSize = DYN_LIST_N(dl);
        int dataType = DYN_LIST_DATATYPE(dl);
        
        // Set column header tooltip with type info
        QTableWidgetItem* headerItem = m_tableWidget->horizontalHeaderItem(col);
        if (headerItem) {
            headerItem->setToolTip(QString("Type: %1\nSize: %2")
                .arg(getDataTypeString(dataType))
                .arg(listSize));
        }
        
        for (int row = 0; row < maxRows; row++) {
            QTableWidgetItem* item = new QTableWidgetItem();
            
            if (row < listSize) {
                QString cellText = formatCellValue(dl, row);
                item->setText(cellText);
                
                // Special formatting for different types
                if (dataType == DF_LIST) {
                    item->setBackground(QColor(240, 240, 255)); // Light blue for nested lists
                    item->setToolTip("Double-click to view nested list");
                    
                    // Store the DYN_LIST pointer for later access
                    DYN_LIST** lists = (DYN_LIST**)DYN_LIST_VALS(dl);
                    item->setData(Qt::UserRole, QVariant::fromValue((void*)lists[row]));
                } else if (dataType == DF_FLOAT || dataType == DF_LONG || dataType == DF_SHORT) {
                    // Right-align numeric data
                    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                }
            } else {
                // Empty cell for shorter lists
                item->setText("");
                item->setBackground(QColor(250, 250, 250)); // Very light gray
            }
            
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            m_tableWidget->setItem(row, col, item);
        }
    }
    
    // Auto-resize columns to content (with reasonable limits)
    for (int col = 0; col < numLists; col++) {
        m_tableWidget->resizeColumnToContents(col);
        int width = m_tableWidget->columnWidth(col);
        // Set reasonable min/max widths
        width = std::max(width, 50);
        width = std::min(width, 200);
        m_tableWidget->setColumnWidth(col, width);
    }
    
    // Add row numbers
    for (int row = 0; row < maxRows; row++) {
        m_tableWidget->setVerticalHeaderItem(row, new QTableWidgetItem(QString::number(row)));
    }
}

void EssDynGroupViewer::populateTree()
{
    m_treeWidget->clear();
    
    if (!m_dynGroup) {
        EssConsoleManager::instance()->logDebug("No DynGroup to populate tree", "DynGroupViewer");
        return;
    }
    
    EssConsoleManager::instance()->logDebug(
        QString("Populating tree with DynGroup '%1', %2 lists").arg(m_groupName).arg(DYN_GROUP_N(m_dynGroup)),
        "DynGroupViewer"
    );
    
    // Create root item for the group
    QTreeWidgetItem* root = new QTreeWidgetItem(m_treeWidget);
    root->setText(0, m_groupName);
    root->setText(1, QString("DynGroup[%1]").arg(DYN_GROUP_N(m_dynGroup)));
    root->setIcon(0, getTypeIcon(DF_LIST)); // Use DF_LIST for group icon
    root->setExpanded(true);
    root->setData(0, Qt::UserRole, m_groupName);
    
    // Add each DYN_LIST as a child
    for (int i = 0; i < DYN_GROUP_N(m_dynGroup); i++) {
        DYN_LIST* dl = DYN_GROUP_LIST(m_dynGroup, i);
        if (!dl) continue;
        
        QString listName = QString::fromUtf8(DYN_LIST_NAME(dl));
        int listSize = DYN_LIST_N(dl);
        int dataType = DYN_LIST_DATATYPE(dl);
        
        QTreeWidgetItem* listItem = new QTreeWidgetItem(root);
        listItem->setText(0, listName);
        listItem->setText(1, QString("%1[%2]").arg(getDataTypeString(dataType)).arg(listSize));
        listItem->setIcon(0, getTypeIcon(dataType));
        listItem->setData(0, Qt::UserRole, QString("%1.%2").arg(m_groupName).arg(listName));
        
        // For small lists or non-list types, show values directly
        if (dataType != DF_LIST && listSize <= 10) {
            // Show all values in the value column
            QStringList values;
            for (int j = 0; j < listSize; j++) {
                values << formatCellValue(dl, j);
            }
            listItem->setText(2, values.join(", "));
        } else if (dataType != DF_LIST && listSize > 10) {
            // For larger non-list types, show first few values
            QStringList values;
            for (int j = 0; j < 3 && j < listSize; j++) {
                values << formatCellValue(dl, j);
            }
            if (listSize > 3) {
                values << "...";
            }
            listItem->setText(2, QString("[%1 values] %2").arg(listSize).arg(values.join(", ")));
        } else {
            // For list types, add expandable children
            listItem->setText(2, QString("<%1 items>").arg(listSize));
            
            // Add placeholder child to make it expandable
            if (listSize > 0) {
                QTreeWidgetItem* placeholder = new QTreeWidgetItem(listItem);
                placeholder->setText(0, "Loading...");
                placeholder->setData(0, Qt::UserRole + 1, true); // Mark as placeholder
                placeholder->setData(0, Qt::UserRole + 2, QVariant::fromValue((void*)dl)); // Store DYN_LIST pointer
            }
        }
    }
    
    // Force update
    m_treeWidget->update();
}

void EssDynGroupViewer::onTreeItemClicked(QTreeWidgetItem* item, int column)
{
    QString path = item->data(0, Qt::UserRole).toString();
    QString value = item->text(2);
    emit itemSelected(path, value);
}

void EssDynGroupViewer::onTreeItemExpanded(QTreeWidgetItem* item)
{
    // Check if this item has a placeholder child
    if (item->childCount() == 1 && item->child(0)->data(0, Qt::UserRole + 1).toBool()) {
        // Get the DYN_LIST this item represents from the placeholder
        void* dlPtr = item->child(0)->data(0, Qt::UserRole + 2).value<void*>();
        
        // Remove placeholder
        delete item->takeChild(0);
        
        if (dlPtr) {
            DYN_LIST* dl = static_cast<DYN_LIST*>(dlPtr);
            QString parentPath = item->data(0, Qt::UserRole).toString();
            
            // Populate children
            for (int j = 0; j < DYN_LIST_N(dl); j++) {
                populateListItem(item, dl, j);
            }
        }
    }
}

void EssDynGroupViewer::populateListItem(QTreeWidgetItem* parent, DYN_LIST* dl, int row)
{
    QString parentPath = parent->data(0, Qt::UserRole).toString();
    QString indexStr = QString("[%1]").arg(row);
    QString fullPath = parentPath + indexStr;
    
    QTreeWidgetItem* item = new QTreeWidgetItem(parent);
    item->setText(0, indexStr);
    item->setData(0, Qt::UserRole, fullPath);
    
    int dataType = DYN_LIST_DATATYPE(dl);
    
    if (dataType == DF_LIST) {
        // Nested list
        DYN_LIST** lists = (DYN_LIST**)DYN_LIST_VALS(dl);
        DYN_LIST* sublist = lists[row];
        
        if (sublist) {
            int subType = DYN_LIST_DATATYPE(sublist);
            int subSize = DYN_LIST_N(sublist);
            
            item->setText(1, QString("%1[%2]").arg(getDataTypeString(subType)).arg(subSize));
            item->setIcon(0, getTypeIcon(subType));
            
            if (subSize <= 5 && subType != DF_LIST) {
                // Show values inline for small lists
                QStringList values;
                for (int i = 0; i < subSize; i++) {
                    values << formatCellValue(sublist, i);
                }
                item->setText(2, values.join(", "));
            } else {
                item->setText(2, QString("<%1 items>").arg(subSize));
                
                // Add placeholder for lazy loading
                if (subSize > 0) {
                    QTreeWidgetItem* placeholder = new QTreeWidgetItem(item);
                    placeholder->setText(0, "Loading...");
                    placeholder->setData(0, Qt::UserRole + 1, true);
                    placeholder->setData(0, Qt::UserRole + 2, QVariant::fromValue((void*)sublist));
                }
            }
        }
    } else {
        // Scalar value
        item->setText(1, getDataTypeString(dataType));
        item->setText(2, formatCellValue(dl, row));
        item->setIcon(0, getTypeIcon(dataType));
    }
}

void EssDynGroupViewer::onTableCellDoubleClicked(int row, int column)
{
    QTableWidgetItem* item = m_tableWidget->item(row, column);
    if (!item) return;
    
    // Check if this is a nested list
    void* listPtr = item->data(Qt::UserRole).value<void*>();
    if (listPtr) {
        DYN_LIST* dl = static_cast<DYN_LIST*>(listPtr);
        QString listName = m_tableWidget->horizontalHeaderItem(column)->text();
        showNestedListDialog(dl, QString("%1[%2]").arg(listName).arg(row));
    }
    
    emit cellDoubleClicked(row, column, m_tableWidget->horizontalHeaderItem(column)->text());
}

void EssDynGroupViewer::showNestedListDialog(DYN_LIST* dl, const QString& title)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QString("Nested List: %1").arg(title));
    dialog.resize(600, 400);
    
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    
    // Create a table for the nested list
    QTableWidget* table = new QTableWidget();
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    
    int dataType = DYN_LIST_DATATYPE(dl);
    int listSize = DYN_LIST_N(dl);
    
    if (dataType == DF_LIST) {
        // Nested list of lists - show as table
        DYN_LIST** lists = (DYN_LIST**)DYN_LIST_VALS(dl);
        
        // Find max size
        int maxCols = 0;
        for (int i = 0; i < listSize; i++) {
            if (lists[i]) {
                maxCols = std::max(maxCols, DYN_LIST_N(lists[i]));
            }
        }
        
        table->setRowCount(listSize);
        table->setColumnCount(maxCols + 1); // +1 for type column
        
        QStringList headers;
        headers << "Type";
        for (int i = 0; i < maxCols; i++) {
            headers << QString("[%1]").arg(i);
        }
        table->setHorizontalHeaderLabels(headers);
        
        for (int row = 0; row < listSize; row++) {
            DYN_LIST* sublist = lists[row];
            if (sublist) {
                // Type column
                table->setItem(row, 0, new QTableWidgetItem(
                    QString("%1[%2]").arg(getDataTypeString(DYN_LIST_DATATYPE(sublist)))
                                     .arg(DYN_LIST_N(sublist))));
                
                // Data columns
                for (int col = 0; col < DYN_LIST_N(sublist) && col < maxCols; col++) {
                    table->setItem(row, col + 1, new QTableWidgetItem(formatCellValue(sublist, col)));
                }
            }
        }
    } else {
        // Simple list - show as single column
        table->setRowCount(listSize);
        table->setColumnCount(2);
        table->setHorizontalHeaderLabels({"Index", "Value"});
        
        for (int i = 0; i < listSize; i++) {
            table->setItem(i, 0, new QTableWidgetItem(QString::number(i)));
            table->setItem(i, 1, new QTableWidgetItem(formatCellValue(dl, i)));
        }
    }
    
    table->resizeColumnsToContents();
    layout->addWidget(table);
    
    // Add copy button
    QPushButton* copyButton = new QPushButton("Copy to Clipboard");
    connect(copyButton, &QPushButton::clicked, [table]() {
        QString text;
        QTextStream stream(&text);
        
        // Headers
        for (int col = 0; col < table->columnCount(); col++) {
            if (col > 0) stream << "\t";
            stream << table->horizontalHeaderItem(col)->text();
        }
        stream << "\n";
        
        // Data
        for (int row = 0; row < table->rowCount(); row++) {
            for (int col = 0; col < table->columnCount(); col++) {
                if (col > 0) stream << "\t";
                if (auto* item = table->item(row, col)) {
                    stream << item->text();
                }
            }
            stream << "\n";
        }
        
        QApplication::clipboard()->setText(text);
    });
    
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    buttonLayout->addWidget(copyButton);
    
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    buttonLayout->addWidget(buttons);
    
    layout->addLayout(buttonLayout);
    
    dialog.exec();
}

QString EssDynGroupViewer::formatCellValue(DYN_LIST* dl, int row) const
{
    if (!dl || row >= DYN_LIST_N(dl)) {
        return QString();
    }
    
    switch (DYN_LIST_DATATYPE(dl)) {
        case DF_LONG: {
            int* vals = (int*)DYN_LIST_VALS(dl);
            return QString::number(vals[row]);
        }
        case DF_SHORT: {
            short* vals = (short*)DYN_LIST_VALS(dl);
            return QString::number(vals[row]);
        }
        case DF_FLOAT: {
            float* vals = (float*)DYN_LIST_VALS(dl);
            return QString::number(vals[row], 'g', 6);
        }
        case DF_CHAR: {
            char* vals = (char*)DYN_LIST_VALS(dl);
            return QString::number(vals[row]);
        }
        case DF_STRING: {
            char** vals = (char**)DYN_LIST_VALS(dl);
            return QString::fromUtf8(vals[row]);
        }
        case DF_LIST: {
            DYN_LIST** lists = (DYN_LIST**)DYN_LIST_VALS(dl);
            DYN_LIST* sublist = lists[row];
            if (sublist) {
                return QString("<%1 × %2>").arg(getDataTypeString(DYN_LIST_DATATYPE(sublist)))
                                           .arg(DYN_LIST_N(sublist));
            }
            return "<null>";
        }
        default:
            return "?";
    }
}

QString EssDynGroupViewer::getDataTypeString(int dataType) const
{
    switch (dataType) {
        case DF_LONG: return "long";
        case DF_SHORT: return "short";
        case DF_FLOAT: return "float";
        case DF_CHAR: return "char";
        case DF_STRING: return "string";
        case DF_LIST: return "list";
        default: return "unknown";
    }
}

QIcon EssDynGroupViewer::getTypeIcon(int dataType) const
{
    switch (dataType) {
        case DF_LONG:
        case DF_SHORT:
        case DF_FLOAT:
            return QIcon::fromTheme("code-variable", QIcon(":/icons/number.png"));
        case DF_STRING:
            return QIcon::fromTheme("text-x-generic", QIcon(":/icons/text.png"));
        case DF_LIST:
            return QIcon::fromTheme("x-office-spreadsheet", QIcon(":/icons/table.png")); // Spreadsheet icon for lists/groups
        default:
            return QIcon();
    }
}

void EssDynGroupViewer::refreshFromTcl()
{
    if (m_groupName.isEmpty()) return;
    
    auto* app = EssApplication::instance();
    if (!app) return;
    
    auto* cmdInterface = app->commandInterface();
    if (!cmdInterface) return;
    
    Tcl_Interp* interp = cmdInterface->tclInterp();
    DYN_GROUP* dg = nullptr;
    
    if (tclFindDynGroup(interp, m_groupName.toUtf8().data(), &dg) == TCL_OK && dg) {
        setDynGroup(dg, m_groupName);
    } else {
        // DG no longer exists in Tcl - clear our display
        clear();
        EssConsoleManager::instance()->logWarning(
            QString("DynGroup '%1' no longer exists in Tcl").arg(m_groupName),
            "DynGroupViewer"
        );
    }
}

void EssDynGroupViewer::setLiveUpdate(bool enabled)
{
    m_liveUpdate = enabled;
    if (enabled) {
        m_updateTimer->start();
    } else {
        m_updateTimer->stop();
    }
}

void EssDynGroupViewer::exportTableToCSV()
{
    if (!m_dynGroup || m_viewMode != TableView) {
        QMessageBox::information(this, "Export", "No table data to export");
        return;
    }
    
    QString filename = QFileDialog::getSaveFileName(this, 
        "Export Table to CSV", 
        QString("%1.csv").arg(m_groupName),
        "CSV Files (*.csv);;All Files (*)");
    
    if (filename.isEmpty()) return;
    
    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Error", 
            QString("Could not open file for writing: %1").arg(file.errorString()));
        return;
    }
    
    QTextStream stream(&file);
    
    // Write headers
    for (int col = 0; col < m_tableWidget->columnCount(); col++) {
        if (col > 0) stream << ",";
        QString header = m_tableWidget->horizontalHeaderItem(col)->text();
        // Quote if contains comma
        if (header.contains(',')) {
            stream << "\"" << header << "\"";
        } else {
            stream << header;
        }
    }
    stream << "\n";
    
    // Write data
    for (int row = 0; row < m_tableWidget->rowCount(); row++) {
        for (int col = 0; col < m_tableWidget->columnCount(); col++) {
            if (col > 0) stream << ",";
            if (auto* item = m_tableWidget->item(row, col)) {
                QString text = item->text();
                // Quote if contains comma or newline
                if (text.contains(',') || text.contains('\n')) {
                    stream << "\"" << text.replace("\"", "\"\"") << "\"";
                } else {
                    stream << text;
                }
            }
        }
        stream << "\n";
    }
    
    file.close();
    
    EssConsoleManager::instance()->logInfo(
        QString("Exported %1 rows to %2").arg(m_tableWidget->rowCount()).arg(filename),
        "DynGroupViewer"
    );
}

void EssDynGroupViewer::setShowRowDetails(bool show)
{
    m_showRowDetails = show;
    m_rowDetailsPane->setVisible(show);
    m_rowDetailsAction->setChecked(show);
    
    if (show && m_tableWidget->currentRow() >= 0) {
        updateRowDetails(m_tableWidget->currentRow());
    }
}

void EssDynGroupViewer::onTableRowChanged(int currentRow, int currentColumn, int previousRow, int previousColumn)
{
    if (currentRow != previousRow && currentRow >= 0 && m_showRowDetails) {
        updateRowDetails(currentRow);
    }
}

void EssDynGroupViewer::updateRowDetails(int row)
{
    if (!m_showRowDetails || !m_dynGroup || row < 0) {
        return;
    }
    
    m_currentDetailRow = row;
    m_rowDetailsLabel->setText(QString("Row %1 Details").arg(row));
    populateRowDetailsTree(row);
}

void EssDynGroupViewer::populateRowDetailsTree(int row)
{
    m_rowDetailsTree->clear();
    
    if (!m_dynGroup) return;
    
    // Block signals to prevent accessibility issues during population
    m_rowDetailsTree->blockSignals(true);
    
    // Add each column's value for this row (show ALL columns, not just visible ones)
    for (int col = 0; col < DYN_GROUP_N(m_dynGroup); col++) {
        DYN_LIST* dl = DYN_GROUP_LIST(m_dynGroup, col);
        if (!dl || row >= DYN_LIST_N(dl)) continue;
        
        QString listName = QString::fromUtf8(DYN_LIST_NAME(dl));
        int dataType = DYN_LIST_DATATYPE(dl);
        
        QTreeWidgetItem* item = new QTreeWidgetItem(m_rowDetailsTree);
        item->setText(0, listName);
        item->setText(1, getDataTypeString(dataType));
        item->setIcon(0, getTypeIcon(dataType));
        
        // Check if this column is currently hidden in focus mode
        bool isHidden = false;
        for (int tableCol = 0; tableCol < m_tableWidget->columnCount(); tableCol++) {
            if (m_tableWidget->horizontalHeaderItem(tableCol)->text() == listName) {
                isHidden = m_tableWidget->isColumnHidden(tableCol);
                break;
            }
        }
        
        // Add visual indicator if column is hidden in main view
        if (isHidden) {
            QFont font = item->font(0);
            font.setItalic(true);
            item->setFont(0, font);
            item->setForeground(0, QColor(128, 128, 128)); // Gray out hidden columns
            item->setToolTip(0, "This column is hidden in the main table view");
        }
        
        if (dataType == DF_LIST) {
            // Nested list - make it expandable
            DYN_LIST** lists = (DYN_LIST**)DYN_LIST_VALS(dl);
            DYN_LIST* sublist = lists[row];
            
            if (sublist) {
                int subType = DYN_LIST_DATATYPE(sublist);
                int subSize = DYN_LIST_N(sublist);
                
                item->setText(1, QString("%1[%2]").arg(getDataTypeString(subType)).arg(subSize));
                item->setText(2, QString("<%1 items>").arg(subSize));
                
                // Only show items that will be visible in the viewport
                int maxItemsToShow = std::min(subSize, 50);
                
                // Block signals while adding sub-items
                m_rowDetailsTree->blockSignals(true);
                
                // Add sub-items
                for (int i = 0; i < maxItemsToShow; i++) {
                    QTreeWidgetItem* subItem = new QTreeWidgetItem(item);
                    subItem->setText(0, QString("[%1]").arg(i));
                    subItem->setText(1, getDataTypeString(subType));
                    subItem->setText(2, formatCellValue(sublist, i));
                    subItem->setIcon(0, getTypeIcon(subType));
                }
                
                if (subSize > maxItemsToShow) {
                    QTreeWidgetItem* moreItem = new QTreeWidgetItem(item);
                    moreItem->setText(0, "...");
                    moreItem->setText(2, QString("(%1 more items)").arg(subSize - maxItemsToShow));
                }
                
                m_rowDetailsTree->blockSignals(false);
                
                // Don't auto-expand very large lists
                if (subSize <= 20) {
                    item->setExpanded(true);
                }
            } else {
                item->setText(2, "<null>");
            }
        } else {
            // Simple value
            item->setText(2, formatCellValue(dl, row));
        }
    }
    
    // Re-enable signals
    m_rowDetailsTree->blockSignals(false);
    
    // Smart expansion strategy:
    // - Expand top-level items (columns) by default
    // - Only expand nested lists if they're small
    // - Keep deeply nested items collapsed
    for (int i = 0; i < m_rowDetailsTree->topLevelItemCount(); i++) {
        QTreeWidgetItem* item = m_rowDetailsTree->topLevelItem(i);
        
        // Always expand top level to show column values
        item->setExpanded(true);
        
        // But for nested lists, only expand if small
        if (item->childCount() > 0 && item->childCount() <= 5) {
            // Small nested list - expand first level only
            for (int j = 0; j < item->childCount(); j++) {
                QTreeWidgetItem* child = item->child(j);
                // Don't expand grandchildren by default
                child->setExpanded(false);
            }
        }
    }
}
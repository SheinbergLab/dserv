#include "DgTableWidget.h"
#include <QApplication>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QDebug>
#include <QFileInfo>

DgTableWidget::DgTableWidget(QWidget *parent)
    : QWidget(parent), dg(nullptr), ownsDynGroup(false) {
    setupUI();
}

DgTableWidget::~DgTableWidget() {
    if (dg && ownsDynGroup) {
        dfuFreeDynGroup(dg);
    }
}

void DgTableWidget::setupUI() {
    mainLayout = new QVBoxLayout(this);
    
    // Toolbar
    toolbarLayout = new QHBoxLayout();
    
    loadButton = new QPushButton("Load File...");
    loadButton->setIcon(QIcon::fromTheme("document-open"));
    connect(loadButton, &QPushButton::clicked, this, &DgTableWidget::onLoadFileClicked);
    
    clearButton = new QPushButton("Clear");
    clearButton->setIcon(QIcon::fromTheme("edit-clear"));
    connect(clearButton, &QPushButton::clicked, this, &DgTableWidget::onClearClicked);
    
    exportButton = new QPushButton("Export...");
    exportButton->setIcon(QIcon::fromTheme("document-save"));
    exportButton->setEnabled(false);
    connect(exportButton, &QPushButton::clicked, this, &DgTableWidget::onExportClicked);
    
    statusLabel = new QLabel("No data loaded");
    statusLabel->setStyleSheet("QLabel { color: gray; }");
    
    progressBar = new QProgressBar();
    progressBar->setVisible(false);
    progressBar->setMaximumWidth(200);
    
    toolbarLayout->addWidget(loadButton);
    toolbarLayout->addWidget(clearButton);
    toolbarLayout->addWidget(exportButton);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(statusLabel);
    toolbarLayout->addWidget(progressBar);
    
    mainLayout->addLayout(toolbarLayout);
    
    // Table
    setupTable();
    mainLayout->addWidget(tableWidget);
    
    setLayout(mainLayout);
}

void DgTableWidget::setupTable() {
    tableWidget = new QTableWidget(this);
    
    // Configure table appearance
    tableWidget->setAlternatingRowColors(true);
    tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tableWidget->setSortingEnabled(true);
    
    // Configure headers
    tableWidget->horizontalHeader()->setStretchLastSection(true);
    tableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    tableWidget->verticalHeader()->setDefaultSectionSize(25);
    
    // Connect signals
    connect(tableWidget, &QTableWidget::cellClicked, this, &DgTableWidget::onCellClicked);
    connect(tableWidget, &QTableWidget::itemSelectionChanged, this, &DgTableWidget::onSelectionChanged);
}

void DgTableWidget::setDynGroup(DYN_GROUP* dynGroup) {
    if (dg && ownsDynGroup) {
        dfuFreeDynGroup(dg);
    }
    
    dg = dynGroup;
    ownsDynGroup = false;  // Assume caller owns it unless we loaded from file
    filename.clear();
    
    if (dg) {
        populateTable();
        exportButton->setEnabled(true);
    } else {
        clear();
    }
}

void DgTableWidget::loadFromFile(const QString& filename) {
    if (filename.isEmpty()) return;
    
    progressBar->setVisible(true);
    progressBar->setRange(0, 0);  // Indeterminate progress
    statusLabel->setText("Loading file...");
    QApplication::processEvents();
    
    DYN_GROUP* newDg = readDgzFile(filename);
    
    progressBar->setVisible(false);
    
    if (!newDg) {
        statusLabel->setText("Failed to load file");
        statusLabel->setStyleSheet("QLabel { color: red; }");
        QMessageBox::warning(this, "Load Error", 
                           QString("Failed to load file: %1").arg(filename));
        return;
    }
    
    // Clean up old data
    if (dg && ownsDynGroup) {
        dfuFreeDynGroup(dg);
    }
    
    dg = newDg;
    ownsDynGroup = true;  // We own this since we loaded it
    this->filename = filename;
    
    populateTable();
    exportButton->setEnabled(true);
    
    emit dataLoaded(filename);
}

void DgTableWidget::clear() {
    if (dg && ownsDynGroup) {
        dfuFreeDynGroup(dg);
    }
    dg = nullptr;
    ownsDynGroup = false;
    filename.clear();
    
    tableWidget->clear();
    tableWidget->setRowCount(0);
    tableWidget->setColumnCount(0);
    
    statusLabel->setText("No data loaded");
    statusLabel->setStyleSheet("QLabel { color: gray; }");
    exportButton->setEnabled(false);
}

void DgTableWidget::populateTable() {
    if (!dg) {
        clear();
        return;
    }
    
    progressBar->setVisible(true);
    statusLabel->setText("Populating table...");
    QApplication::processEvents();
    
    int numCols = DYN_GROUP_N(dg);
    int maxRows = 0;
    
    // Find maximum number of rows
    for (int i = 0; i < numCols; i++) {
        DYN_LIST* dl = DYN_GROUP_LIST(dg, i);
        if (DYN_LIST_N(dl) > maxRows) {
            maxRows = DYN_LIST_N(dl);
        }
    }
    
    // Setup table dimensions
    tableWidget->setRowCount(maxRows);
    tableWidget->setColumnCount(numCols);
    
    // Set column headers (DYN_LIST names)
    QStringList headers;
    for (int i = 0; i < numCols; i++) {
        DYN_LIST* dl = DYN_GROUP_LIST(dg, i);
        headers << QString::fromUtf8(DYN_LIST_NAME(dl));
    }
    tableWidget->setHorizontalHeaderLabels(headers);
    
    // Populate cells
    progressBar->setRange(0, maxRows * numCols);
    int progress = 0;
    
    for (int row = 0; row < maxRows; row++) {
        for (int col = 0; col < numCols; col++) {
            DYN_LIST* dl = DYN_GROUP_LIST(dg, col);
            QString cellText;
            
            if (row < DYN_LIST_N(dl)) {
                cellText = formatCellValue(dl, row);
            } else {
                cellText = "";  // Empty cell for shorter lists
            }
            
            QTableWidgetItem* item = new QTableWidgetItem(cellText);
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);  // Read-only
            tableWidget->setItem(row, col, item);
            
            progressBar->setValue(++progress);
            if (progress % 1000 == 0) {
                QApplication::processEvents();  // Keep UI responsive
            }
        }
    }
    
    progressBar->setVisible(false);
    updateStatus();
}

QString DgTableWidget::formatCellValue(DYN_LIST* dl, int row) const {
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
            return QString::number(vals[row], 'g', 6);  // 6 significant digits
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
            DYN_LIST** vals = (DYN_LIST**)DYN_LIST_VALS(dl);
            const char* listType;
            switch (DYN_LIST_DATATYPE(vals[row])) {
                case DF_LONG: listType = "long"; break;
                case DF_SHORT: listType = "short"; break;
                case DF_FLOAT: listType = "float"; break;
                case DF_CHAR: listType = "char"; break;
                case DF_STRING: listType = "string"; break;
                case DF_LIST: listType = "list"; break;
                default: listType = "unknown"; break;
            }
            return QString("%1 (%2)").arg(listType).arg(DYN_LIST_N(vals[row]));
        }
        default:
            return QString("unknown type");
    }
}

void DgTableWidget::updateStatus() {
    if (!dg) {
        statusLabel->setText("No data loaded");
        statusLabel->setStyleSheet("QLabel { color: gray; }");
        return;
    }
    
    QString name = DYN_GROUP_NAME(dg) ? QString::fromUtf8(DYN_GROUP_NAME(dg)) : "Unnamed";
    int rows = tableWidget->rowCount();
    int cols = tableWidget->columnCount();
    
    QString status;
    if (!filename.isEmpty()) {
        QFileInfo info(filename);
        status = QString("%1: %2 rows × %3 cols").arg(info.baseName()).arg(rows).arg(cols);
    } else {
        status = QString("%1: %2 rows × %3 cols").arg(name).arg(rows).arg(cols);
    }
    
    statusLabel->setText(status);
    statusLabel->setStyleSheet("QLabel { color: black; }");
}

// Static utility methods
DYN_GROUP* DgTableWidget::readDgzFile(const QString& filename) {
    // This is a simplified version - you'll need to adapt your DGFile::read_dgz method
    QByteArray filenameBytes = filename.toLocal8Bit();
    return DGFile::read_dgz(filenameBytes.data());
}

QString DgTableWidget::cellValueAsString(DYN_LIST* dl, int row) {
    DgTableWidget temp;  // Temporary instance to use formatCellValue
    return temp.formatCellValue(dl, row);
}

// Slot implementations
void DgTableWidget::onLoadFileClicked() {
    QString filename = QFileDialog::getOpenFileName(
        this,
        "Load Dynamic Group File",
        QString(),
        "Dynamic Group Files (*.dg *.dgz *.lz4);;All Files (*)"
    );
    
    if (!filename.isEmpty()) {
        loadFromFile(filename);
    }
}

void DgTableWidget::onClearClicked() {
    clear();
}

void DgTableWidget::onExportClicked() {
    if (!dg) return;
    
    QString filename = QFileDialog::getSaveFileName(
        this,
        "Export Data",
        QString(),
        "CSV Files (*.csv);;Tab-separated (*.tsv);;All Files (*)"
    );
    
    if (filename.isEmpty()) return;
    
    // TODO: Implement export functionality
    statusLabel->setText("Export functionality - TODO");
}

void DgTableWidget::onCellClicked(int row, int column) {
    if (!dg || column >= DYN_GROUP_N(dg)) return;
    
    DYN_LIST* dl = DYN_GROUP_LIST(dg, column);
    QString value = (row < DYN_LIST_N(dl)) ? formatCellValue(dl, row) : QString();
    
    emit cellClicked(row, column, value);
}

void DgTableWidget::onSelectionChanged() {
    emit selectionChanged();
}

// Multi-tab implementation
DgTableTabs::DgTableTabs(QWidget *parent) : QWidget(parent) {
    setupUI();
}

DgTableTabs::~DgTableTabs() {
    clearAllTabs();
}

void DgTableTabs::setupUI() {
    mainLayout = new QVBoxLayout(this);
    
    tabWidget = new QTabWidget();
    tabWidget->setTabsClosable(true);
    tabWidget->setMovable(true);
    
    connect(tabWidget, &QTabWidget::currentChanged, this, &DgTableTabs::onTabChanged);
    connect(tabWidget, &QTabWidget::tabCloseRequested, this, &DgTableTabs::onTabCloseRequested);
    
    mainLayout->addWidget(tabWidget);
    setLayout(mainLayout);
}

int DgTableTabs::addDynGroup(DYN_GROUP* dg, const QString& name) {
    if (!dg) return -1;
    
    DgTableWidget* table = new DgTableWidget();
    table->setDynGroup(dg);
    
    QString tabName = name.isEmpty() ? 
        (DYN_GROUP_NAME(dg) ? QString::fromUtf8(DYN_GROUP_NAME(dg)) : "Untitled") : 
        name;
    
    int index = tabWidget->addTab(table, tabName);
    
    // Connect signals
    connect(table, &DgTableWidget::dataLoaded, this, [this, index](const QString& filename) {
        emit dataLoaded(index, filename);
    });
    
    return index;
}

int DgTableTabs::addFromFile(const QString& filename) {
    DgTableWidget* table = new DgTableWidget();
    
    QFileInfo info(filename);
    QString tabName = info.baseName();
    
    int index = tabWidget->addTab(table, tabName);
    
    // Connect signals
    connect(table, &DgTableWidget::dataLoaded, this, [this, index](const QString& filename) {
        emit dataLoaded(index, filename);
    });
    
    // Load the file
    table->loadFromFile(filename);
    
    return index;
}

void DgTableTabs::removeTab(int index) {
    if (index >= 0 && index < tabWidget->count()) {
        QWidget* widget = tabWidget->widget(index);
        tabWidget->removeTab(index);
        delete widget;
    }
}

void DgTableTabs::clearAllTabs() {
    while (tabWidget->count() > 0) {
        removeTab(0);
    }
}

DgTableWidget* DgTableTabs::currentTable() {
    return qobject_cast<DgTableWidget*>(tabWidget->currentWidget());
}

DgTableWidget* DgTableTabs::tableAt(int index) {
    return qobject_cast<DgTableWidget*>(tabWidget->widget(index));
}

int DgTableTabs::currentIndex() const {
    return tabWidget->currentIndex();
}

void DgTableTabs::onTabChanged(int index) {
    emit tabChanged(index);
}

void DgTableTabs::onTabCloseRequested(int index) {
    removeTab(index);
}

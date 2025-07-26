#pragma once
#include <QWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QProgressBar>
#include <QTabWidget>
#include <QSplitter>

// Include your DYN_GROUP headers
#include "TclInterp.h"  // For DYN_GROUP definitions
extern "C" {
#include <df.h>
#include <dynio.h>
}

class DgTableModel;

class DgTableWidget : public QWidget {
    Q_OBJECT

public:
    explicit DgTableWidget(QWidget *parent = nullptr);
    ~DgTableWidget();
    
    // Main interface methods
    void setDynGroup(DYN_GROUP* dg);
    void loadFromFile(const QString& filename);
    void clear();
    
    // Access methods
    DYN_GROUP* dynGroup() const { return dg; }
    QString currentFilename() const { return filename; }
    
    // Static utility methods
    static DYN_GROUP* readDgzFile(const QString& filename);
    static QString cellValueAsString(DYN_LIST* dl, int row);

signals:
    void dataLoaded(const QString& filename);
    void cellClicked(int row, int col, const QString& value);
    void selectionChanged();

private slots:
    void onLoadFileClicked();
    void onClearClicked();
    void onExportClicked();
    void onCellClicked(int row, int column);
    void onSelectionChanged();

private:
    void setupUI();
    void setupTable();
    void populateTable();
    void updateStatus();
    QString formatCellValue(DYN_LIST* dl, int row) const;
    
    // UI Components
    QVBoxLayout* mainLayout;
    QHBoxLayout* toolbarLayout;
    QPushButton* loadButton;
    QPushButton* clearButton;
    QPushButton* exportButton;
    QLabel* statusLabel;
    QProgressBar* progressBar;
    
    QTableWidget* tableWidget;
    
    // Data
    DYN_GROUP* dg;
    QString filename;
    bool ownsDynGroup;  // Whether we should free the DYN_GROUP in destructor
};

// Multi-tab version for handling multiple DYN_GROUPs
class DgTableTabs : public QWidget {
    Q_OBJECT

public:
    explicit DgTableTabs(QWidget *parent = nullptr);
    ~DgTableTabs();
    
    // Tab management
    int addDynGroup(DYN_GROUP* dg, const QString& name = QString());
    int addFromFile(const QString& filename);
    void removeTab(int index);
    void clearAllTabs();
    
    // Access current tab
    DgTableWidget* currentTable();
    DgTableWidget* tableAt(int index);
    int currentIndex() const;

signals:
    void tabChanged(int index);
    void dataLoaded(int tabIndex, const QString& filename);

private slots:
    void onTabChanged(int index);
    void onTabCloseRequested(int index);

private:
    void setupUI();
    
    QVBoxLayout* mainLayout;
    QTabWidget* tabWidget;
};

#include "DgFile.h"  // We'll need to create this Qt version too

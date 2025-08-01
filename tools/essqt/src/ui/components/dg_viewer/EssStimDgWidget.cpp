#include "EssStimDgWidget.h"
#include "core/EssApplication.h"
#include "core/EssDataProcessor.h"
#include "core/EssCommandInterface.h"
#include "console/EssOutputConsole.h"
#include <QHeaderView>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QTextEdit>
#include <QPushButton>
#include <QClipboard>
#include <QApplication>
#include <QScrollArea>
#include <numeric>
#include <cmath>

extern "C" {
#include "dlfuncs.h"
}

EssStimDgWidget::EssStimDgWidget(QWidget *parent)
    : EssDynGroupViewer(parent)
    , m_autoRefresh(true)
    , m_focusMode(false)
{
    // Default to table view for stimulus data
    setViewMode(TableView);
    
    // Set window title
    setWindowTitle("Stimulus Data (stimdg)");
    
    // Default highlight columns for typical stimulus parameters
    m_highlightColumns << "trial" << "stim_type" << "target_x" << "target_y" 
                      << "reward" << "correct" << "rt";
    
    connectToDataProcessor();
    customizeForStimulus();
    
    // Try to load existing stimdg if available
    refreshStimDg();
}

EssStimDgWidget::~EssStimDgWidget() = default;

void EssStimDgWidget::connectToDataProcessor()
{
    auto* app = EssApplication::instance();
    if (!app) return;
    
    auto* processor = app->dataProcessor();
    if (!processor) return;
    
    // Connect to the generic datapoint signal and filter for stimdg
    connect(processor, &EssDataProcessor::genericDatapointReceived,
            this, [this](const QString& name, const QVariant& value, qint64 timestamp) {
                if (name == "stimdg") {
                    onStimDgReceived();
                }
            });
    
    // Also connect to stimulus-specific signal if available
    connect(processor, &EssDataProcessor::stimulusDataReceived,
            this, [this](const QByteArray& dgData, qint64 timestamp) {
                onStimDgReceived();
            });
    
    // Clear on disconnect
    auto* cmdInterface = app->commandInterface();
    if (cmdInterface) {
        connect(cmdInterface, &EssCommandInterface::disconnected,
                this, [this]() {
                    clear();
                    EssConsoleManager::instance()->logInfo(
                        "Cleared stimulus data on disconnect",
                        "StimDG"
                    );
                });
    }
}

void EssStimDgWidget::onStimDgReceived()
{
    if (!m_autoRefresh) return;
    
    EssConsoleManager::instance()->logDebug("Stimulus data received", "StimDG");
    
    // The DG is already processed and stored in Tcl by EssDataProcessor
    refreshStimDg();
    emit stimulusDataUpdated();
}

void EssStimDgWidget::refreshStimDg()
{
    // Try to get stimdg from Tcl
    auto* app = EssApplication::instance();
    if (!app) return;
    
    auto* cmdInterface = app->commandInterface();
    if (!cmdInterface) return;
    
    Tcl_Interp* interp = cmdInterface->tclInterp();
    if (!interp) return;
    
    DYN_GROUP* dg = nullptr;
    
    // Try to find stimdg in Tcl
    int result = tclFindDynGroup(interp, const_cast<char*>("stimdg"), &dg);
    if (result == TCL_OK && dg) {
        setDynGroup(dg, "stimdg");
        
        int numTrials = tableWidget()->rowCount();
        EssConsoleManager::instance()->logInfo(
            QString("Loaded stimdg with %1 trials").arg(numTrials),
            "StimDG"
        );
        
        // Store all column names
        m_allColumns.clear();
        for (int col = 0; col < tableWidget()->columnCount(); col++) {
            m_allColumns << tableWidget()->horizontalHeaderItem(col)->text();
        }
        
        // Apply highlighting to important columns
        highlightImportantColumns();
        
        // Update statistics if we have data
        if (numTrials > 0) {
            updateStatistics();
        }
    } else {
        // Log that no stimdg is available yet
        EssConsoleManager::instance()->logDebug(
            "No stimdg available in Tcl interpreter yet",
            "StimDG"
        );
    }
}

void EssStimDgWidget::customizeForStimulus()
{
    // Add stimulus-specific toolbar actions
    toolbar()->addSeparator();
    
    // Auto-refresh toggle
    QAction* autoRefreshAction = toolbar()->addAction("Auto-refresh");
    autoRefreshAction->setCheckable(true);
    autoRefreshAction->setChecked(m_autoRefresh);
    autoRefreshAction->setToolTip("Automatically update when new stimulus data arrives");
    connect(autoRefreshAction, &QAction::toggled, [this](bool checked) {
        m_autoRefresh = checked;
        if (checked) {
            refreshStimDg(); // Refresh immediately when re-enabled
        }
    });
    
    toolbar()->addSeparator();
    
    // Trial navigation
    QAction* firstTrialAction = toolbar()->addAction(QIcon::fromTheme("go-first"), "First Trial");
    firstTrialAction->setToolTip("Go to first trial");
    connect(firstTrialAction, &QAction::triggered, [this]() {
        if (tableWidget()->rowCount() > 0) {
            tableWidget()->selectRow(0);
            tableWidget()->scrollToItem(tableWidget()->item(0, 0));
            emit trialSelected(0);
        }
    });
    
    QAction* lastTrialAction = toolbar()->addAction(QIcon::fromTheme("go-last"), "Last Trial");
    lastTrialAction->setToolTip("Go to last trial");
    connect(lastTrialAction, &QAction::triggered, [this]() {
        int lastRow = tableWidget()->rowCount() - 1;
        if (lastRow >= 0) {
            tableWidget()->selectRow(lastRow);
            tableWidget()->scrollToItem(tableWidget()->item(lastRow, 0));
            emit trialSelected(lastRow);
        }
    });
    
    toolbar()->addSeparator();
    
    // Statistics action
    QAction* statsAction = toolbar()->addAction(QIcon::fromTheme("view-statistics"), "Statistics");
    statsAction->setToolTip("Show trial statistics");
    connect(statsAction, &QAction::triggered, this, &EssStimDgWidget::showStatistics);
    
    // Highlight columns action
    QAction* highlightAction = toolbar()->addAction("Select Columns");
    highlightAction->setToolTip("Choose which columns to highlight/show");
    connect(highlightAction, &QAction::triggered, this, &EssStimDgWidget::configureHighlighting);
    
    // Focus mode toggle
    m_focusModeAction = toolbar()->addAction(QIcon::fromTheme("view-filter"), "Focus Mode");
    m_focusModeAction->setCheckable(true);
    m_focusModeAction->setChecked(false);
    m_focusModeAction->setToolTip("Show only selected columns");
    connect(m_focusModeAction, &QAction::toggled, this, &EssStimDgWidget::toggleFocusMode);
    
    // Add context menu to table for trial-specific actions
    connect(tableWidget(), &QTableWidget::customContextMenuRequested, 
            this, &EssStimDgWidget::showTrialContextMenu);
    
    // Connect row selection
    connect(tableWidget(), &QTableWidget::currentCellChanged,
            [this](int currentRow, int currentColumn, int previousRow, int previousColumn) {
                if (currentRow != previousRow && currentRow >= 0) {
                    emit trialSelected(currentRow);
                }
            });
}

void EssStimDgWidget::setHighlightColumns(const QStringList& columnNames)
{
    m_highlightColumns = columnNames;
    highlightImportantColumns();
}

void EssStimDgWidget::updateRowDetails(int row)
{
    // Call parent implementation
    EssDynGroupViewer::updateRowDetails(row);
    
    // Add focus mode indicator to the label if needed
    if (m_focusMode && row >= 0) {
        QString labelText = QString("Row %1 Details").arg(row);
        if (m_focusMode) {
            labelText += " (showing all columns including hidden)";
        }
        // We need access to the label - let's update through the parent
        // For now, the visual indicators in the tree itself are sufficient
    }
}

int EssStimDgWidget::currentTrialIndex() const
{
    return tableWidget()->currentRow();
}

void EssStimDgWidget::highlightImportantColumns()
{
    if (viewMode() != TableView) return;
    
    QTableWidget* table = tableWidget();
    
    // Reset all column backgrounds first
    for (int col = 0; col < table->columnCount(); col++) {
        QTableWidgetItem* headerItem = table->horizontalHeaderItem(col);
        if (!headerItem) continue;
        
        QString columnName = headerItem->text();
        bool isHighlighted = m_highlightColumns.contains(columnName, Qt::CaseInsensitive);
        
        // Update header appearance
        QFont font = headerItem->font();
        font.setBold(isHighlighted);
        headerItem->setFont(font);
        headerItem->setForeground(isHighlighted ? QColor(0, 100, 200) : QColor());
        
        // Update cell backgrounds
        for (int row = 0; row < table->rowCount(); row++) {
            if (auto* item = table->item(row, col)) {
                // Only update background if it's not a nested list
                if (!item->data(Qt::UserRole).value<void*>()) {
                    if (isHighlighted) {
                        item->setBackground(QColor(245, 250, 255)); // Very light blue
                    } else {
                        item->setBackground(QBrush());
                    }
                }
            }
        }
    }
    
    // Apply column visibility if in focus mode
    if (m_focusMode) {
        applyColumnVisibility();
    }
}

void EssStimDgWidget::showTrialContextMenu(const QPoint& pos)
{
    QTableWidget* table = tableWidget();
    QTableWidgetItem* item = table->itemAt(pos);
    if (!item) return;
    
    int row = item->row();
    
    QMenu menu;
    menu.addAction(QString("Trial %1").arg(row))->setEnabled(false);
    menu.addSeparator();
    
    // Copy trial data
    menu.addAction(QIcon::fromTheme("edit-copy"), "Copy Trial Data", [this, row]() {
        QStringList trialData;
        QTableWidget* table = tableWidget();
        
        // Header
        QStringList headers;
        for (int col = 0; col < table->columnCount(); col++) {
            headers << table->horizontalHeaderItem(col)->text();
        }
        trialData << headers.join("\t");
        
        // Data
        QStringList values;
        for (int col = 0; col < table->columnCount(); col++) {
            if (auto* item = table->item(row, col)) {
                values << item->text();
            } else {
                values << "";
            }
        }
        trialData << values.join("\t");
        
        QApplication::clipboard()->setText(trialData.join("\n"));
    });
    
    // Find similar trials
    menu.addAction("Find Similar Trials", [this, row]() {
        findSimilarTrials(row);
    });
    
    menu.addSeparator();
    
    // Go to trial in experiment (if connected)
    auto* cmdInterface = EssApplication::instance()->commandInterface();
    if (cmdInterface && cmdInterface->isConnected()) {
        menu.addAction("Jump to This Trial", [this, row, cmdInterface]() {
            // Send command to jump to specific trial
            QString cmd = QString("jump_to_trial %1").arg(row);
            cmdInterface->executeCommand(cmd);
        });
    }
    
    menu.exec(table->mapToGlobal(pos));
}

void EssStimDgWidget::findSimilarTrials(int referenceRow)
{
    QTableWidget* table = tableWidget();
    if (referenceRow < 0 || referenceRow >= table->rowCount()) return;
    
    // Ask which column to use for similarity
    QStringList columnNames;
    for (int col = 0; col < table->columnCount(); col++) {
        columnNames << table->horizontalHeaderItem(col)->text();
    }
    
    bool ok;
    QString selectedColumn = QInputDialog::getItem(this, 
        "Find Similar Trials",
        "Find trials with same value in column:",
        columnNames, 0, false, &ok);
    
    if (!ok || selectedColumn.isEmpty()) return;
    
    // Find the column index
    int columnIndex = -1;
    for (int col = 0; col < table->columnCount(); col++) {
        if (table->horizontalHeaderItem(col)->text() == selectedColumn) {
            columnIndex = col;
            break;
        }
    }
    
    if (columnIndex < 0) return;
    
    // Get reference value
    QString referenceValue;
    if (auto* item = table->item(referenceRow, columnIndex)) {
        referenceValue = item->text();
    }
    
    // Clear current selection
    table->clearSelection();
    
    // Select all matching rows
    int matchCount = 0;
    for (int row = 0; row < table->rowCount(); row++) {
        if (auto* item = table->item(row, columnIndex)) {
            if (item->text() == referenceValue) {
                table->selectRow(row);
                matchCount++;
            }
        }
    }
    
    // Show result
    EssConsoleManager::instance()->logInfo(
        QString("Found %1 trials with %2 = '%3'")
            .arg(matchCount)
            .arg(selectedColumn)
            .arg(referenceValue),
        "StimDG"
    );
}

void EssStimDgWidget::configureHighlighting()
{
    QTableWidget* table = tableWidget();
    if (table->columnCount() == 0) {
        QMessageBox::information(this, "Select Columns", 
            "No data loaded. Load stimulus data first.");
        return;
    }
    
    // Create a dialog with checkboxes for each column
    QDialog dialog(this);
    dialog.setWindowTitle("Select Columns");
    dialog.resize(350, 500);
    
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    
    QLabel* label = new QLabel("Select columns to highlight/show:");
    layout->addWidget(label);
    
    // Add select all/none buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    QPushButton* selectAllBtn = new QPushButton("Select All");
    QPushButton* selectNoneBtn = new QPushButton("Select None");
    buttonLayout->addWidget(selectAllBtn);
    buttonLayout->addWidget(selectNoneBtn);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);
    
    // Create checkboxes for each column
    QList<QCheckBox*> checkboxes;
    QWidget* scrollWidget = new QWidget();
    QVBoxLayout* scrollLayout = new QVBoxLayout(scrollWidget);
    
    for (int col = 0; col < table->columnCount(); col++) {
        QString columnName = table->horizontalHeaderItem(col)->text();
        QCheckBox* cb = new QCheckBox(columnName);
        cb->setChecked(m_highlightColumns.contains(columnName, Qt::CaseInsensitive));
        checkboxes.append(cb);
        scrollLayout->addWidget(cb);
    }
    scrollLayout->addStretch();
    
    // Put in scroll area
    QScrollArea* scrollArea = new QScrollArea();
    scrollArea->setWidget(scrollWidget);
    scrollArea->setWidgetResizable(true);
    layout->addWidget(scrollArea);
    
    // Connect select all/none buttons
    connect(selectAllBtn, &QPushButton::clicked, [&checkboxes]() {
        for (auto* cb : checkboxes) cb->setChecked(true);
    });
    connect(selectNoneBtn, &QPushButton::clicked, [&checkboxes]() {
        for (auto* cb : checkboxes) cb->setChecked(false);
    });
    
    // Add focus mode reminder if active
    if (m_focusMode) {
        QLabel* focusLabel = new QLabel("<i>Focus Mode is ON - unselected columns will be hidden</i>");
        focusLabel->setStyleSheet("QLabel { color: #0066cc; padding: 5px; }");
        layout->addWidget(focusLabel);
    }
    
    // Buttons
    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    
    if (dialog.exec() == QDialog::Accepted) {
        // Update highlight columns
        m_highlightColumns.clear();
        for (int i = 0; i < checkboxes.size(); i++) {
            if (checkboxes[i]->isChecked()) {
                m_highlightColumns << checkboxes[i]->text();
            }
        }
        highlightImportantColumns();
    }
}

void EssStimDgWidget::toggleFocusMode(bool enabled)
{
    m_focusMode = enabled;
    m_focusModeAction->setChecked(enabled);
    
    if (enabled) {
        // Show only selected columns
        applyColumnVisibility();
        
        // Update row details label to indicate focus mode
        if (isShowingRowDetails()) {
            updateRowDetails(tableWidget()->currentRow());
        }
        
        EssConsoleManager::instance()->logInfo(
            QString("Focus mode enabled - showing %1 of %2 columns")
                .arg(m_highlightColumns.size())
                .arg(m_allColumns.size()),
            "StimDG"
        );
    } else {
        // Show all columns
        QTableWidget* table = tableWidget();
        for (int col = 0; col < table->columnCount(); col++) {
            table->setColumnHidden(col, false);
        }
        
        // Update row details to remove focus mode indicators
        if (isShowingRowDetails()) {
            updateRowDetails(tableWidget()->currentRow());
        }
        
        EssConsoleManager::instance()->logInfo("Focus mode disabled - showing all columns", "StimDG");
    }
}

void EssStimDgWidget::applyColumnVisibility()
{
    if (!m_focusMode) return;
    
    QTableWidget* table = tableWidget();
    
    // Hide/show columns based on selection
    for (int col = 0; col < table->columnCount(); col++) {
        QString columnName = table->horizontalHeaderItem(col)->text();
        bool shouldShow = m_highlightColumns.contains(columnName, Qt::CaseInsensitive);
        table->setColumnHidden(col, !shouldShow);
    }
    
    // If no columns are selected, show all to avoid empty table
    if (m_highlightColumns.isEmpty()) {
        for (int col = 0; col < table->columnCount(); col++) {
            table->setColumnHidden(col, false);
        }
        
        QMessageBox::warning(this, "Focus Mode", 
            "No columns selected. Showing all columns.\n"
            "Use 'Select Columns' to choose which columns to show.");
        
        m_focusMode = false;
        m_focusModeAction->setChecked(false);
    }
}

void EssStimDgWidget::showStatistics()
{
    QTableWidget* table = tableWidget();
    if (table->rowCount() == 0) {
        QMessageBox::information(this, "Statistics", "No data to analyze.");
        return;
    }
    
    // Create statistics dialog
    QDialog dialog(this);
    dialog.setWindowTitle("Trial Statistics");
    dialog.resize(600, 500);
    
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    
    QTextEdit* statsText = new QTextEdit();
    statsText->setReadOnly(true);
    statsText->setFont(QFont("Courier", 10));
    
    QString stats;
    QTextStream stream(&stats);
    
    stream << "STIMULUS DATA STATISTICS\n";
    stream << "========================\n\n";
    stream << QString("Total Trials: %1\n\n").arg(table->rowCount());
    
    // Analyze each column
    for (int col = 0; col < table->columnCount(); col++) {
        QString columnName = table->horizontalHeaderItem(col)->text();
        stream << QString("Column: %1\n").arg(columnName);
        stream << QString("-").repeated(columnName.length() + 8) << "\n";
        
        // Collect values
        QStringList stringValues;
        QList<double> numericValues;
        bool hasNumeric = false;
        int nonEmptyCount = 0;
        
        for (int row = 0; row < table->rowCount(); row++) {
            if (auto* item = table->item(row, col)) {
                QString value = item->text();
                if (!value.isEmpty()) {
                    nonEmptyCount++;
                    stringValues << value;
                    
                    // Try to parse as number
                    bool ok;
                    double numValue = value.toDouble(&ok);
                    if (ok) {
                        numericValues << numValue;
                        hasNumeric = true;
                    }
                }
            }
        }
        
        stream << QString("  Non-empty values: %1/%2\n").arg(nonEmptyCount).arg(table->rowCount());
        
        if (hasNumeric && numericValues.size() > 0) {
            // Numeric statistics
            double sum = std::accumulate(numericValues.begin(), numericValues.end(), 0.0);
            double mean = sum / numericValues.size();
            
            double min = *std::min_element(numericValues.begin(), numericValues.end());
            double max = *std::max_element(numericValues.begin(), numericValues.end());
            
            // Standard deviation
            double variance = 0;
            for (double val : numericValues) {
                variance += (val - mean) * (val - mean);
            }
            variance /= numericValues.size();
            double stdDev = std::sqrt(variance);
            
            stream << QString("  Numeric values: %1\n").arg(numericValues.size());
            stream << QString("  Mean: %1\n").arg(mean, 0, 'f', 3);
            stream << QString("  Std Dev: %1\n").arg(stdDev, 0, 'f', 3);
            stream << QString("  Min: %1\n").arg(min, 0, 'f', 3);
            stream << QString("  Max: %1\n").arg(max, 0, 'f', 3);
        } else {
            // Categorical statistics
            QMap<QString, int> valueCounts;
            for (const QString& value : stringValues) {
                valueCounts[value]++;
            }
            
            stream << QString("  Unique values: %1\n").arg(valueCounts.size());
            
            // Show top values if not too many
            if (valueCounts.size() <= 10) {
                stream << "  Value distribution:\n";
                for (auto it = valueCounts.begin(); it != valueCounts.end(); ++it) {
                    double percent = 100.0 * it.value() / nonEmptyCount;
                    stream << QString("    %1: %2 (%3%)\n")
                        .arg(it.key())
                        .arg(it.value())
                        .arg(percent, 0, 'f', 1);
                }
            } else {
                stream << "  (Too many unique values to display)\n";
            }
        }
        
        stream << "\n";
    }
    
    statsText->setPlainText(stats);
    layout->addWidget(statsText);
    
    // Buttons
    QPushButton* copyButton = new QPushButton("Copy to Clipboard");
    connect(copyButton, &QPushButton::clicked, [stats]() {
        QApplication::clipboard()->setText(stats);
    });
    
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(copyButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(buttons);
    
    layout->addLayout(buttonLayout);
    
    dialog.exec();
}

void EssStimDgWidget::updateStatistics()
{
    // This is a simplified version that could update a status label
    // or emit signals with basic statistics
    QTableWidget* table = tableWidget();
    int numTrials = table->rowCount();
    
    if (numTrials > 0) {
        // Look for common columns and compute quick stats
        int correctColumn = -1;
        int rtColumn = -1;
        
        for (int col = 0; col < table->columnCount(); col++) {
            QString columnName = table->horizontalHeaderItem(col)->text().toLower();
            if (columnName == "correct") {
                correctColumn = col;
            } else if (columnName == "rt" || columnName == "reaction_time") {
                rtColumn = col;
            }
        }
        
        // Compute percent correct if available
        if (correctColumn >= 0) {
            int correctCount = 0;
            for (int row = 0; row < table->rowCount(); row++) {
                if (auto* item = table->item(row, correctColumn)) {
                    QString value = item->text();
                    if (value == "1" || value.toLower() == "true" || value.toLower() == "yes") {
                        correctCount++;
                    }
                }
            }
            double percentCorrect = 100.0 * correctCount / numTrials;
            
            EssConsoleManager::instance()->logInfo(
                QString("Performance: %1/%2 correct (%3%)")
                    .arg(correctCount)
                    .arg(numTrials)
                    .arg(percentCorrect, 0, 'f', 1),
                "StimDG"
            );
        }
    }
}
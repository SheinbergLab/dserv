#include "EssBehavmonWidget.h"
#include <QSplitter>
#include <QGridLayout>
#include <QHeaderView>
#include <QDateTime>
#include <QRegularExpression>

EssBehavmonWidget::EssBehavmonWidget(const QString& name, QWidget* parent)
    : EssScriptableWidget(name.isEmpty() ? QString("behavmon_%1").arg(QDateTime::currentMSecsSinceEpoch()) : name, parent)
    , m_mainWidget(nullptr)
    , m_generalGroup(nullptr)
    , m_detailedGroup(nullptr)
    , m_controlsGroup(nullptr)
    , m_detailedTable(nullptr)
    , m_primarySortCombo(nullptr)
    , m_secondarySortCombo(nullptr)
    , m_resetButton(nullptr)
    , m_exportButton(nullptr)
{
    // Set default setup script
    setSetupScript(R"tcl(

# Behavior Monitor Widget Setup Script
local_log "Behavmon widget script loaded"

# Initialize performance tracking variables
set ::behavmon_trials {}
set ::behavmon_sort_columns {}

# Bind to trial data updates
bind_datapoint "trialdg" {
    local_log "Trial data received - updating performance display"
    process_trial_data
}

# Bind to stimdg updates for sort options
bind_datapoint "stimdg" {
    local_log "Stimulus data received - updating sort options"
    update_sort_options
}

# Bind to reset events
bind_datapoint "ess/reset" {
    local_log "System reset - clearing performance data"
    clear_behavmon_data
}

proc normalizePair {a b} {
    if {$a eq "" && $b eq ""} {
        return ""
    } elseif {$a eq ""} {
        return $b
    } elseif {$b eq ""} {
        return $a
    } else {
        return "$a $b"
    }
}

proc transposeList {matrix} {
    set result {}
    set numCols [llength [lindex $matrix 0]]
    for {set col 0} {$col < $numCols} {incr col} {
        set newRow {}
        foreach row $matrix {
            lappend newRow [lindex $row $col]
        }
        lappend result $newRow
    }
    return $result
}

# Main function to process trial data and update display
proc process_trial_data {} {
    # copy trialdg from main interpreter
    get_dg trialdg
    
    if {![dg_exists trialdg]} {
        local_log "No trialdg available"
        return
    }
    
    # Get trial data lists
    if {![dl_exists trialdg:status] || ![dl_exists trialdg:rt]} {
        local_log "Required trial data not found"
        return
    }
    
    set status_list [dl_tcllist trialdg:status]
    set rt_list [dl_tcllist trialdg:rt]
    
    if {[llength $status_list] == 0} {
        local_log "No trials in trialdg"
        return
    }
    
    # Calculate basic performance stats
    set total_trials [llength $status_list]
    set correct_trials [llength [lsearch -all $status_list 1]]
    set percent_correct [expr {$total_trials > 0 ? int(100.0 * $correct_trials / $total_trials) : 0}]
    set percent_complete 100  ;# Assume trials in trialdg are complete
    
    # Update general performance display
    set_general_performance $percent_correct $percent_complete $total_trials
    
    # Update detailed performance table if we have sort options
    update_performance_table
    
    local_log "Performance updated: $percent_correct% correct, $total_trials trials"
}

# Update sort options from stimdg
proc update_sort_options {} {

    # get a local copy of stimdg for ourselves
    get_dg stimdg

    if {![dg_exists stimdg]} {
        local_log "No stimdg available for sort options"
        return
    }
    
    set ::behavmon_sort_columns {}
    set n_trials [dl_length stimdg:stimtype]
    
    # Find suitable columns for sorting
    foreach list_name [dg_tclListnames stimdg] {
        if { [dl_datatype stimdg:$list_name] == "list" } { continue }
        if {[dl_length stimdg:$list_name] == $n_trials && $list_name ne "remaining"} {
            # Check if this list has reasonable number of unique values for sorting
            set unique_vals [dl_tcllist [dl_unique stimdg:$list_name]]
            if {[llength $unique_vals] <= 10 && [llength $unique_vals] > 1} {
                lappend ::behavmon_sort_columns $list_name
            }
        }
    }
    
    # Update the UI sort options
    if {[llength $::behavmon_sort_columns] > 0} {
        set_sort_options $::behavmon_sort_columns
        local_log "Sort options updated: $::behavmon_sort_columns"
    }
}

# Update the performance table based on current sort selection
proc update_performance_table {} {
    if {![dg_exists trialdg]} return
    
    # Get current sort selection from UI
    lassign [get_sort_selection] primary_sort secondary_sort
    
    local_log "Updating table with sort: primary='$primary_sort' secondary='$secondary_sort'"
    
    # Calculate and display performance data
    set table_data [calculate_performance_data {*}[normalizePair $primary_sort $secondary_sort]]
    lassign $table_data headers rows nrows
    
    # Update the table display
    if { $nrows == 1 } { set rows [list $rows] }
    set_performance_table $headers $rows
}

proc calculate_performance_data { args } {
    set nargs [llength $args]
    if { $nargs > 2 } return
    set curdg [dg_copySelected trialdg [dl_oneof trialdg:status [dl_ilist 0 1]]]
    if { $nargs == 0 } {
	set pc [format %d [expr int(100*[dl_mean $curdg:status])]]
	set rt [format %.2f [dl_mean $curdg:rt]]
	set  n [dl_length $curdg:status]
	set headers "{% correct} rt n"
        dg_delete $curdg
	return [list $headers [list $pc $rt $n] 1]
    } elseif { $nargs == 1 } {
	set sortby $args
	dl_local pc [dl_selectSortedFunc $curdg:status \
			 "$curdg:$sortby" \
			 "stimdg:$sortby" \
			 dl_means]
	dl_local rt [dl_selectSortedFunc $curdg:rt \
			 "$curdg:$sortby" \
			 "stimdg:$sortby" \
			 dl_means]
	dl_local n [dl_selectSortedFunc $curdg:status \
			"$curdg:$sortby" \
			"stimdg:$sortby" \
			dl_lengths]
	dl_local result [dl_llist [dl_unique stimdg:$sortby]]
	dl_local pc [dl_slist \
                        {*}[lmap v [dl_tcllist [dl_int [dl_mult 100 $pc:1]]] {format %d $v}]]
	dl_local rt [dl_slist {*}[lmap v [dl_tcllist $rt:1] {format %.2f $v}]]
	dl_append $result $pc
	dl_append $result $rt
	dl_append $result $n:1
	
	set headers "$sortby {% correct} rt n"
        dg_delete $curdg
	return [list $headers [transposeList [dl_tcllist $result]] [dl_length $pc]]
    } else {
	lassign $args s1 s2
	dl_local pc [dl_selectSortedFunc $curdg:status \
			 "$curdg:$s2 $curdg:$s1" \
			 "stimdg:$s2 stimdg:$s1" \
			 dl_means]
	dl_local rt [dl_selectSortedFunc $curdg:rt \
			 "$curdg:$s2 $curdg:$s1" \
			 "stimdg:$s2 stimdg:$s1" \
			 dl_means]
	dl_local n [dl_selectSortedFunc $curdg:status \
			 "$curdg:$s2 $curdg:$s1" \
			 "stimdg:$s2 stimdg:$s1" \
			 dl_lengths]
	dl_local result [dl_reverse [dl_uniqueCross stimdg:$s1 stimdg:$s2]]

	dl_local pc [dl_slist \
                         {*}[lmap v [dl_tcllist [dl_int [dl_mult 100 $pc:2]]] {format %d $v}]]
	dl_local rt [dl_slist {*}[lmap v [dl_tcllist $rt:2] {format %.2f $v}]]
	dl_append $result $pc
	dl_append $result $rt
	dl_append $result $n:2

	set headers "$s1 $s2 {% correct} rt n"
        dg_delete $curdg
	return [list $headers [transposeList [dl_tcllist $result]] [dl_length $pc]]
    }
}

# Clear all performance data
proc clear_behavmon_data {} {
    set ::behavmon_trials {}
    set_general_performance 0 0 0
    clear_table
    local_log "Behavmon data cleared"
}

# Test function for development
proc test_behavmon {} {
    local_log "Testing behavior monitor with sample data"
    
    # Set some sample performance values
    set_general_performance 75 100 20
    
    # Set sample table data
    set headers [list "Condition" "% Correct" "RT" "N"]
    set rows [list \
        [list "Easy" "85" "450.2" "10"] \
        [list "Hard" "65" "650.8" "10"]]
    
    set_performance_table $headers $rows
    
    local_log "Sample performance data loaded"
}

# Callback for when sort selection changes
proc on_sort_changed {} {
    local_log "Sort selection changed - updating table"
    update_performance_table
}

local_log "Behavmon widget setup complete"

)tcl");
    
    initializeWidget();
}

EssBehavmonWidget::~EssBehavmonWidget()
{
    // Qt handles cleanup automatically
}

void EssBehavmonWidget::registerCustomCommands()
{
    if (!interpreter()) return;
    
    // Register simple UI update commands
    Tcl_CreateObjCommand(interpreter(), "set_general_performance", tcl_set_general_performance, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "set_performance_table", tcl_set_performance_table, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "set_sort_options", tcl_set_sort_options, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "get_sort_selection", tcl_get_sort_selection, this, nullptr);
    Tcl_CreateObjCommand(interpreter(), "clear_table", tcl_clear_table, this, nullptr);
}

QWidget* EssBehavmonWidget::createMainWidget()
{
    m_mainWidget = new QWidget();

    // Set size policy to expand and fill available space
    m_mainWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    
    // Create main layout
    QVBoxLayout* mainLayout = new QVBoxLayout(m_mainWidget);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(6);
    
    // Setup the three main areas
    setupGeneralPerformanceArea();
    setupDetailedPerformanceArea();
    setupControlsArea();
    
    m_generalGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_detailedGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_controlsGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    
    // Add to main layout with proper sizing
    mainLayout->addWidget(m_generalGroup, 0);      // Fixed size
    mainLayout->addWidget(m_detailedGroup, 1);     // Expanding
    mainLayout->addWidget(m_controlsGroup, 0);     // Fixed size

    
    return m_mainWidget;
}

void EssBehavmonWidget::setupGeneralPerformanceArea()
{
    m_generalGroup = new QGroupBox("Performance Overview");
    m_generalGroup->setMaximumHeight(120); // Reduced from 140
    
    QVBoxLayout* mainLayout = new QVBoxLayout(m_generalGroup);
    mainLayout->setContentsMargins(6, 6, 6, 6); // Reduced from 8
    mainLayout->setSpacing(6);
    
    // Create more compact card container
    QHBoxLayout* cardsLayout = new QHBoxLayout();
    cardsLayout->setSpacing(4); // Reduced from 6
    
    // Helper function to create compact cards
    auto createCompactCard = [this](const QString& title, const QString& color1, const QString& color2) -> QFrame* {
        QFrame* card = new QFrame();
        card->setFrameStyle(QFrame::StyledPanel);
        card->setStyleSheet(QString(
            "QFrame { "
            "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %1, stop:1 %2);"
            "  border-radius: 6px;" // Reduced from 8px
            "  border: 1px solid %2;"
            "}"
            "QLabel { background: transparent; color: white; }"
        ).arg(color1, color2));
        
        QVBoxLayout* layout = new QVBoxLayout(card);
        layout->setContentsMargins(4, 3, 4, 3); // Reduced padding
        layout->setSpacing(1); // Reduced spacing
        
        QLabel* titleLabel = new QLabel(title);
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setStyleSheet("font-weight: bold; font-size: 9px;"); // Smaller font
        
        QLabel* valueLabel = new QLabel("0");
        valueLabel->setAlignment(Qt::AlignCenter);
        valueLabel->setStyleSheet("font-weight: bold; font-size: 12px;"); // Slightly smaller
        
        layout->addWidget(titleLabel);
        layout->addWidget(valueLabel);
        
        // Store value label for updates
        if (title == "% Correct") m_percentCorrectLabel = valueLabel;
        else if (title == "% Complete") m_percentCompleteLabel = valueLabel;
        else if (title == "Trials") m_totalTrialsLabel = valueLabel;
        
        return card;
    };
    
    // Create compact cards with shorter title
    QFrame* correctCard = createCompactCard("% Correct", "#4CAF50", "#45a049");
    QFrame* completeCard = createCompactCard("% Complete", "#2196F3", "#1976D2");
    QFrame* trialsCard = createCompactCard("Trials", "#FF9800", "#F57C00"); // Shorter title
    
    // Make all cards the same size and fill available space equally
    correctCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    completeCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    trialsCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    
    // Add cards to layout with equal stretch
    cardsLayout->addWidget(correctCard, 1);    // Equal stretch factor
    cardsLayout->addWidget(completeCard, 1);   // Equal stretch factor  
    cardsLayout->addWidget(trialsCard, 1);     // Equal stretch factor
    // No addStretch() - let the cards fill the space
    
    mainLayout->addLayout(cardsLayout);
    
    // Keep progress bars for backwards compatibility (hidden by default)
    m_correctProgress = new QProgressBar();
    m_correctProgress->setRange(0, 100);
    m_correctProgress->setVisible(false);
    
    m_completeProgress = new QProgressBar();
    m_completeProgress->setRange(0, 100);
    m_completeProgress->setVisible(false);
}

void EssBehavmonWidget::setupDetailedPerformanceArea()
{
    m_detailedGroup = new QGroupBox("Detailed Performance");
    
    QVBoxLayout* layout = new QVBoxLayout(m_detailedGroup);
    layout->setContentsMargins(6, 6, 6, 6); // Reduced from 8
    layout->setSpacing(4); // Reduced from 6
    
    // More compact sorting controls
    QHBoxLayout* sortLayout = new QHBoxLayout();
    sortLayout->setSpacing(4); // Reduced spacing
    
    QLabel* sortLabel = new QLabel("Sort:");  // Shorter label
    sortLabel->setStyleSheet("font-size: 9px; font-weight: bold;"); // Smaller font
    
    m_primarySortCombo = new QComboBox();
    m_primarySortCombo->addItem("(none)", "");
    m_primarySortCombo->setMinimumWidth(70); // Reduced from 90
    m_primarySortCombo->setMaximumWidth(90);
    m_primarySortCombo->setStyleSheet("font-size: 9px;"); // Smaller font
    
    m_secondarySortCombo = new QComboBox();
    m_secondarySortCombo->addItem("(none)", "");
    m_secondarySortCombo->setMinimumWidth(70); // Reduced from 90
    m_secondarySortCombo->setMaximumWidth(90);
    m_secondarySortCombo->setStyleSheet("font-size: 9px;"); // Smaller font
    
    QLabel* thenLabel = new QLabel("then:");  // Shorter label
    thenLabel->setStyleSheet("font-size: 9px;"); // Smaller font
    
    // Connect to Tcl callback (same as before)
    connect(m_primarySortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() {
        emit sortSelectionChanged(
            m_primarySortCombo->currentData().toString(),
            m_secondarySortCombo->currentData().toString()
        );
        if (interpreter()) {
            eval("on_sort_changed");
        }
    });
    
    connect(m_secondarySortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [this]() {
        emit sortSelectionChanged(
            m_primarySortCombo->currentData().toString(),
            m_secondarySortCombo->currentData().toString()
        );
        if (interpreter()) {
            eval("on_sort_changed");
        }
    });
    
    sortLayout->addWidget(sortLabel);
    sortLayout->addWidget(m_primarySortCombo);
    sortLayout->addWidget(thenLabel);
    sortLayout->addWidget(m_secondarySortCombo);
    sortLayout->addStretch();
    
    layout->addLayout(sortLayout);
    
    // Performance table with better scrolling
    m_detailedTable = new QTableWidget();
    m_detailedTable->setAlternatingRowColors(true);
    m_detailedTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_detailedTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_detailedTable->setSortingEnabled(false); // Sorting handled by Tcl
    m_detailedTable->verticalHeader()->setVisible(false);
    
    // Configure headers for compact display
    m_detailedTable->horizontalHeader()->setStretchLastSection(false); // Changed to false
    m_detailedTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_detailedTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    m_detailedTable->horizontalHeader()->setMinimumSectionSize(30); // Minimum column width
    
    // Enable scrolling for both directions
    m_detailedTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_detailedTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    
    // Set nice styling with smaller fonts
    m_detailedTable->setStyleSheet(
        "QTableWidget { "
        "  gridline-color: #d0d0d0; "
        "  font-size: 9px;"  // Smaller font for table
        "}"
        "QTableWidget::item { "
        "  padding: 2px;" // Reduced padding
        "}"
        "QHeaderView::section { "
        "  background-color: #f0f0f0; "
        "  font-weight: bold; "
        "  font-size: 9px;"  // Smaller header font
        "  padding: 2px;"    // Reduced header padding
        "}"
    );
    
    layout->addWidget(m_detailedTable);
}

void EssBehavmonWidget::setupControlsArea()
{
    m_controlsGroup = new QGroupBox("Controls");
    m_controlsGroup->setMaximumHeight(50); // Reduced from 60
    
    QHBoxLayout* layout = new QHBoxLayout(m_controlsGroup);
    layout->setContentsMargins(6, 6, 6, 6); // Reduced from 8
    layout->setSpacing(4); // Reduced from 6
    
    // Smaller development mode toggle button
    QPushButton* devModeButton = new QPushButton("Dev");  // Shorter text
    devModeButton->setCheckable(true);
    devModeButton->setToolTip("Toggle development mode");
    devModeButton->setMaximumWidth(40); // Constrain width
    devModeButton->setStyleSheet(
        "QPushButton { "
        "  background-color: #f0f0f0; "
        "  border: 1px solid #ccc; "
        "  border-radius: 3px;"  // Slightly smaller radius
        "  padding: 2px 4px;"    // Reduced padding
        "  font-weight: bold; "
        "  font-size: 9px;"      // Smaller font
        "}"
        "QPushButton:checked { "
        "  background-color: #4CAF50; "
        "  color: white; "
        "  border-color: #45a049; "
        "}"
        "QPushButton:hover { "
        "  background-color: #e0e0e0; "
        "}"
        "QPushButton:checked:hover { "
        "  background-color: #45a049; "
        "}"
    );
    devModeButton->setChecked(isDevelopmentMode());
    
    connect(devModeButton, &QPushButton::toggled, [this](bool checked) {
        setDevelopmentMode(checked);
        if (checked) {
            setDevelopmentLayout(DevBottomPanel);
            localLog("Development mode enabled via button");
        } else {
            localLog("Development mode disabled via button");
        }
    });
    
    // Smaller control buttons
    m_resetButton = new QPushButton("Reset");  // Shorter text
    m_resetButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_resetButton->setStyleSheet("font-size: 9px; padding: 2px 4px;");
    m_resetButton->setMaximumWidth(60);
    connect(m_resetButton, &QPushButton::clicked, [this]() {
        emit resetRequested();
        if (interpreter()) {
            eval("clear_behavmon_data");
        }
    });
    
    m_exportButton = new QPushButton("Export");  // Shorter text
    m_exportButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_exportButton->setStyleSheet("font-size: 9px; padding: 2px 4px;");
    m_exportButton->setMaximumWidth(60);
    connect(m_exportButton, &QPushButton::clicked, [this]() {
        emit exportRequested();
        // Could add Tcl export callback here
    });
    
    layout->addWidget(devModeButton);
    
    // Add visual separator
    QFrame* separator = new QFrame();
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    separator->setMaximumWidth(2);
    layout->addWidget(separator);
    
    layout->addWidget(m_resetButton);
    layout->addWidget(m_exportButton);
    layout->addStretch();
}

// Tcl command implementations - just UI updates, no business logic

int EssBehavmonWidget::tcl_set_general_performance(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssBehavmonWidget*>(clientData);
    
    if (objc != 4) {
        Tcl_WrongNumArgs(interp, 1, objv, "percent_correct percent_complete total_trials");
        return TCL_ERROR;
    }
    
    int percentCorrect, percentComplete, totalTrials;
    
    if (Tcl_GetIntFromObj(interp, objv[1], &percentCorrect) != TCL_OK ||
        Tcl_GetIntFromObj(interp, objv[2], &percentComplete) != TCL_OK ||
        Tcl_GetIntFromObj(interp, objv[3], &totalTrials) != TCL_OK) {
        return TCL_ERROR;
    }
    
    // Update UI elements (both cards and progress bars)
    widget->m_percentCorrectLabel->setText(QString("%1%").arg(percentCorrect));
    widget->m_percentCompleteLabel->setText(QString("%1%").arg(percentComplete));
    widget->m_totalTrialsLabel->setText(QString::number(totalTrials));
    
    widget->m_correctProgress->setValue(percentCorrect);
    widget->m_completeProgress->setValue(percentComplete);
    
    return TCL_OK;
}

int EssBehavmonWidget::tcl_set_performance_table(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssBehavmonWidget*>(clientData);
    
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "headers rows");
        return TCL_ERROR;
    }
    
    // Parse headers
    Tcl_Size headerCount;
    Tcl_Obj** headerObjs;
    if (Tcl_ListObjGetElements(interp, objv[1], &headerCount, &headerObjs) != TCL_OK) {
        return TCL_ERROR;
    }
    
    QStringList headers;
    for (Tcl_Size i = 0; i < headerCount; i++) {
        headers << QString::fromUtf8(Tcl_GetString(headerObjs[i]));
    }
    
    // Parse rows
    Tcl_Size rowCount;
    Tcl_Obj** rowObjs;
    if (Tcl_ListObjGetElements(interp, objv[2], &rowCount, &rowObjs) != TCL_OK) {
        return TCL_ERROR;
    }
    
    // Setup table
    widget->m_detailedTable->clear();
    widget->m_detailedTable->setRowCount(rowCount);
    widget->m_detailedTable->setColumnCount(headers.size());
    widget->m_detailedTable->setHorizontalHeaderLabels(headers);
    
    // Fill table data
    for (Tcl_Size row = 0; row < rowCount; row++) {
        Tcl_Size colCount;
        Tcl_Obj** colObjs;
        if (Tcl_ListObjGetElements(interp, rowObjs[row], &colCount, &colObjs) != TCL_OK) {
            continue;
        }
        
        for (Tcl_Size col = 0; col < colCount && col < headers.size(); col++) {
            QString text = QString::fromUtf8(Tcl_GetString(colObjs[col]));
            QTableWidgetItem* item = new QTableWidgetItem(text);
            
            // Center-align numeric-looking columns
            if (headers[col].contains("correct", Qt::CaseInsensitive) ||
                headers[col].contains("rt", Qt::CaseInsensitive) ||
                headers[col].contains("n", Qt::CaseInsensitive) ||
                QRegularExpression("^\\d+(\\.\\d+)?$").match(text).hasMatch()) {
                item->setTextAlignment(Qt::AlignCenter);
            }
            
            widget->m_detailedTable->setItem(row, col, item);
        }
    }
    
    // Auto-resize columns for nice appearance
    widget->m_detailedTable->resizeColumnsToContents();
    
    return TCL_OK;
}

int EssBehavmonWidget::tcl_set_sort_options(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssBehavmonWidget*>(clientData);
    
    if (objc != 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option_list");
        return TCL_ERROR;
    }
    
    // Parse option list
    Tcl_Size optionCount;
    Tcl_Obj** optionObjs;
    if (Tcl_ListObjGetElements(interp, objv[1], &optionCount, &optionObjs) != TCL_OK) {
        return TCL_ERROR;
    }
    
    // Store current selections
    QString currentPrimary = widget->m_primarySortCombo->currentData().toString();
    QString currentSecondary = widget->m_secondarySortCombo->currentData().toString();
    
    // Clear and rebuild combo boxes
    widget->m_primarySortCombo->clear();
    widget->m_secondarySortCombo->clear();
    
    widget->m_primarySortCombo->addItem("(none)", "");
    widget->m_secondarySortCombo->addItem("(none)", "");
    
    for (Tcl_Size i = 0; i < optionCount; i++) {
        QString option = QString::fromUtf8(Tcl_GetString(optionObjs[i]));
        widget->m_primarySortCombo->addItem(option, option);
        widget->m_secondarySortCombo->addItem(option, option);
    }
    
    // Restore selections if they still exist
    int primaryIndex = widget->m_primarySortCombo->findData(currentPrimary);
    if (primaryIndex >= 0) {
        widget->m_primarySortCombo->setCurrentIndex(primaryIndex);
    }
    
    int secondaryIndex = widget->m_secondarySortCombo->findData(currentSecondary);
    if (secondaryIndex >= 0) {
        widget->m_secondarySortCombo->setCurrentIndex(secondaryIndex);
    }
    
    return TCL_OK;
}

int EssBehavmonWidget::tcl_get_sort_selection(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssBehavmonWidget*>(clientData);
    
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    QString primary = widget->m_primarySortCombo->currentData().toString();
    QString secondary = widget->m_secondarySortCombo->currentData().toString();
    
    // Return as a list
    Tcl_Obj* result = Tcl_NewListObj(0, nullptr);
    Tcl_ListObjAppendElement(interp, result, Tcl_NewStringObj(primary.toUtf8().constData(), -1));
    Tcl_ListObjAppendElement(interp, result, Tcl_NewStringObj(secondary.toUtf8().constData(), -1));
    
    Tcl_SetObjResult(interp, result);
    return TCL_OK;
}

int EssBehavmonWidget::tcl_clear_table(ClientData clientData, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[])
{
    auto* widget = static_cast<EssBehavmonWidget*>(clientData);
    
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    
    widget->m_detailedTable->clear();
    widget->m_detailedTable->setRowCount(0);
    widget->m_detailedTable->setColumnCount(0);
    
    return TCL_OK;
}
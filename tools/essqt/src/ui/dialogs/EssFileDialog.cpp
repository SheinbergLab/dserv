#include "EssFileDialog.h"
#include "core/EssApplication.h"
#include "core/EssCommandInterface.h"
#include "console/EssOutputConsole.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QMessageBox>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QTimer>

EssFileDialog::EssFileDialog(QWidget *parent)
    : QDialog(parent)
    , m_filenameEdit(nullptr)
    , m_suggestBtn(nullptr)
    , m_okBtn(nullptr)
    , m_cancelBtn(nullptr)
    , m_statusLabel(nullptr)
    , m_progressBar(nullptr)
    , m_suggesting(false)
{
    setupUi();
    connectSignals();
    
    // Auto-suggest filename on dialog open
    QTimer::singleShot(100, this, &EssFileDialog::suggestFilename);
}

EssFileDialog::~EssFileDialog()
{
}

void EssFileDialog::setupUi()
{
    setWindowTitle("Open Data File");
    setMinimumWidth(600);  // Wider for long filenames
    setModal(true);
    
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(16);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    
    // Title
    QLabel *titleLabel = new QLabel("Select or enter a data filename:");
    titleLabel->setStyleSheet("QLabel { font-weight: bold; font-size: 14px; }");
    mainLayout->addWidget(titleLabel);
    
    // Form layout for filename input
    QFormLayout *formLayout = new QFormLayout();
    formLayout->setSpacing(8);
    
    // Filename input with suggestion button
    QHBoxLayout *filenameLayout = new QHBoxLayout();
    m_filenameEdit = new QLineEdit();
    m_filenameEdit->setPlaceholderText("Enter filename or click Suggest...");
    m_filenameEdit->setMinimumWidth(400);  // Make input wider for long filenames
    
    // Add validator for reasonable filename characters
    QRegularExpression filenameRegex("[a-zA-Z0-9_\\-\\.]+");
    m_filenameEdit->setValidator(new QRegularExpressionValidator(filenameRegex, this));
    
    m_suggestBtn = new QPushButton("Suggest");
    m_suggestBtn->setToolTip("Get suggested filename from server");
    m_suggestBtn->setMaximumWidth(80);
    
    filenameLayout->addWidget(m_filenameEdit, 1);
    filenameLayout->addWidget(m_suggestBtn);
    
    formLayout->addRow("Filename:", filenameLayout);
    mainLayout->addLayout(formLayout);
    
    // Status area
    QVBoxLayout *statusLayout = new QVBoxLayout();
    statusLayout->setSpacing(4);
    
    m_statusLabel = new QLabel();
    m_statusLabel->setStyleSheet("QLabel { color: #666; font-size: 12px; }");
    m_statusLabel->hide(); // Hidden by default
    
    m_progressBar = new QProgressBar();
    m_progressBar->setRange(0, 0); // Indeterminate
    m_progressBar->hide(); // Hidden by default
    m_progressBar->setMaximumHeight(6);
    
    statusLayout->addWidget(m_statusLabel);
    statusLayout->addWidget(m_progressBar);
    mainLayout->addLayout(statusLayout);
    
    // Button box
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    
    m_cancelBtn = new QPushButton("Cancel");
    m_okBtn = new QPushButton("Open File");
    m_okBtn->setDefault(true);
    
    // Style both buttons consistently
    QString buttonStyle = 
        "QPushButton { "
        "  padding: 8px 16px; "
        "  border-radius: 4px; "
        "  border: 1px solid #d9d9d9; "
        "  font-weight: normal; "
        "} "
        "QPushButton:hover { "
        "  border-color: #40a9ff; "
        "} ";
    
    // Style the OK button
    m_okBtn->setStyleSheet(buttonStyle +
        "QPushButton { "
        "  background-color: #1890ff; "
        "  color: white; "
        "  border-color: #1890ff; "
        "  font-weight: bold; "
        "} "
        "QPushButton:hover { "
        "  background-color: #40a9ff; "
        "  border-color: #40a9ff; "
        "} "
        "QPushButton:disabled { "
        "  background-color: #d9d9d9; "
        "  color: #999; "
        "  border-color: #d9d9d9; "
        "}"
    );
    
    // Style the Cancel button
    m_cancelBtn->setStyleSheet(buttonStyle +
        "QPushButton { "
        "  background-color: white; "
        "  color: #333; "
        "} "
        "QPushButton:hover { "
        "  background-color: #f5f5f5; "
        "}"
    );
    
    buttonLayout->addWidget(m_cancelBtn);
    buttonLayout->addWidget(m_okBtn);
    mainLayout->addLayout(buttonLayout);
    
    // Initial state
    updateButtonStates();
}

void EssFileDialog::connectSignals()
{
    connect(m_filenameEdit, &QLineEdit::textChanged, 
            this, &EssFileDialog::onFilenameChanged);
    connect(m_filenameEdit, &QLineEdit::returnPressed,
            this, &EssFileDialog::onAccepted);
            
    connect(m_suggestBtn, &QPushButton::clicked,
            this, &EssFileDialog::onSuggestClicked);
            
    connect(m_okBtn, &QPushButton::clicked,
            this, &EssFileDialog::onAccepted);
    connect(m_cancelBtn, &QPushButton::clicked,
            this, &QDialog::reject);
}

QString EssFileDialog::selectedFilename() const
{
    return m_selectedFilename;
}

QString EssFileDialog::getDatafileName(QWidget *parent)
{
    EssFileDialog dialog(parent);
    if (dialog.exec() == QDialog::Accepted) {
        return dialog.selectedFilename();
    }
    return QString();
}

void EssFileDialog::suggestFilename()
{
    if (m_suggesting) {
        return;
    }
    
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (!cmdInterface || !cmdInterface->isConnected()) {
        m_statusLabel->setText("Not connected to server");
        m_statusLabel->setStyleSheet("QLabel { color: #f5222d; }");
        m_statusLabel->show();
        return;
    }
    
    m_suggesting = true;
    m_suggestBtn->setEnabled(false);
    m_progressBar->show();
    m_statusLabel->setText("Getting filename suggestion...");
    m_statusLabel->setStyleSheet("QLabel { color: #1890ff; }");
    m_statusLabel->show();
    
    // Execute the suggestion command
    auto result = cmdInterface->executeEss("::ess::file_suggest");
    
    m_suggesting = false;
    m_progressBar->hide();
    m_suggestBtn->setEnabled(true);
    
    if (result.status == EssCommandInterface::StatusSuccess) {
        QString suggestion = result.response.trimmed();
        if (!suggestion.isEmpty()) {
            m_filenameEdit->setText(suggestion);
            m_statusLabel->setText(QString("Suggested: %1").arg(suggestion));
            m_statusLabel->setStyleSheet("QLabel { color: #52c41a; }");
            
            EssConsoleManager::instance()->logSuccess(
                QString("Filename suggested: %1").arg(suggestion), 
                "FileDialog"
            );
        } else {
            m_statusLabel->setText("No suggestion available");
            m_statusLabel->setStyleSheet("QLabel { color: #faad14; }");
        }
    } else {
        m_statusLabel->setText("Failed to get suggestion");
        m_statusLabel->setStyleSheet("QLabel { color: #f5222d; }");
        
        EssConsoleManager::instance()->logError(
            QString("Failed to get filename suggestion: %1").arg(result.error),
            "FileDialog"
        );
    }
    
    updateButtonStates();
}

void EssFileDialog::onSuggestClicked()
{
    suggestFilename();
}

void EssFileDialog::onFilenameChanged()
{
    updateButtonStates();
    
    // Clear status when user types
    if (!m_suggesting) {
        m_statusLabel->hide();
    }
}

void EssFileDialog::onAccepted()
{
    QString filename = m_filenameEdit->text().trimmed();
    
    if (!validateFilename(filename)) {
        return; // Validation error already shown
    }
    
    // Check connection
    EssCommandInterface *cmdInterface = EssApplication::instance()->commandInterface();
    if (!cmdInterface || !cmdInterface->isConnected()) {
        showValidationError("Not connected to server");
        return;
    }
    
    // Try to open the file
    m_okBtn->setEnabled(false);
    m_progressBar->show();
    m_statusLabel->setText("Opening file...");
    m_statusLabel->setStyleSheet("QLabel { color: #1890ff; }");
    m_statusLabel->show();
    
    QString command = QString("::ess::file_open %1").arg(filename);
    auto result = cmdInterface->executeEss(command);
    
    m_progressBar->hide();
    m_okBtn->setEnabled(true);
    
    if (result.status == EssCommandInterface::StatusSuccess) {
        QString response = result.response.trimmed();
        
        if (response == "1") {
            // Success
            m_selectedFilename = filename;
            EssConsoleManager::instance()->logSuccess(
                QString("Datafile opened: %1").arg(filename),
                "FileDialog"
            );
            accept();
        } else if (response == "0") {
            showValidationError(QString("File '%1' already exists").arg(filename));
        } else if (response == "-1") {
            showValidationError("Another file is already open. Close it first.");
        } else {
            showValidationError(QString("Unexpected response: %1").arg(response));
        }
    } else {
        showValidationError(QString("Failed to open file: %1").arg(result.error));
    }
}

void EssFileDialog::updateButtonStates()
{
    QString filename = m_filenameEdit->text().trimmed();
    bool hasFilename = !filename.isEmpty();
    bool isConnected = false;
    
    if (EssApplication::instance() && EssApplication::instance()->commandInterface()) {
        isConnected = EssApplication::instance()->commandInterface()->isConnected();
    }
    
    m_okBtn->setEnabled(hasFilename && isConnected && !m_suggesting);
    m_suggestBtn->setEnabled(isConnected && !m_suggesting);
}

bool EssFileDialog::validateFilename(const QString &filename) const
{
    if (filename.isEmpty()) {
        const_cast<EssFileDialog*>(this)->showValidationError("Please enter a filename");
        return false;
    }
    
    // Check for invalid characters (basic validation)
    if (filename.contains('/') || filename.contains('\\') || 
        filename.contains(':') || filename.contains('*') ||
        filename.contains('?') || filename.contains('"') ||
        filename.contains('<') || filename.contains('>') ||
        filename.contains('|')) {
        const_cast<EssFileDialog*>(this)->showValidationError(
            "Filename contains invalid characters"
        );
        return false;
    }
    
    return true;
}

void EssFileDialog::showValidationError(const QString &message)
{
    m_statusLabel->setText(message);
    m_statusLabel->setStyleSheet("QLabel { color: #f5222d; }");
    m_statusLabel->show();
    
    EssConsoleManager::instance()->logError(message, "FileDialog");
}
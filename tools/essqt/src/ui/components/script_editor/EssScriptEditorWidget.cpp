#include "EssScriptEditorWidget.h"
#include "core/EssApplication.h"
#include "core/EssCommandInterface.h"
#include "console/EssOutputConsole.h"

#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexertcl.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexerbash.h>

#include <QVBoxLayout>
#include <QToolBar>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QTextStream>
#include <QFileInfo>
#include <QLabel>
#include <QKeySequence>

EssScriptEditorWidget::EssScriptEditorWidget(QWidget *parent)
    : QWidget(parent)
    , m_editor(new QsciScintilla(this))
    , m_statusLabel(new QLabel(this))
    , m_currentFile()
{
    setupEditor();
    setupActions();
    createToolBar();
    
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_toolbar);
    layout->addWidget(m_editor);
    layout->addWidget(m_statusLabel);
    
    m_statusLabel->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    m_statusLabel->setMinimumHeight(20);
    
    setCurrentFile(QString());
}

EssScriptEditorWidget::~EssScriptEditorWidget() = default;



void EssScriptEditorWidget::setupEmacsBindings()
{
    // Install event filter to catch key events before QScintilla
    m_editor->installEventFilter(this);
}

bool EssScriptEditorWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_editor && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        
        // Tab key - smart indent
        if (keyEvent->key() == Qt::Key_Tab && keyEvent->modifiers() == Qt::NoModifier) {
            smartIndent();
            return true;
        }
        
        // Emacs bindings
        if (keyEvent->modifiers() & Qt::ControlModifier) {
            switch (keyEvent->key()) {
                case Qt::Key_A: // Beginning of line
                    m_editor->SendScintilla(QsciScintillaBase::SCI_VCHOME);
                    return true;
                    
                case Qt::Key_E: // End of line
                    m_editor->SendScintilla(QsciScintillaBase::SCI_LINEEND);
                    return true;
                    
                case Qt::Key_K: // Kill to end of line
                    m_editor->SendScintilla(QsciScintillaBase::SCI_DELLINERIGHT);
                    return true;
                    
                case Qt::Key_D: // Delete character forward
                    m_editor->SendScintilla(QsciScintillaBase::SCI_CLEAR);
                    return true;
                    
                case Qt::Key_N: // Next line
                    m_editor->SendScintilla(QsciScintillaBase::SCI_LINEDOWN);
                    return true;
                    
                case Qt::Key_P: // Previous line
                    m_editor->SendScintilla(QsciScintillaBase::SCI_LINEUP);
                    return true;
                    
                case Qt::Key_F: // Forward character
                    m_editor->SendScintilla(QsciScintillaBase::SCI_CHARRIGHT);
                    return true;
                    
                case Qt::Key_B: // Backward character
                    m_editor->SendScintilla(QsciScintillaBase::SCI_CHARLEFT);
                    return true;
                    
                case Qt::Key_Space: // Ctrl+Space for auto-completion
                    m_editor->autoCompleteFromAll();
                    return true;
            }
        }
        
        // Alt key bindings
        if (keyEvent->modifiers() & Qt::AltModifier) {
            switch (keyEvent->key()) {
                case Qt::Key_F: // Forward word
                    m_editor->SendScintilla(QsciScintillaBase::SCI_WORDRIGHT);
                    return true;
                    
                case Qt::Key_B: // Backward word
                    m_editor->SendScintilla(QsciScintillaBase::SCI_WORDLEFT);
                    return true;
                    
                case Qt::Key_D: // Delete word forward
                    m_editor->SendScintilla(QsciScintillaBase::SCI_DELWORDRIGHT);
                    return true;
                    
                case Qt::Key_Backspace: // Delete word backward
                    m_editor->SendScintilla(QsciScintillaBase::SCI_DELWORDLEFT);
                    return true;
            }
        }
    }
    
    return QWidget::eventFilter(obj, event);
}

void EssScriptEditorWidget::smartIndent()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    
    if (line > 0) {
        // Get indentation of previous line
        QString prevLineText = m_editor->text(line - 1);
        int prevIndent = 0;
        for (QChar ch : prevLineText) {
            if (ch == ' ') prevIndent++;
            else if (ch == '\t') prevIndent += m_editor->indentationWidth();
            else break;
        }
        
        // Check if previous line ends with { or starts a block
        QString trimmed = prevLineText.trimmed();
        if (trimmed.endsWith('{') || trimmed.startsWith("proc ") || 
            trimmed.startsWith("if ") || trimmed.startsWith("while ") ||
            trimmed.startsWith("for ") || trimmed.startsWith("foreach ")) {
            prevIndent += m_editor->indentationWidth();
        }
        
        // Check current line for closing brace
        QString currentLineText = m_editor->text(line);
        QString currentTrimmed = currentLineText.trimmed();
        if (currentTrimmed.startsWith('}')) {
            prevIndent -= m_editor->indentationWidth();
            if (prevIndent < 0) prevIndent = 0;
        }
        
        // Apply indentation
        m_editor->setIndentation(line, prevIndent);
        
        // Move cursor to end of indentation
        m_editor->setCursorPosition(line, prevIndent);
    }
}


void EssScriptEditorWidget::setupEditor()
{
    // Editor settings
    m_editor->setUtf8(true);
    m_editor->setEolMode(QsciScintilla::EolUnix);
    m_editor->setIndentationsUseTabs(false);
    m_editor->setIndentationWidth(4);
    m_editor->setAutoIndent(true);
    
    // Line numbers margin
    m_editor->setMarginType(0, QsciScintilla::NumberMargin);
    m_editor->setMarginLineNumbers(0, true);
    m_editor->setMarginWidth(0, "00000");
    
    // Folding - use SymbolMargin type for folding
    m_editor->setMarginType(1, QsciScintilla::SymbolMargin);
    m_editor->setFolding(QsciScintilla::BoxedTreeFoldStyle);
    
    // Marker margin for breakpoints/bookmarks
    m_editor->setMarginType(2, QsciScintilla::SymbolMargin);
    m_editor->setMarginWidth(2, 20);
    m_editor->setMarginSensitivity(2, true);
    
    // Current line highlighting
    m_editor->setCaretLineVisible(true);
    
    // Brace matching
    m_editor->setBraceMatching(QsciScintilla::SloppyBraceMatch);
    
    // Enable auto-completion
    m_editor->setAutoCompletionSource(QsciScintilla::AcsAll);
    m_editor->setAutoCompletionThreshold(3);  // Show after 3 characters
    m_editor->setAutoCompletionCaseSensitivity(false);
    m_editor->setAutoCompletionReplaceWord(true);
    
    // Set default lexer (Tcl)
    auto tclLexer = new QsciLexerTCL(m_editor);
    m_lexer.reset(tclLexer);
    m_editor->setLexer(m_lexer.get());
    
    // Apply dark theme
    //applyTheme();
    
    // Connect signals
    connect(m_editor, &QsciScintilla::textChanged,
            this, &EssScriptEditorWidget::onTextChanged);
    connect(m_editor, &QsciScintilla::cursorPositionChanged,
            this, &EssScriptEditorWidget::onCursorPositionChanged);
    connect(m_editor, &QsciScintilla::modificationChanged,
            this, &EssScriptEditorWidget::onModificationChanged);
    connect(m_editor, &QsciScintilla::marginClicked,
            this, [this](int margin, int line, Qt::KeyboardModifiers) {
                if (margin == 2) {
                    // Toggle bookmark
                    if (m_editor->markersAtLine(line) & (1 << 1)) {
                        m_editor->markerDelete(line, 1);
                    } else {
                        m_editor->markerAdd(line, 1);
                    }
                }
            });
    
    // Define bookmark marker
    m_editor->markerDefine(QsciScintilla::Circle, 1);
    m_editor->setMarkerBackgroundColor(QColor(255, 195, 0), 1);
    
    setupEmacsBindings();    
}

void EssScriptEditorWidget::applyTheme()
{
    // Base colors (matching your terminal theme)
    m_editor->setPaper(QColor(40, 44, 52));  // Background
    m_editor->setColor(QColor(171, 178, 191)); // Default text
    
    // Margins
    m_editor->setMarginsBackgroundColor(QColor(40, 44, 52));
    m_editor->setMarginsForegroundColor(QColor(100, 100, 100));
    m_editor->setFoldMarginColors(QColor(40, 44, 52), QColor(40, 44, 52));
    
    // Selection
    m_editor->setSelectionBackgroundColor(QColor(61, 90, 128));
    m_editor->setSelectionForegroundColor(Qt::white);
    
    // Caret line
    m_editor->setCaretLineBackgroundColor(QColor(50, 54, 62));
    m_editor->setCaretForegroundColor(QColor(171, 178, 191));
    
    // Matching braces
    m_editor->setMatchedBraceBackgroundColor(QColor(86, 182, 255));
    m_editor->setMatchedBraceForegroundColor(Qt::white);
    
    if (auto tcl = qobject_cast<QsciLexerTCL*>(m_lexer.get())) {
        // Apply theme to lexer
        QFont font("Consolas, Monaco, Courier New, monospace", 10);
        font.setFixedPitch(true);
        
        // Set paper and default colors for all styles
        for (int i = 0; i < 128; ++i) {
            tcl->setPaper(QColor(40, 44, 52), i);
            tcl->setFont(font, i);
        }
        
        // Tcl-specific syntax colors
        tcl->setColor(QColor(171, 178, 191), QsciLexerTCL::Default);
        tcl->setColor(QColor(224, 108, 117), QsciLexerTCL::Comment);
        tcl->setColor(QColor(224, 108, 117), QsciLexerTCL::CommentLine);
        // QuotedString is the enum for strings in TCL lexer
        tcl->setColor(QColor(152, 195, 121), QsciLexerTCL::QuotedString);
        tcl->setColor(QColor(229, 192, 123), QsciLexerTCL::Number);
        tcl->setColor(QColor(198, 120, 221), QsciLexerTCL::TCLKeyword);
        tcl->setColor(QColor(86, 182, 255), QsciLexerTCL::TkKeyword);
        tcl->setColor(QColor(97, 175, 239), QsciLexerTCL::ITCLKeyword);
        tcl->setColor(QColor(224, 108, 117), QsciLexerTCL::Operator);
        tcl->setColor(QColor(171, 178, 191), QsciLexerTCL::Identifier);
        tcl->setColor(QColor(229, 192, 123), QsciLexerTCL::Substitution);
        tcl->setColor(QColor(97, 175, 239), QsciLexerTCL::SubstitutionBrace);
    }
}

void EssScriptEditorWidget::setupActions()
{
    // File actions
    m_newAction = new QAction(tr("&New"), this);
    m_newAction->setShortcut(QKeySequence::New);
    connect(m_newAction, &QAction::triggered, this, &EssScriptEditorWidget::newFile);
    
    m_openAction = new QAction(tr("&Open..."), this);
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, [this]() { openFile(); });
    
    m_saveAction = new QAction(tr("&Save"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    connect(m_saveAction, &QAction::triggered, this, &EssScriptEditorWidget::saveFile);
    
    m_saveAsAction = new QAction(tr("Save &As..."), this);
    m_saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(m_saveAsAction, &QAction::triggered, this, &EssScriptEditorWidget::saveFileAs);
    
    // Execute actions
    m_executeSelAction = new QAction(tr("Execute &Selection"), this);
    m_executeSelAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    connect(m_executeSelAction, &QAction::triggered, 
            this, &EssScriptEditorWidget::executeSelection);
    
    m_executeAllAction = new QAction(tr("Execute &All"), this);
    m_executeAllAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Return));
    connect(m_executeAllAction, &QAction::triggered, 
            this, &EssScriptEditorWidget::executeAll);
    
    // Find action
    m_findAction = new QAction(tr("&Find..."), this);
    m_findAction->setShortcut(QKeySequence::Find);
    connect(m_findAction, &QAction::triggered, [this]() {
        // QScintilla has built-in find dialog
        m_editor->findFirst(QString(), false, false, false, true);
    });
}

void EssScriptEditorWidget::createToolBar()
{
    m_toolbar = new QToolBar(this);
    m_toolbar->setMovable(false);
    
    m_toolbar->addAction(m_newAction);
    m_toolbar->addAction(m_openAction);
    m_toolbar->addAction(m_saveAction);
    m_toolbar->addSeparator();
    m_toolbar->addAction(m_executeSelAction);
    m_toolbar->addAction(m_executeAllAction);
    m_toolbar->addSeparator();
    m_toolbar->addAction(m_findAction);
}

void EssScriptEditorWidget::newFile()
{
    if (maybeSave()) {
        m_editor->clear();
        setCurrentFile(QString());
    }
}

void EssScriptEditorWidget::openFile(const QString &path)
{
    QString fileName = path;
    if (fileName.isEmpty()) {
        fileName = QFileDialog::getOpenFileName(this, 
            tr("Open Script"), m_defaultPath,
            tr("Tcl Scripts (*.tcl);;Python Scripts (*.py);;All Files (*)"));
    }
    
    if (!fileName.isEmpty()) {
        loadFile(fileName);
    }
}

void EssScriptEditorWidget::loadFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QFile::ReadOnly | QFile::Text)) {
        QMessageBox::warning(this, tr("Script Editor"),
                             tr("Cannot read file %1:\n%2.")
                             .arg(path, file.errorString()));
        return;
    }
    
    QTextStream in(&file);
    m_editor->setText(in.readAll());
    
    setCurrentFile(path);
    setLexerForFile(path);
    emit fileOpened(path);
    emit statusMessage(tr("Opened %1").arg(QFileInfo(path).fileName()), 3000);
}

bool EssScriptEditorWidget::saveFile()
{
    if (m_currentFile.isEmpty()) {
        return saveFileAs();
    }
    
    QFile file(m_currentFile);
    if (!file.open(QFile::WriteOnly | QFile::Text)) {
        QMessageBox::warning(this, tr("Script Editor"),
                             tr("Cannot write file %1:\n%2.")
                             .arg(m_currentFile, file.errorString()));
        return false;
    }
    
    QTextStream out(&file);
    out << m_editor->text();
    
    m_editor->setModified(false);
    emit fileSaved(m_currentFile);
    emit statusMessage(tr("Saved %1").arg(QFileInfo(m_currentFile).fileName()), 3000);
    return true;
}

bool EssScriptEditorWidget::saveFileAs()
{
    QString fileName = QFileDialog::getSaveFileName(this,
        tr("Save Script"), m_defaultPath,
        tr("Tcl Scripts (*.tcl);;Python Scripts (*.py);;All Files (*)"));
    
    if (fileName.isEmpty())
        return false;
    
    m_currentFile = fileName;
    return saveFile();
}

void EssScriptEditorWidget::executeSelection()
{
    QString code;
    if (m_editor->hasSelectedText()) {
        code = m_editor->selectedText();
    } else {
        // Execute current line
        int line, index;
        m_editor->getCursorPosition(&line, &index);
        code = m_editor->text(line);
    }
    
    if (!code.trimmed().isEmpty()) {
        emit executeRequested(code);
        
        // Execute through command interface
        auto cmdInterface = EssApplication::instance()->commandInterface();
        if (cmdInterface && cmdInterface->isConnected()) {
            // Use the appropriate async method based on file type or current channel
            if (m_currentFile.endsWith(".tcl") || 
                cmdInterface->defaultChannel() == EssCommandInterface::ChannelLocal) {
                // Execute as Tcl
                cmdInterface->executeCommand("/tcl " + code);
            } else {
                // Execute as ESS command
                cmdInterface->executeEssAsync(code);
            }
            
            EssConsoleManager::instance()->logInfo(
                QString("Executing: %1...").arg(code.left(50)), 
                "ScriptEditor"
            );
        }
    }
}

void EssScriptEditorWidget::executeAll()
{
    QString code = m_editor->text();
    if (!code.trimmed().isEmpty()) {
        emit executeRequested(code);
        
        auto cmdInterface = EssApplication::instance()->commandInterface();
        if (cmdInterface && cmdInterface->isConnected()) {
            // For full scripts, use appropriate execution method
            if (m_currentFile.endsWith(".tcl")) {
                // Execute entire script as Tcl
                // We can send it line by line or as a block
                QStringList lines = code.split('\n');
                for (const QString &line : lines) {
                    if (!line.trimmed().isEmpty()) {
                        cmdInterface->executeCommand("/tcl " + line);
                    }
                }
            } else {
                cmdInterface->executeEssAsync(code);
            }
            
            EssConsoleManager::instance()->logInfo(
                "Executing entire script...", 
                "ScriptEditor"
            );
        }
    }
}

void EssScriptEditorWidget::setCurrentFile(const QString &path)
{
    m_currentFile = path;
    m_editor->setModified(false);
    
    QString shownName = m_currentFile;
    if (m_currentFile.isEmpty())
        shownName = "untitled.tcl";
    
    setWindowTitle(tr("%1[*] - Script Editor").arg(QFileInfo(shownName).fileName()));
    
    if (!path.isEmpty()) {
        m_defaultPath = QFileInfo(path).path();
    }
}

void EssScriptEditorWidget::setLexerForFile(const QString &path)
{
    QFileInfo fi(path);
    QString ext = fi.suffix().toLower();
    
    if (ext == "py") {
        m_lexer.reset(new QsciLexerPython(m_editor));
    } else if (ext == "cpp" || ext == "cxx" || ext == "h" || ext == "hpp") {
        m_lexer.reset(new QsciLexerCPP(m_editor));
    } else if (ext == "sh" || ext == "bash") {
        m_lexer.reset(new QsciLexerBash(m_editor));
    } else {
        // Default to Tcl
        m_lexer.reset(new QsciLexerTCL(m_editor));
    }
    
    m_editor->setLexer(m_lexer.get());
    applyTheme();  // Reapply theme to new lexer
}

bool EssScriptEditorWidget::maybeSave()
{
    if (!m_editor->isModified())
        return true;
    
    const QMessageBox::StandardButton ret = 
        QMessageBox::warning(this, tr("Script Editor"),
                             tr("The document has been modified.\n"
                                "Do you want to save your changes?"),
                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    
    switch (ret) {
    case QMessageBox::Save:
        return saveFile();
    case QMessageBox::Cancel:
        return false;
    default:
        break;
    }
    return true;
}

bool EssScriptEditorWidget::isModified() const
{
    return m_editor->isModified();
}

void EssScriptEditorWidget::onTextChanged()
{
    // Update status or other UI elements if needed
}

void EssScriptEditorWidget::onCursorPositionChanged(int line, int index)
{
    m_statusLabel->setText(tr("Line %1, Column %2").arg(line + 1).arg(index + 1));
}

void EssScriptEditorWidget::onModificationChanged(bool modified)
{
    emit modificationChanged(modified);
}

void EssScriptEditorWidget::closeEvent(QCloseEvent *event)
{
    if (maybeSave()) {
        event->accept();
    } else {
        event->ignore();
    }
}

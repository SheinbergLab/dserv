#include "EssCodeEditor.h"

#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexertcl.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexerjavascript.h>
#include <Qsci/qscilexerbash.h>

#include <QVBoxLayout>
#include <QToolBar>
#include <QAction>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileInfo>
#include <QLabel>

EssCodeEditor::EssCodeEditor(QWidget *parent)
    : QWidget(parent)
    , m_editor(new QsciScintilla(this))
    , m_toolbar(new QToolBar(this))
    , m_statusLabel(nullptr)
    , m_language(Tcl)
    , m_showToolbar(true)
{
    setupUi();
    setupEditor();
    createActions();
    installEventFilter(this);
    setAcceptDrops(true);
}

EssCodeEditor::~EssCodeEditor() = default;

void EssCodeEditor::setupUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    
    m_toolbar->setMovable(false);
    m_toolbar->setIconSize(QSize(16, 16));
    layout->addWidget(m_toolbar);
    layout->addWidget(m_editor);
    
    // Allow derived classes to customize toolbar
    setupCustomActions(m_toolbar);
}

void EssCodeEditor::setupEditor()
{
    // Basic settings
    m_editor->setUtf8(true);
    m_editor->setEolMode(QsciScintilla::EolUnix);
    m_editor->setIndentationsUseTabs(false);
    m_editor->setIndentationWidth(4);
    m_editor->setAutoIndent(true);
    
    // Line numbers
    m_editor->setMarginType(MARGIN_LINE_NUMBERS, QsciScintilla::NumberMargin);
    m_editor->setMarginLineNumbers(MARGIN_LINE_NUMBERS, true);
    updateLineNumberMarginWidth();
    
    // Folding
    m_editor->setMarginType(MARGIN_FOLDING, QsciScintilla::SymbolMargin);
    m_editor->setFolding(QsciScintilla::BoxedTreeFoldStyle);
    
    // Bookmark margin
    m_editor->setMarginType(MARGIN_BOOKMARKS, QsciScintilla::SymbolMargin);
    m_editor->setMarginWidth(MARGIN_BOOKMARKS, 20);
    m_editor->setMarginSensitivity(MARGIN_BOOKMARKS, true);
    
    // Visual settings
    m_editor->setCaretLineVisible(true);
    m_editor->setBraceMatching(QsciScintilla::SloppyBraceMatch);
    
    // Auto-completion
    m_editor->setAutoCompletionSource(QsciScintilla::AcsAll);
    m_editor->setAutoCompletionThreshold(3);
    m_editor->setAutoCompletionCaseSensitivity(false);
    m_editor->setAutoCompletionReplaceWord(true);
    
    // Set default lexer
    setLexerForLanguage(m_language);
    
    // Apply theme
    applyTheme();
    
    // Define bookmark marker
    m_editor->markerDefine(QsciScintilla::Circle, BOOKMARK_MARKER);
    m_editor->setMarkerBackgroundColor(QColor(255, 195, 0), BOOKMARK_MARKER);
    
    // Connect signals
    connect(m_editor, &QsciScintilla::textChanged,
            this, &EssCodeEditor::onTextChanged);
    connect(m_editor, &QsciScintilla::cursorPositionChanged,
            this, &EssCodeEditor::onCursorPositionChanged);
    connect(m_editor, &QsciScintilla::marginClicked,
            this, &EssCodeEditor::onMarginClicked);
    connect(m_editor, &QsciScintilla::textChanged,
            this, &EssCodeEditor::updateLineNumberMarginWidth);
    
    connect(m_editor, &QsciScintilla::modificationChanged,
            this, [this](bool modified) {
                m_saveAction->setEnabled(modified);
                emit modificationChanged(modified);
            });
    
    // Still connect textChanged for content updates
    connect(m_editor, &QsciScintilla::textChanged,
            this, [this]() {
                emit contentChanged(m_editor->text());
                onContentChanged();
            });
    
    // Install event filter on editor too
    m_editor->installEventFilter(this);
}

void EssCodeEditor::applyTheme()
{
  // Create a proper fixed-width font
    QFont fixedFont;
    
    // Try specific fonts in order of preference
    QStringList fontFamilies;
    #ifdef Q_OS_MAC
        fontFamilies << "SF Mono" << "Monaco" << "Menlo" << "Courier New";
    #elif defined(Q_OS_WIN)
        fontFamilies << "Consolas" << "Courier New" << "Lucida Console";
    #else
        fontFamilies << "DejaVu Sans Mono" << "Ubuntu Mono" << "Courier New";
    #endif
    
    // Find the first available font
    bool fontFound = false;
    for (const QString &family : fontFamilies) {
        fixedFont.setFamily(family);
        if (QFontInfo(fixedFont).family().contains(family, Qt::CaseInsensitive)) {
            fontFound = true;
            break;
        }
    }
    
    // Fallback to generic monospace
    if (!fontFound) {
        fixedFont.setFamily("monospace");
        fixedFont.setStyleHint(QFont::Monospace);
    }
    
    fixedFont.setFixedPitch(true);
    fixedFont.setPointSize(12);

    
    // Base colors (dark theme)
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
    
    // Apply theme to current lexer
    if (m_lexer) {

      for (int i = 0; i < 128; ++i) {
	m_lexer->setPaper(QColor(40, 44, 52), i);
	m_lexer->setFont(fixedFont, i);
      }
      
        // Language-specific colors
        if (auto tcl = qobject_cast<QsciLexerTCL*>(m_lexer.get())) {
            tcl->setColor(QColor(171, 178, 191), QsciLexerTCL::Default);
            tcl->setColor(QColor(224, 108, 117), QsciLexerTCL::Comment);
            tcl->setColor(QColor(224, 108, 117), QsciLexerTCL::CommentLine);
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
        else if (auto python = qobject_cast<QsciLexerPython*>(m_lexer.get())) {
            python->setColor(QColor(171, 178, 191), QsciLexerPython::Default);
            python->setColor(QColor(224, 108, 117), QsciLexerPython::Comment);
            python->setColor(QColor(152, 195, 121), QsciLexerPython::SingleQuotedString);
            python->setColor(QColor(152, 195, 121), QsciLexerPython::DoubleQuotedString);
            python->setColor(QColor(229, 192, 123), QsciLexerPython::Number);
            python->setColor(QColor(198, 120, 221), QsciLexerPython::Keyword);
            python->setColor(QColor(86, 182, 255), QsciLexerPython::ClassName);
            python->setColor(QColor(97, 175, 239), QsciLexerPython::FunctionMethodName);
            python->setColor(QColor(224, 108, 117), QsciLexerPython::Operator);
            python->setColor(QColor(171, 178, 191), QsciLexerPython::Identifier);
        }
        // Add more language-specific themes as needed
    }

  // Also set for the editor itself
    m_editor->setFont(fixedFont);
    
    // Force monospaced font metrics
    m_editor->SendScintilla(QsciScintillaBase::SCI_STYLESETFONT, 
                           QsciScintillaBase::STYLE_DEFAULT, 
                           fixedFont.family().toUtf8().constData());    
}

void EssCodeEditor::createActions()
{
    m_saveAction = new QAction(tr("Save"), this);
    m_saveAction->setShortcut(QKeySequence::Save);
    m_saveAction->setIcon(QIcon::fromTheme("document-save"));
    m_saveAction->setEnabled(false);  // Disabled until content is modified
    connect(m_saveAction, &QAction::triggered, this, &EssCodeEditor::saveRequested);
    
    m_findAction = new QAction(tr("Find"), this);
    m_findAction->setShortcut(QKeySequence::Find);
    m_findAction->setIcon(QIcon::fromTheme("edit-find"));
    connect(m_findAction, &QAction::triggered, this, &EssCodeEditor::showFindDialog);
    
    m_toggleBookmarkAction = new QAction(tr("Toggle Bookmark"), this);
    m_toggleBookmarkAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F2));
    m_toggleBookmarkAction->setIcon(QIcon::fromTheme("bookmark-new"));
    connect(m_toggleBookmarkAction, &QAction::triggered, this, [this]() { toggleBookmark(); });
    
    m_nextBookmarkAction = new QAction(tr("Next Bookmark"), this);
    m_nextBookmarkAction->setShortcut(QKeySequence(Qt::Key_F2));
    connect(m_nextBookmarkAction, &QAction::triggered, this, &EssCodeEditor::nextBookmark);
    
    m_prevBookmarkAction = new QAction(tr("Previous Bookmark"), this);
    m_prevBookmarkAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F2));
    connect(m_prevBookmarkAction, &QAction::triggered, this, &EssCodeEditor::previousBookmark);
    
    // Add common actions to toolbar
    m_toolbar->addAction(m_saveAction);
    m_toolbar->addSeparator();
    m_toolbar->addAction(m_findAction);
    m_toolbar->addSeparator();
    m_toolbar->addAction(m_toggleBookmarkAction);
}

void EssCodeEditor::setContent(const QString &content)
{
    m_editor->blockSignals(true);
    m_editor->setText(content);
    m_editor->blockSignals(false);
    
    m_originalContent = content;
    
    // Use QScintilla's modification tracking
    m_editor->setModified(false);
    m_saveAction->setEnabled(false);
    
    // Ensure UI is updated
    emit modificationChanged(false);
}

QString EssCodeEditor::content() const
{
    return m_editor->text();
}

void EssCodeEditor::clear()
{
    m_editor->clear();
    m_originalContent.clear();
    updateModificationState();
}

bool EssCodeEditor::isModified() const
{
    return m_editor->isModified();
}

void EssCodeEditor::setModified(bool modified)
{
    m_editor->setModified(modified);
    if (!modified) {
        m_originalContent = m_editor->text();
    }
    m_saveAction->setEnabled(modified);
    emit modificationChanged(modified);
}

void EssCodeEditor::setLanguage(Language lang)
{
    if (m_language != lang) {
        m_language = lang;
        setLexerForLanguage(lang);
        emit languageChanged(lang);
        onLanguageChanged(lang);
    }
}

void EssCodeEditor::setLexerForLanguage(Language lang)
{
    m_lexer.reset();
    
    switch (lang) {
        case Tcl:
            m_lexer.reset(new QsciLexerTCL(m_editor));
            break;
        case Python:
            m_lexer.reset(new QsciLexerPython(m_editor));
            break;
        case Cpp:
            m_lexer.reset(new QsciLexerCPP(m_editor));
            break;
        case JavaScript:
            m_lexer.reset(new QsciLexerJavaScript(m_editor));
            break;
        case Bash:
            m_lexer.reset(new QsciLexerBash(m_editor));
            break;
        case PlainText:
        default:
            // No lexer for plain text
            break;
    }
    
    if (m_lexer) {
        m_editor->setLexer(m_lexer.get());
        applyTheme();  // Reapply theme to new lexer
    } else {
        m_editor->setLexer(nullptr);
    }
}

void EssCodeEditor::onTextChanged()
{
    updateModificationState();
    emit contentChanged(m_editor->text());
    onContentChanged(); // Virtual hook for derived classes
}

void EssCodeEditor::updateModificationState()
{
    bool wasModified = m_editor->isModified();
    bool isNowModified = (m_editor->text() != m_originalContent);

    if (wasModified != isNowModified) {
        m_editor->setModified(isNowModified);
        m_saveAction->setEnabled(isNowModified);
        emit modificationChanged(isNowModified);
    }    
}

bool EssCodeEditor::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        
        // Let derived classes handle first
        if (handleCustomKeyEvent(keyEvent)) {
            return true;
        }
    // Handle Enter/Return key for auto-indent
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
         // Only intercept if no modifiers (plain Enter)
            if (keyEvent->modifiers() == Qt::NoModifier) {
                int line, index;
                m_editor->getCursorPosition(&line, &index);
                
                // Let QScintilla handle the actual newline insertion
                m_editor->SendScintilla(QsciScintillaBase::SCI_NEWLINE);
                
                // Now apply auto-indent to the new line
                int newLine = line + 1;
                if (newLine < m_editor->lines()) {
                    // Calculate and apply indent
                    QString indent = getIndentForLine(newLine);
                    if (!indent.isEmpty()) {
                        m_editor->setIndentation(newLine, indent.length());
                        m_editor->setCursorPosition(newLine, indent.length());
                    }
                }
                
                return true;  // We handled it
            }
	}

        
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
                    
                case Qt::Key_Space: // Auto-completion
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

QString EssCodeEditor::getIndentForLine(int lineNumber) const
{
    // This will be called for the NEW line (after Enter)
    // So we need to look at the previous line
    int prevLine = lineNumber - 1;
    
    if (prevLine < 0) return "";
    
    // Get the previous line's text
    QString prevLineText = m_editor->text(prevLine);
    
    // Calculate base indentation (copy from previous line)
    int baseIndent = 0;
    for (QChar ch : prevLineText) {
        if (ch == ' ') baseIndent++;
        else if (ch == '\t') baseIndent += m_editor->indentationWidth();
        else break;
    }
    
    // Check if we should increase indent
    QString trimmed = prevLineText.trimmed();
    if (shouldIncreaseIndent(trimmed)) {
        baseIndent += m_editor->indentationWidth();
    }
    
    // Check if the new line already has a closing brace
    // (in case we're pressing enter in the middle of a line)
    if (lineNumber < m_editor->lines()) {
        QString nextLineText = m_editor->text(lineNumber);
        QString nextTrimmed = nextLineText.trimmed();
        if (shouldDecreaseIndent(nextTrimmed)) {
            // Don't indent as much if the next line has a closing brace
            baseIndent -= m_editor->indentationWidth();
            if (baseIndent < 0) baseIndent = 0;
        }
    }
    
    return QString(baseIndent, ' ');
}

void EssCodeEditor::smartIndent()
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
        
        // Check if previous line should increase indent
        QString trimmed = prevLineText.trimmed();
        if (shouldIncreaseIndent(trimmed)) {
            prevIndent += m_editor->indentationWidth();
        }
        
        // Check current line for closing brace
        QString currentLineText = m_editor->text(line);
        QString currentTrimmed = currentLineText.trimmed();
        if (shouldDecreaseIndent(currentTrimmed)) {
            prevIndent -= m_editor->indentationWidth();
            if (prevIndent < 0) prevIndent = 0;
        }
        
        // Apply indentation
        m_editor->setIndentation(line, prevIndent);
        
        // Move cursor to end of indentation
        m_editor->setCursorPosition(line, prevIndent);
    }
}

bool EssCodeEditor::shouldIncreaseIndent(const QString &line) const
{
    // Language-specific indent rules
    switch (m_language) {
        case Tcl:
            return line.endsWith('{') || 
                   line.startsWith("proc ") || 
                   line.startsWith("if ") || 
                   line.startsWith("while ") ||
                   line.startsWith("for ") || 
                   line.startsWith("foreach ");
        case Python:
            return line.endsWith(':');
        case Cpp:
        case JavaScript:
            return line.endsWith('{');
        default:
            return false;
    }
}

bool EssCodeEditor::shouldDecreaseIndent(const QString &line) const
{
    switch (m_language) {
        case Tcl:
            return line.startsWith('}');
        case Python:
            return line.startsWith("return") || 
                   line.startsWith("break") || 
                   line.startsWith("continue") ||
                   line.startsWith("pass");
        case Cpp:
        case JavaScript:
            return line.startsWith('}');
        default:
            return false;
    }
}

void EssCodeEditor::onCursorPositionChanged(int line, int column)
{
    emit cursorPositionChanged(line, column);
}

void EssCodeEditor::onMarginClicked(int margin, int line, Qt::KeyboardModifiers)
{
    if (margin == MARGIN_BOOKMARKS) {
        toggleBookmark(line);
    }
}

void EssCodeEditor::updateLineNumberMarginWidth()
{
    int lines = m_editor->lines();
    int digits = 1;
    while (lines >= 10) {
        lines /= 10;
        ++digits;
    }
    m_editor->setMarginWidth(MARGIN_LINE_NUMBERS, QString(digits + 1, '0'));
}

// Bookmark management
void EssCodeEditor::toggleBookmark()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    toggleBookmark(line);
}

void EssCodeEditor::toggleBookmark(int line)
{
    if (m_editor->markersAtLine(line) & (1 << BOOKMARK_MARKER)) {
        m_editor->markerDelete(line, BOOKMARK_MARKER);
    } else {
        m_editor->markerAdd(line, BOOKMARK_MARKER);
    }
}

void EssCodeEditor::nextBookmark()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    
    int nextLine = m_editor->markerFindNext(line + 1, (1 << BOOKMARK_MARKER));
    if (nextLine == -1) {
        // Wrap around to beginning
        nextLine = m_editor->markerFindNext(0, (1 << BOOKMARK_MARKER));
    }
    
    if (nextLine != -1) {
        m_editor->setCursorPosition(nextLine, 0);
    }
}

void EssCodeEditor::previousBookmark()
{
    int line, index;
    m_editor->getCursorPosition(&line, &index);
    
    int prevLine = m_editor->markerFindPrevious(line - 1, (1 << BOOKMARK_MARKER));
    if (prevLine == -1) {
        // Wrap around to end
        prevLine = m_editor->markerFindPrevious(m_editor->lines() - 1, (1 << BOOKMARK_MARKER));
    }
    
    if (prevLine != -1) {
        m_editor->setCursorPosition(prevLine, 0);
    }
}

QList<int> EssCodeEditor::bookmarkedLines() const
{
    QList<int> bookmarks;
    int line = 0;
    while ((line = m_editor->markerFindNext(line, (1 << BOOKMARK_MARKER))) != -1) {
        bookmarks.append(line);
        line++;
    }
    return bookmarks;
}

// Additional features
void EssCodeEditor::setReadOnly(bool readOnly)
{
    m_editor->setReadOnly(readOnly);
    m_saveAction->setEnabled(!readOnly && isModified());
}

bool EssCodeEditor::isReadOnly() const
{
    return m_editor->isReadOnly();
}

void EssCodeEditor::showFindDialog()
{
    m_editor->findFirst(QString(), false, false, false, true);
}

void EssCodeEditor::setToolbarVisible(bool visible)
{
    m_showToolbar = visible;
    m_toolbar->setVisible(visible);
}

// Drag and drop support
void EssCodeEditor::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void EssCodeEditor::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    
    if (mimeData->hasUrls()) {
        QList<QUrl> urls = mimeData->urls();
        if (!urls.isEmpty()) {
            QString path = urls.first().toLocalFile();
            if (!path.isEmpty()) {
                emit fileDropped(path);
            }
        }
    }
}

// Additional utility methods
void EssCodeEditor::addToolbarAction(QAction *action)
{
    m_toolbar->addAction(action);
}

void EssCodeEditor::addToolbarSeparator()
{
    m_toolbar->addSeparator();
}

void EssCodeEditor::addToolbarWidget(QWidget *widget)
{
    m_toolbar->addWidget(widget);
}

QString EssCodeEditor::selectedText() const
{
    return m_editor->selectedText();
}

bool EssCodeEditor::hasSelectedText() const
{
    return m_editor->hasSelectedText();
}

void EssCodeEditor::selectAll()
{
    m_editor->selectAll();
}

void EssCodeEditor::getCursorPosition(int &line, int &column) const
{
    m_editor->getCursorPosition(&line, &column);
}

void EssCodeEditor::setCursorPosition(int line, int column)
{
    m_editor->setCursorPosition(line, column);
}

void EssCodeEditor::gotoLine(int line)
{
    m_editor->setCursorPosition(line - 1, 0);  // Convert to 0-based
    m_editor->ensureLineVisible(line - 1);
}

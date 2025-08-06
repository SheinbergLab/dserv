#include "EssCodeEditor.h"
#include "tcl/TclUtils.h"

#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexertcl.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexerjavascript.h>
#include <Qsci/qscilexerbash.h>

#include <QStack>
#include <QVBoxLayout>
#include <QToolBar>
#include <QAction>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QContextMenuEvent>

EssCodeEditor::EssCodeEditor(QWidget *parent)
    : QWidget(parent)
    , m_editor(new QsciScintilla(this))
    , m_toolbar(new QToolBar(this))
    , m_statusLabel(nullptr)
    , m_language(Tcl)
    , m_showToolbar(true)
    , m_formatAction(nullptr)           
    , m_formatSelectionAction(nullptr)  
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
        fontFamilies << "Monaco" << "Menlo" << "Courier New";
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
    
    // Format actions
m_formatAction = new QAction(tr("Format Code"), this);
    m_formatAction->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_F));
    QIcon formatIcon = QIcon::fromTheme("format-indent-more");
    if (!formatIcon.isNull()) {
        m_formatAction->setIcon(formatIcon);
    }
    connect(m_formatAction, &QAction::triggered, this, &EssCodeEditor::formatCode);
    
    m_formatSelectionAction = new QAction(tr("Format Selection"), this);
    m_formatSelectionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    connect(m_formatSelectionAction, &QAction::triggered, this, &EssCodeEditor::formatSelection);
    
    // Only add to toolbar if toolbar exists
    if (m_toolbar) {
        m_toolbar->addSeparator();
        m_toolbar->addAction(m_formatAction);
    }
    
    // Update format action availability based on language
    bool canFormat = (m_language == Tcl);
    m_formatAction->setEnabled(canFormat);
    m_formatSelectionAction->setEnabled(canFormat);
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

// Main formatting functions:
void EssCodeEditor::setLanguage(Language lang)
{
    if (m_language != lang) {
        m_language = lang;
        setLexerForLanguage(lang);
        
        // Only update format actions if they exist
        if (m_formatAction && m_formatSelectionAction) {
            bool canFormat = (lang == Tcl);
            m_formatAction->setEnabled(canFormat);
            m_formatSelectionAction->setEnabled(canFormat);
        }
        
        emit languageChanged(lang);
        onLanguageChanged(lang);
    }
}

QString EssCodeEditor::formatTclCodeRobust(const QString &code, int baseIndent)
{
    QStringList lines = code.split('\n', Qt::KeepEmptyParts);
    QStringList formatted;
    
    int indentWidth = m_editor ? m_editor->indentationWidth() : 4;
    
    // Enhanced state tracking
    struct FormatState {
        int braceLevel = 0;                    // Current brace-based indentation level
        bool inContinuation = false;           // Are we in a line continuation?
        int baseLineIndent = 0;                // Base indent of the line that started continuation
        bool inParameterList = false;          // Are we in a proc/method parameter list?
        
        // Track alignment points for brackets and parentheses
        struct AlignmentPoint {
            int column;      // Column position in the formatted line
            QChar type;      // '[', '(', or '{'
            int lineNumber;  // Which line this bracket appeared on
        };
        QStack<AlignmentPoint> alignmentStack;
        
        // Get the best alignment column for continuation
        int getAlignmentColumn(int defaultIndent) const {
            if (!alignmentStack.isEmpty()) {
                // Align 2 spaces after the most recent unclosed bracket/paren
                return alignmentStack.top().column + 2;
            }
            // No brackets to align with
            if (inParameterList) {
                // Double indent for parameter lists
                return defaultIndent + 8; // Two indent levels
            }
            // Standard continuation indent
            return defaultIndent + 4; // One indent level
        }
        
        // Reset continuation state
        void endContinuation() {
            inContinuation = false;
            inParameterList = false;
            alignmentStack.clear();
            baseLineIndent = 0;
        }
        
        // Update alignment points based on a formatted line
        void updateAlignmentPoints(const QString &formattedLine, int lineNumber) {
            bool inQuotes = false;
            bool escaped = false;
            bool inBraces = false;
            int braceDepth = 0;
            
            for (int i = 0; i < formattedLine.length(); ++i) {
                QChar c = formattedLine[i];
                
                if (escaped) {
                    escaped = false;
                    continue;
                }
                
                if (c == '\\') {
                    escaped = true;
                    continue;
                }
                
                if (c == '"' && !inBraces) {
                    inQuotes = !inQuotes;
                    continue;
                }
                
                if (!inQuotes) {
                    if (c == '{') {
                        braceDepth++;
                        inBraces = (braceDepth > 0);
                    } else if (c == '}') {
                        braceDepth--;
                        inBraces = (braceDepth > 0);
                    } else if (!inBraces) {
                        if (c == '[' || c == '(') {
                            alignmentStack.push({i, c, lineNumber});
                        } else if (c == ']' && !alignmentStack.isEmpty() && 
                                   alignmentStack.top().type == '[') {
                            alignmentStack.pop();
                        } else if (c == ')' && !alignmentStack.isEmpty() && 
                                   alignmentStack.top().type == '(') {
                            alignmentStack.pop();
                        }
                    }
                }
            }
        }
    };
    
    FormatState state;
    
    for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
        QString line = lines[lineIdx];
        QString trimmed = line.trimmed();
        
        // Empty lines - preserve them
        if (trimmed.isEmpty()) {
            formatted.append("");
            state.endContinuation();
            continue;
        }
        
        // Comments - preserve at appropriate indent
        if (trimmed.startsWith('#')) {
            int commentIndent;
            if (state.inContinuation) {
                commentIndent = state.getAlignmentColumn(state.baseLineIndent);
            } else {
                commentIndent = baseIndent + state.braceLevel * indentWidth;
            }
            
            QString formattedLine = QString(commentIndent, ' ') + trimmed;
            formatted.append(formattedLine);
            
            if (endsWithContinuation(trimmed)) {
                if (!state.inContinuation) {
                    state.inContinuation = true;
                    state.baseLineIndent = commentIndent;
                    state.updateAlignmentPoints(formattedLine, lineIdx);
                }
            } else {
                state.endContinuation();
            }
            continue;
        }
        
        // Analyze the line
        LineAnalysis analysis = analyzeTclLine(trimmed);
        
        // Calculate base indent for this line
        int lineIndent = baseIndent + state.braceLevel * indentWidth;
        
        // Handle continuation lines
        if (state.inContinuation) {
            lineIndent = state.getAlignmentColumn(state.baseLineIndent);
        }
        
        // Special adjustments for current line (only if not in continuation)
        if (!state.inContinuation) {
            // Reduce indent for lines starting with closing braces
            if (analysis.startsWithCloseBrace) {
                lineIndent = qMax(baseIndent, 
                    baseIndent + (state.braceLevel - analysis.leadingCloseBraces) * indentWidth);
            }
            
            // Handle special keywords that reduce indent
            if (startsWithSpecialKeyword(trimmed)) {
                lineIndent = qMax(baseIndent, 
                    baseIndent + (state.braceLevel - 1) * indentWidth);
            }
        }
        
        // Format the line
        QString formattedLine = QString(lineIndent, ' ') + trimmed;
        formatted.append(formattedLine);
        
        // Update state based on this line
        if (!state.inContinuation && analysis.hasContinuation) {
            // Starting a new continuation
            state.inContinuation = true;
            state.baseLineIndent = lineIndent;
            state.alignmentStack.clear();
            
            // Check if this looks like a proc/method parameter list
            if (looksLikeProcParameterList(trimmed)) {
                state.inParameterList = true;
            }
            
            state.updateAlignmentPoints(formattedLine, lineIdx);
            
        } else if (state.inContinuation && analysis.hasContinuation) {
            // Continuing a continuation - update alignment points
            state.updateAlignmentPoints(formattedLine, lineIdx);
            
        } else if (state.inContinuation && !analysis.hasContinuation) {
            // Ending continuation
            state.endContinuation();
        }
        
        // Always update brace level (affects next line's base indent)
        state.braceLevel += (analysis.openBraces - analysis.closeBraces);
        state.braceLevel = qMax(0, state.braceLevel);
    }
    
    return formatted.join('\n');
}

// Check if a line looks like it's starting a proc/method parameter list
bool EssCodeEditor::looksLikeProcParameterList(const QString &line)
{
    QString trimmed = line.trimmed();
    
    // Look for patterns that typically have parameter lists:
    // proc name { params...
    // method name { params...
    // $obj add_method name { params...
    // constructor { params...
    // destructor { params...
    
    // Check if line contains "proc", "method", "constructor", or "destructor"
    // followed by a name and then an opening brace
    static const QRegularExpression procPattern(
        "(?:proc|method|constructor|destructor)\\s+\\S+\\s*\\{|"  // proc/method name {
        "add_method\\s+\\S+\\s*\\{"                                // add_method name {
    );
    
    if (procPattern.match(trimmed).hasMatch()) {
        // Additional check: the line should end with \ for continuation
        // and likely has parameter-like content after the {
        int bracePos = trimmed.indexOf('{');
        if (bracePos >= 0 && bracePos < trimmed.length() - 1) {
            QString afterBrace = trimmed.mid(bracePos + 1).trimmed();
            // If there's content after the brace that looks like parameters
            // (variables, identifiers, not commands)
            if (!afterBrace.isEmpty() && !afterBrace.startsWith('[')) {
                return true;
            }
        }
    }
    
    return false;
}


// Helper structure for line analysis
struct LineAnalysis {
    int openBraces = 0;
    int closeBraces = 0;
    int openBrackets = 0;
    int closeBrackets = 0;
    bool hasContinuation = false;
    bool startsWithCloseBrace = false;
    int leadingCloseBraces = 0;
};

// Analyze a single line of Tcl code
EssCodeEditor::LineAnalysis EssCodeEditor::analyzeTclLine(const QString &line)
{
    LineAnalysis result;
    
    if (line.isEmpty()) return result;
    
    // Check for leading close braces
    result.startsWithCloseBrace = line.startsWith('}');
    if (result.startsWithCloseBrace) {
        for (int i = 0; i < line.length() && line[i] == '}'; i++) {
            result.leadingCloseBraces++;
        }
    }
    
    // Check for line continuation
    result.hasContinuation = endsWithContinuation(line);
    
    // Count braces and brackets
    bool inQuotes = false;
    bool inBraces = false;
    int braceDepth = 0;
    bool escaped = false;
    
    for (int i = 0; i < line.length(); ++i) {
        QChar c = line[i];
        
        if (escaped) {
            escaped = false;
            continue;
        }
        
        if (c == '\\') {
            escaped = true;
            continue;
        }
        
        if (c == '"' && !inBraces) {
            inQuotes = !inQuotes;
            continue;
        }
        
        if (!inQuotes) {
            if (c == '{') {
                braceDepth++;
                inBraces = (braceDepth > 0);
                result.openBraces++;
            } else if (c == '}') {
                braceDepth--;
                inBraces = (braceDepth > 0);
                result.closeBraces++;
            } else if (c == '[' && !inBraces) {
                result.openBrackets++;
            } else if (c == ']' && !inBraces) {
                result.closeBrackets++;
            }
        }
    }
    
    return result;
}

// Check if a line ends with a continuation backslash
bool EssCodeEditor::endsWithContinuation(const QString &line)
{
    if (line.isEmpty()) return false;
    
    // Count trailing backslashes
    int backslashCount = 0;
    for (int i = line.length() - 1; i >= 0 && line[i] == '\\'; i--) {
        backslashCount++;
    }
    
    // Odd number of backslashes means continuation
    return (backslashCount % 2) == 1;
}

// Check if line starts with special keywords that affect indentation
bool EssCodeEditor::startsWithSpecialKeyword(const QString &line)
{
    static const QStringList keywords = {
        "else", "elseif", "catch", "finally", "then"
    };
    
    for (const QString &keyword : keywords) {
        if (line.startsWith(keyword + " ") || line.startsWith(keyword + "\t") || 
            line == keyword || line.startsWith(keyword + "{")) {
            return true;
        }
    }
    
    return false;
}

// Alternative: Use parser-based formatting when available
QString EssCodeEditor::formatTclCodeWithParser(const QString &code, int baseIndent)
{
    // For now, just use the robust formatter
    // The parser-based approach has proven problematic with complex Tcl structures
    return formatTclCodeRobust(code, baseIndent);
}

void EssCodeEditor::formatCode()
{
    if (!m_editor || m_language != Tcl) return;
    
    // Save detailed cursor context
    int line, column;
    m_editor->getCursorPosition(&line, &column);
    
    // Get the current line text before formatting
    QString currentLineText;
    if (line < m_editor->lines()) {
        currentLineText = m_editor->text(line);
    }
    
    // Find position context - what token/word is the cursor in?
    QString beforeCursor = currentLineText.left(column);
    QString afterCursor = currentLineText.mid(column);
    
    // Find the word/token at cursor position
    int wordStart = column;
    int wordEnd = column;
    
    // Find start of current word
    for (int i = column - 1; i >= 0; i--) {
        QChar ch = currentLineText[i];
        if (ch.isLetterOrNumber() || ch == '_' || ch == ':' || ch == '$') {
            wordStart = i;
        } else {
            break;
        }
    }
    
    // Find end of current word
    for (int i = column; i < currentLineText.length(); i++) {
        QChar ch = currentLineText[i];
        if (ch.isLetterOrNumber() || ch == '_' || ch == ':' || ch == '$') {
            wordEnd = i + 1;
        } else {
            break;
        }
    }
    
    // Extract the word at cursor and offset within it
    QString wordAtCursor;
    int offsetInWord = 0;
    if (wordEnd > wordStart) {
        wordAtCursor = currentLineText.mid(wordStart, wordEnd - wordStart);
        offsetInWord = column - wordStart;
    }
    
    // Get the entire document content
    QString code = m_editor->text();
    if (code.isEmpty()) return;
    
    // Format using the enhanced formatter
    QString formatted = formatTclCodeRobust(code, 0);
    
    // Only update if formatting actually changed something
    if (formatted != code && !formatted.isEmpty()) {
        // Begin undo action for the entire format operation
        m_editor->beginUndoAction();
        
        // Replace all text
        m_editor->setText(formatted);
        
        // End undo action
        m_editor->endUndoAction();
        
        // Restore cursor position intelligently
        if (line < m_editor->lines()) {
            QString newLineText = m_editor->text(line);
            int newColumn = column; // Default to same column
            
            // Strategy 1: Try to find the same word
            if (!wordAtCursor.isEmpty()) {
                int wordPos = newLineText.indexOf(wordAtCursor);
                if (wordPos >= 0) {
                    // Found the word, restore position within it
                    newColumn = wordPos + qMin(offsetInWord, wordAtCursor.length());
                } else {
                    // Word not found on this line, try adjacent lines
                    bool found = false;
                    
                    // Check line above
                    if (line > 0) {
                        QString prevLine = m_editor->text(line - 1);
                        wordPos = prevLine.indexOf(wordAtCursor);
                        if (wordPos >= 0) {
                            m_editor->setCursorPosition(line - 1, wordPos + offsetInWord);
                            found = true;
                        }
                    }
                    
                    // Check line below
                    if (!found && line < m_editor->lines() - 1) {
                        QString nextLine = m_editor->text(line + 1);
                        wordPos = nextLine.indexOf(wordAtCursor);
                        if (wordPos >= 0) {
                            m_editor->setCursorPosition(line + 1, wordPos + offsetInWord);
                            found = true;
                        }
                    }
                    
                    if (found) {
                        m_editor->ensureCursorVisible();
                        return;
                    }
                }
            }
            
            // Strategy 2: If no word or word not found, try to maintain relative position
            if (wordAtCursor.isEmpty() || newColumn == column) {
                // Find first non-whitespace in old and new lines
                int oldFirstNonSpace = 0;
                int newFirstNonSpace = 0;
                
                for (int i = 0; i < currentLineText.length(); i++) {
                    if (!currentLineText[i].isSpace()) {
                        oldFirstNonSpace = i;
                        break;
                    }
                }
                
                for (int i = 0; i < newLineText.length(); i++) {
                    if (!newLineText[i].isSpace()) {
                        newFirstNonSpace = i;
                        break;
                    }
                }
                
                // Calculate relative position from first non-space
                if (column >= oldFirstNonSpace) {
                    int relativePos = column - oldFirstNonSpace;
                    newColumn = newFirstNonSpace + relativePos;
                    newColumn = qMin(newColumn, newLineText.length());
                } else {
                    // Cursor was in leading whitespace
                    newColumn = qMin(column, newFirstNonSpace);
                }
            }
            
            m_editor->setCursorPosition(line, newColumn);
        } else {
            // Line doesn't exist anymore, go to last line
            int lastLine = m_editor->lines() - 1;
            if (lastLine >= 0) {
                m_editor->setCursorPosition(lastLine, m_editor->text(lastLine).length());
            }
        }
        
        // Ensure cursor is visible
        m_editor->ensureCursorVisible();
    }
}



void EssCodeEditor::formatSelection()
{
    if (!m_editor || m_language != Tcl || !m_editor->hasSelectedText()) return;
    
    // Get selection boundaries
    int startLine, startCol, endLine, endCol;
    m_editor->getSelection(&startLine, &startCol, &endLine, &endCol);
    
    if (startLine < 0 || endLine >= m_editor->lines()) return;
    
    // Extend selection to complete lines
    startCol = 0;
    endCol = m_editor->text(endLine).length();
    
    // Get the selected text
    m_editor->setSelection(startLine, startCol, endLine, endCol);
    QString selected = m_editor->selectedText();
    if (selected.isEmpty()) return;
    
    // Calculate base indentation from the first line
    QString firstLine = m_editor->text(startLine);
    int baseIndent = 0;
    for (QChar ch : firstLine) {
        if (ch == ' ') baseIndent++;
        else if (ch == '\t') baseIndent += m_editor->indentationWidth();
        else break;
    }
    
    // Format the selection with the base indent
    QString formatted = formatTclCodeRobust(selected, baseIndent);
    
    // Only update if formatting actually changed something
    if (!formatted.isEmpty() && formatted != selected) {
        // Begin undo action
        m_editor->beginUndoAction();
        
        // Replace selected text
        m_editor->replaceSelectedText(formatted);
        
        // End undo action
        m_editor->endUndoAction();
        
        // Calculate new end position
        int newEndLine = startLine + formatted.count('\n');
        int newEndCol = 0;
        
        // If there are lines, get the length of the last line
        if (newEndLine < m_editor->lines()) {
            newEndCol = m_editor->text(newEndLine).length();
        }
        
        // Reselect the formatted text
        m_editor->setSelection(startLine, 0, newEndLine, newEndCol);
        
        // Ensure selection is visible
        m_editor->ensureCursorVisible();
    }
}


void EssCodeEditor::contextMenuEvent(QContextMenuEvent *event)
{
    if (!m_editor) {
        QWidget::contextMenuEvent(event);
        return;
    }
    
    QMenu *menu = m_editor->createStandardContextMenu();
    if (!menu) {
        QWidget::contextMenuEvent(event);
        return;
    }
    
    if (m_language == Tcl && m_formatAction) {
        menu->addSeparator();
        menu->addAction(m_formatAction);
        if (m_formatSelectionAction && m_editor->hasSelectedText()) {
            menu->addAction(m_formatSelectionAction);
        }
    }
    
    menu->exec(event->globalPos());
    delete menu;
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

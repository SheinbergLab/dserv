#pragma once

#include <QWidget>
#include <QString>
#include <QMenu>
#include <QContextMenuEvent>
#include <QRegularExpression>
#include <memory>

// Add these includes for the search bar
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>

class QsciScintilla;
class QsciLexer;
class QAction;
class QToolBar;
class TclUtils;

/**
 * @brief Generic code editor widget based on QScintilla
 * 
 * Provides a reusable code editor with syntax highlighting, Emacs key bindings,
 * dark theme, and common editing features. Designed to be subclassed for
 * specific use cases.
 */
class EssCodeEditor : public QWidget
{
    Q_OBJECT

public:
    explicit EssCodeEditor(QWidget *parent = nullptr);
    virtual ~EssCodeEditor();

    // Language support
    enum Language {
        Tcl,
        Python,
        Cpp,
        JavaScript,
        Bash,
        PlainText
    };

    // Content management
    void setContent(const QString &content);
    QString content() const;
    void clear();
    
    // State management
    bool isModified() const;
    void setModified(bool modified);
    
    // File operations
    QString currentFile() const { return m_currentFile; }
    void setCurrentFile(const QString &path);
    
    // Language configuration
    void setLanguage(Language lang);
    Language language() const { return m_language; }
    
    // Editor features
    void setReadOnly(bool readOnly);
    bool isReadOnly() const;
    void setShowLineNumbers(bool show);
    void setShowFolding(bool show);
    void setWordWrap(bool wrap);
    
    // Selection
    QString selectedText() const;
    bool hasSelectedText() const;
    void selectAll();
    
    // Cursor position
    void getCursorPosition(int &line, int &column) const;
    void setCursorPosition(int line, int column);
    void gotoLine(int line);
    
    // Bookmarks
    void toggleBookmark();
    void toggleBookmark(int line);
    void nextBookmark();
    void previousBookmark();
    void clearAllBookmarks();
    QList<int> bookmarkedLines() const;
    
    // Indentation
    void setIndentationWidth(int width);
    int indentationWidth() const;
    void setUseTabs(bool useTabs);
    bool useTabs() const;
  
    // Formatting
    void formatCode();
    void formatSelection();
  
    // Toolbar visibility
    void setToolbarVisible(bool visible);
    bool isToolbarVisible() const;
    
    // Search functionality
    void showFindDialog();

signals:
    void modificationChanged(bool modified);
    void cursorPositionChanged(int line, int column);
    void contentChanged(const QString &content);
    void saveRequested();  // Emitted on Ctrl+S
    void fileDropped(const QString &path);
    void languageChanged(Language language);

protected:
    // Virtual hooks for derived classes
    virtual void setupCustomActions(QToolBar *toolbar) {}
    virtual void onContentChanged() {}
    virtual bool handleCustomKeyEvent(QKeyEvent *event) { return false; }
    virtual void onLanguageChanged(Language /*lang*/) {}
    
    void contextMenuEvent(QContextMenuEvent *event) override;
     
    // Utilities for derived classes
    QsciScintilla* editor() const { return m_editor; }
    QToolBar* toolbar() const { return m_toolbar; }
    void addToolbarAction(QAction *action);
    void addToolbarSeparator();
    void addToolbarWidget(QWidget *widget);
    
    // Event handling
    bool eventFilter(QObject *obj, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onTextChanged();
    void onCursorPositionChanged(int line, int index);
    void onMarginClicked(int margin, int line, Qt::KeyboardModifiers state);
    void updateLineNumberMarginWidth();
    
    // Search bar slots
    void onSearchTextChanged(const QString &text);
    void findNext();
    void findPrevious();
    void updateSearchResults();
    void clearSearchHighlights();
    void hideSearchBar();

private:
    void setupUi();
    void setupEditor();
    void applyTheme();
    void setupEmacsBindings();
    void createActions();
    void setLexerForLanguage(Language lang);
    void updateModificationState();
    void smartIndent();
    QString getIndentForLine(int line) const;
    bool shouldIncreaseIndent(const QString &line) const;
    bool shouldDecreaseIndent(const QString &line) const;
    
    // Search bar methods
    void createSearchBar();
    void highlightAllMatches();
    
    // Core components
    QsciScintilla *m_editor;
    std::unique_ptr<QsciLexer> m_lexer;
    QToolBar *m_toolbar;
    QLabel *m_statusLabel;
    
    // Search bar components
    QWidget *m_searchBar;
    QLineEdit *m_searchEdit;
    QLabel *m_searchResultLabel;
    QPushButton *m_findPrevButton;
    QPushButton *m_findNextButton;
    QCheckBox *m_caseSensitiveCheck;
    QCheckBox *m_wholeWordCheck;
    
    // State
    QString m_currentFile;
    QString m_originalContent;
    Language m_language;
    bool m_showToolbar;
    
    // Search state
    QString m_lastSearchText;
    QList<int> m_searchResultLines;
    int m_currentSearchResult;
    
    // Line analysis structure
    struct LineAnalysis {
        int openBraces = 0;
        int closeBraces = 0;
        int openBrackets = 0;
        int closeBrackets = 0;
        bool hasContinuation = false;
        bool startsWithCloseBrace = false;
        int leadingCloseBraces = 0;
    };
    
    // Tcl formatting methods
    QString formatTclCodeRobust(const QString &code, int baseIndent = 0);
    QString formatTclCodeWithParser(const QString &code, int baseIndent = 0);
    LineAnalysis analyzeTclLine(const QString &line);
    bool endsWithContinuation(const QString &line);
    bool startsWithSpecialKeyword(const QString &line);
    bool looksLikeProcParameterList(const QString &line);
    
    // Common actions
    QAction *m_saveAction;
    QAction *m_findAction;
    QAction *m_toggleBookmarkAction;
    QAction *m_nextBookmarkAction;
    QAction *m_prevBookmarkAction;
    QAction *m_formatAction;
    QAction *m_formatSelectionAction;
        
    // Constants
    static constexpr int MARGIN_LINE_NUMBERS = 0;
    static constexpr int MARGIN_FOLDING = 1;
    static constexpr int MARGIN_BOOKMARKS = 2;
    static constexpr int BOOKMARK_MARKER = 1;
    static constexpr int SEARCH_INDICATOR = 1;
    static constexpr int CURRENT_SEARCH_INDICATOR = 2;
};
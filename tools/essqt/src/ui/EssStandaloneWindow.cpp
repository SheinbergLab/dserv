#include "EssStandaloneWindow.h"
#include <QCloseEvent>
#include <QVBoxLayout>
#include <QMenuBar>
#include <QAction>
#include <QApplication>

#ifdef Q_OS_MACOS
#include <QOperatingSystemVersion>
#endif

EssStandaloneWindow::EssStandaloneWindow(QWidget* content, 
                                        const QString& title,
                                        WindowBehavior behavior,
                                        QWidget* parent)
    : QMainWindow(parent)
    , m_content(content)
    , m_behavior(behavior)
    , m_stayVisible(behavior == UtilityWindow || behavior == StayVisible)
{
    setWindowTitle(title);
    
    // Take ownership of content
    if (m_content) {
        
        setCentralWidget(m_content);
        
        // Ensure the content is visible
        m_content->setVisible(true);
        m_content->show();
        
    } else {
    }
    
    setupWindowBehavior();
    setupMenuBar();
    setupFocusTracking();
    
    // Size appropriately based on content
    if (content) {
        QSize contentSize = content->sizeHint();
        
        if (contentSize.isValid() && contentSize.width() > 0 && contentSize.height() > 0) {
            // Add some padding for window decorations and menu bar
            QSize windowSize = contentSize + QSize(40, 100);
            resize(windowSize);
        } else {
            // Use the widget's current size if size hint is invalid
            QSize currentSize = content->size();
            if (currentSize.isValid() && currentSize.width() > 0 && currentSize.height() > 0) {
                resize(currentSize + QSize(40, 100));
            } else {
                // Fallback for eye/touch visualizer
                resize(600, 500);
            }
        }
    } else {
        resize(600, 500);
    }
}

void EssStandaloneWindow::setupWindowBehavior()
{
    Qt::WindowFlags flags = Qt::Window;
    
    switch (m_behavior) {
    case Normal:
        // Standard window - no special flags needed
        break;
        
    case UtilityWindow:
        // Tool window that can disappear/reappear with focus tracking
#ifdef Q_OS_MACOS
        flags |= Qt::Tool;
#else
        flags |= Qt::Tool | Qt::WindowStaysOnBehindHint;
#endif
        break;
        
    case StayVisible:
        // Should actually stay visible - use a different approach on macOS
#ifdef Q_OS_MACOS
        // On macOS, don't use Qt::Tool if we want it to stay visible
        // Use a regular window but we'll manage its behavior
        flags = Qt::Window;
#else
        flags |= Qt::WindowStaysOnBehindHint;
#endif
        break;
        
    case AlwaysOnTop:
        // Always on top - use sparingly as it can be annoying
        flags |= Qt::WindowStaysOnTopHint;
        break;
    }
    setWindowFlags(flags);
    
#ifdef Q_OS_MACOS
    setupMacOSBehavior();
#endif
}

void EssStandaloneWindow::setupMacOSBehavior()
{
#ifdef Q_OS_MACOS
    // Don't quit app when this window closes
    setAttribute(Qt::WA_QuitOnClose, false);
    
    if (m_behavior == StayVisible) {
        // For "Always Visible" mode, try to prevent the window from hiding
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAttribute(Qt::WA_MacAlwaysShowToolWindow, true);
        
        // Force the window to stay at a higher level
        winId(); // Force native window creation
    }
    
    // Try to set tool window behavior (may not be available in all Qt6 versions)
    #if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
    if (m_behavior == UtilityWindow) {
        setAttribute(Qt::WA_MacAlwaysShowToolWindow, true);
    }
    #endif
    
#endif
}

void EssStandaloneWindow::setupMenuBar()
{
    // Simple menu for standalone window
    QMenuBar* menuBar = this->menuBar();
    
    QMenu* windowMenu = menuBar->addMenu("Window");
    
    QAction* returnAction = windowMenu->addAction("Return to Main Window");
    returnAction->setShortcut(QKeySequence("Ctrl+R"));
    connect(returnAction, &QAction::triggered, 
            this, &EssStandaloneWindow::returnToMainRequested);
    
    windowMenu->addSeparator();
    
    // Add stay visible toggle for utility and stay visible windows
    if (m_behavior == UtilityWindow || m_behavior == StayVisible) {
        QAction* stayVisibleAction = windowMenu->addAction("Keep Visible When App Loses Focus");
        stayVisibleAction->setCheckable(true);
        stayVisibleAction->setChecked(m_stayVisible);
        connect(stayVisibleAction, &QAction::triggered, [this](bool checked) {
            setStayVisible(checked);
        });
        
        windowMenu->addSeparator();
    }
    
    QAction* closeAction = windowMenu->addAction("Close Window");
    closeAction->setShortcut(QKeySequence::Close);
    connect(closeAction, &QAction::triggered, this, &QWidget::close);
    
    // Add behavior switching for testing
    if (m_behavior != AlwaysOnTop) {
        windowMenu->addSeparator();
        QAction* onTopAction = windowMenu->addAction("Always On Top");
        onTopAction->setCheckable(true);
        onTopAction->setChecked(m_behavior == AlwaysOnTop);
        connect(onTopAction, &QAction::triggered, [this](bool checked) {
            Qt::WindowFlags flags = windowFlags();
            if (checked) {
                flags |= Qt::WindowStaysOnTopHint;
            } else {
                flags &= ~Qt::WindowStaysOnTopHint;
            }
            setWindowFlags(flags);
            show(); // Reshow after changing flags
        });
    }
}

QWidget* EssStandaloneWindow::releaseContent()
{
    QWidget* content = m_content;
    if (content) {        
        // Remove from this window
        setCentralWidget(nullptr);
        
        // Reset parent to allow reparenting
        content->setParent(nullptr);
        
        // Ensure the widget stays visible during transfer
        content->setVisible(true);        
        m_content = nullptr;
    }
    return content;
}

void EssStandaloneWindow::closeEvent(QCloseEvent* event)
{
    emit windowClosing();
    event->accept();
}

bool EssStandaloneWindow::event(QEvent* event)
{
    // Handle window activation on macOS to ensure proper visibility behavior
#ifdef Q_OS_MACOS
    if (event->type() == QEvent::WindowActivate) {
        // Ensure the window is properly raised and activated
        raise();
        activateWindow();
    } else if (event->type() == QEvent::Show) {
        // Ensure proper positioning when shown
        raise();
    }
#endif
    
    return QMainWindow::event(event);
}

void EssStandaloneWindow::changeEvent(QEvent* event)
{
#ifdef Q_OS_MACOS
    if (event->type() == QEvent::WindowStateChange && m_stayVisible) {
        // If we were minimized, restore ourselves when the main app gets focus
        if (isMinimized()) {
        }
    } else if (event->type() == QEvent::ActivationChange && m_stayVisible) {
        // Handle activation changes
    }
#endif
    
    QMainWindow::changeEvent(event);
}

void EssStandaloneWindow::setupFocusTracking()
{
#ifdef Q_OS_MACOS
    if (m_stayVisible) {        
        // Connect to the application's focus change signal
        if (QApplication* app = qobject_cast<QApplication*>(QApplication::instance())) {
            connect(app, &QApplication::focusChanged, 
                    this, [this](QWidget* old, QWidget* now) {
                Q_UNUSED(old)
                
//                 qDebug() << "Focus changed - old:" << old << "new:" << now;
                
                if (!now) return;
                
                // Check if focus went to the main window or one of its children
                QWidget* topLevel = now->window();
//                 qDebug() << "New focus top level window:" << topLevel;
//                 qDebug() << "This window:" << this;
//                 qDebug() << "Parent widget:" << qobject_cast<QWidget*>(parent());
                 
                if (topLevel && topLevel != this) {
                    // Focus went to another window - check if it's our parent/main window
                    QWidget* parentWidget = qobject_cast<QWidget*>(parent());
                    if (parentWidget) {
                        QWidget* parentTopLevel = parentWidget->window();
//                         qDebug() << "Parent top level:" << parentTopLevel;
                        
                        if (topLevel == parentWidget || topLevel == parentTopLevel) {
                            // Main app got focus - make sure we're visible too
//                             qDebug() << "Main app got focus! Window visible:" << isVisible() << "minimized:" << isMinimized();
                            
                            if (!isVisible() || isMinimized()) {
//                                 qDebug() << "Restoring standalone window";
                                show();
                                raise();
                                activateWindow();
                            } else {
                                // Even if visible, make sure it's raised
//                                 qDebug() << "Raising standalone window";
                                raise();
                            }
                        }
                    }
                }
            });
            
            // Also try connecting to application activation
            connect(app, &QApplication::applicationStateChanged,
                    this, [this](Qt::ApplicationState state) {
//                 qDebug() << "Application state changed:" << state;
                if (state == Qt::ApplicationActive && m_stayVisible) {
//                     qDebug() << "App became active, ensuring standalone window is visible";
                    if (!isVisible()) {
                        show();
                        raise();
                    }
                }
            });
        }
    }
#endif
}

void EssStandaloneWindow::setStayVisible(bool stayVisible)
{
    m_stayVisible = stayVisible;
}

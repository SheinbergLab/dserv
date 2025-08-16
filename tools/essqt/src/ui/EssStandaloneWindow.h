#pragma once

#include <QMainWindow>
#include <QWidget>

class QCloseEvent;
class QEvent;

class EssStandaloneWindow : public QMainWindow
{
    Q_OBJECT

public:
    enum WindowBehavior {
        Normal,           // Standard window behavior
        StayVisible,      // Visible but not intrusive  
        AlwaysOnTop,      // Critical monitoring (use sparingly)
        UtilityWindow     // Tool window (best for most cases)
    };

    explicit EssStandaloneWindow(QWidget* content, 
                                const QString& title,
                                WindowBehavior behavior = UtilityWindow,
                                QWidget* parent = nullptr);
    
    QWidget* content() const { return m_content; }
    WindowBehavior behavior() const { return m_behavior; }
    
    // Return content to main window
    QWidget* releaseContent();
    
    // Focus management
    void setStayVisible(bool stayVisible);
    bool stayVisible() const { return m_stayVisible; }

signals:
    void returnToMainRequested();
    void windowClosing();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void setupWindowBehavior();
    void setupMacOSBehavior();
    void setupMenuBar();
    void setupFocusTracking();
    void setMacOSWindowLevel();
    
    QWidget* m_content;
    WindowBehavior m_behavior;
    bool m_stayVisible;
};
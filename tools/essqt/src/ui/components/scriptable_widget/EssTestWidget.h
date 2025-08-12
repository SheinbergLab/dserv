// EssTestWidget.h - Simple test widget for the base class
#ifndef ESSTESTWIDGET_H
#define ESSTESTWIDGET_H

#include "EssScriptableWidget.h"
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>

/**
 * @brief Simple test widget to verify the scriptable base class
 */
class EssTestWidget : public EssScriptableWidget
{
    Q_OBJECT

public:
    explicit EssTestWidget(QWidget* parent = nullptr);

protected:
    void registerCustomCommands() override;
    QString getWidgetTypeName() const override { return "EssTestWidget"; }
    QWidget* createMainWidget() override;
    
    void onSetupComplete() override;

private:
    // Custom Tcl commands for testing
    static int tcl_set_message(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_set_counter(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_add_to_log(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);
    static int tcl_clear_log(ClientData cd, Tcl_Interp* interp, int objc, Tcl_Obj* const objv[]);

private slots:
    void onButtonClicked();
    void onTextChanged();

private:
    // UI components
    QLabel* m_messageLabel;
    QLabel* m_counterLabel;
    QLineEdit* m_textEdit;
    QPushButton* m_testButton;
    QTextEdit* m_logArea;
    
    int m_counter;
};
#endif // ESSTESTWIDGET_H
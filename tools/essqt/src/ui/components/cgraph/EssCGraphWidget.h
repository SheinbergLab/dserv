#ifndef ESS_CGRAPH_WIDGET_H
#define ESS_CGRAPH_WIDGET_H

#include <QWidget>
#include <memory>

QT_BEGIN_NAMESPACE
class QToolBar;
QT_END_NAMESPACE

class EssCommandInterface;
class QtCGTabWidget;

class EssCGraphWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EssCGraphWidget(QWidget *parent = nullptr);
    ~EssCGraphWidget();

    // Set the command interface (must be called before using cgraph features)
    void setCommandInterface(EssCommandInterface *commandInterface);

    // Get the tab widget for direct access if needed
    QtCGTabWidget* tabWidget() const { return m_tabWidget; }

signals:
    void graphUpdated();
    void statusMessage(const QString &message, int timeout = 0);

private slots:
    void onAddTab();
    void onExportGraph();
    void onClearGraph();
    void onRefreshGraph();
    void onGraphUpdated();

private:
    void setupUi();
    void registerWithTcl();

    EssCommandInterface *m_commandInterface;
    QtCGTabWidget *m_tabWidget;
};

#endif // ESS_CGRAPH_WIDGET_H
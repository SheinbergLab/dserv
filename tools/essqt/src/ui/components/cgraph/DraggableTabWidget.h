#ifndef DRAGGABLETABWIDGET_H
#define DRAGGABLETABWIDGET_H

#include <QTabWidget>
#include <QPoint>

class QMouseEvent;
class QDragEnterEvent;
class QDropEvent;

class DraggableTabWidget : public QTabWidget
{
    Q_OBJECT
    
public:
    explicit DraggableTabWidget(QWidget* parent = nullptr);
    
signals:
    void tabDetached(QWidget* widget, const QString& title, const QPoint& globalPos);
    void tabDropped(QWidget* widget, int index);
    
private slots:
    void handleTabDetachRequest(int index, const QPoint& globalPos);
    
protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    
private:
    QPoint m_dragStartPos;
    QWidget* m_draggedWidget;
    int m_draggedIndex;
};

#endif // DRAGGABLETABWIDGET_H
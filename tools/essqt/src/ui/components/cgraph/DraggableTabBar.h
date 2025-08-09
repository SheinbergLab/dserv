#ifndef DRAGGABLETABBAR_H
#define DRAGGABLETABBAR_H

#include <QTabBar>
#include <QPoint>

class QWidget;

class DraggableTabBar : public QTabBar
{
    Q_OBJECT
    
public:
    explicit DraggableTabBar(QWidget* parent = nullptr);
    ~DraggableTabBar();
    
signals:
    void tabDetachRequested(int index, const QPoint& globalPos);
    
protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    
private:
    QPoint m_dragStartPos;
    int m_pressedIndex = -1;
    QWidget* m_dragPreview = nullptr;
};

#endif // DRAGGABLETABBAR_H
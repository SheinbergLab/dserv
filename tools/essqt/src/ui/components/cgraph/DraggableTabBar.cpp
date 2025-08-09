#include "DraggableTabBar.h"
#include <QMouseEvent>
#include <QApplication>
#include <QPainter>
#include <QLabel>

// Simple drag preview widget
class DragPreview : public QWidget
{
public:
    DragPreview(const QString& text, QWidget* parent = nullptr)
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        
        m_text = text;
        resize(150, 30);
    }
    
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        
        // Draw semi-transparent background
        painter.setBrush(QColor(70, 70, 70, 200));
        painter.setPen(QPen(QColor(100, 100, 100), 1));
        painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 4, 4);
        
        // Draw text
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter, m_text);
    }
    
private:
    QString m_text;
};

DraggableTabBar::DraggableTabBar(QWidget* parent) 
    : QTabBar(parent)
    , m_dragPreview(nullptr)
{
    setMouseTracking(true);
}

DraggableTabBar::~DraggableTabBar()
{
    delete m_dragPreview;
}

void DraggableTabBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragStartPos = event->pos();
        m_pressedIndex = tabAt(event->pos());
    }
    QTabBar::mousePressEvent(event);
}

void DraggableTabBar::mouseMoveEvent(QMouseEvent* event)
{
    // Only handle drag if we started with a valid tab
    if ((event->buttons() & Qt::LeftButton) && m_pressedIndex >= 0) {
        int distance = (event->pos() - m_dragStartPos).manhattanLength();
        
        if (distance >= QApplication::startDragDistance()) {
            // Check if we've moved far enough vertically to detach
            int verticalDistance = qAbs(event->pos().y() - m_dragStartPos.y());
            
            // Show preview when dragging vertically
            if (verticalDistance > 20) {  // Show preview earlier than detach
                if (!m_dragPreview) {
                    m_dragPreview = new DragPreview(tabText(m_pressedIndex));
                    m_dragPreview->show();
                }
                
                // Position preview near cursor
                m_dragPreview->move(event->globalPosition().toPoint() + QPoint(10, 10));
                
                // Change cursor to indicate detach mode
                setCursor(Qt::DragMoveCursor);
            }
            
            // If dragged down/up far enough, request detach
            if (verticalDistance > 40) {  // Actual detach threshold
                emit tabDetachRequested(m_pressedIndex, event->globalPosition().toPoint());
                
                // Clean up preview
                delete m_dragPreview;
                m_dragPreview = nullptr;
                setCursor(Qt::ArrowCursor);
                
                m_pressedIndex = -1;  // Reset to prevent multiple signals
                return;
            }
        }
    }
    QTabBar::mouseMoveEvent(event);
}

void DraggableTabBar::mouseReleaseEvent(QMouseEvent* event)
{
    m_pressedIndex = -1;
    
    // Clean up preview if still showing
    if (m_dragPreview) {
        delete m_dragPreview;
        m_dragPreview = nullptr;
    }
    
    setCursor(Qt::ArrowCursor);
    QTabBar::mouseReleaseEvent(event);
}
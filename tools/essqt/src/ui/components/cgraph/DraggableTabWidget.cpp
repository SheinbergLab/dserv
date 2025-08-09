#include "DraggableTabWidget.h"
#include "DraggableTabBar.h"
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QApplication>
#include <QTabBar>
#include <QDebug>
#include <QDrag>

DraggableTabWidget::DraggableTabWidget(QWidget* parent)
    : QTabWidget(parent)
    , m_draggedWidget(nullptr)
    , m_draggedIndex(-1)
{
    // Create and set our custom tab bar
    DraggableTabBar* customTabBar = new DraggableTabBar(this);
    setTabBar(customTabBar);
    
    // Connect the detach signal
    connect(customTabBar, &DraggableTabBar::tabDetachRequested,
            this, &DraggableTabWidget::handleTabDetachRequest);
    
    setAcceptDrops(true);
    setMovable(true);  // Still allow reordering within the tab bar
    setTabsClosable(true);
    setDocumentMode(true);
    
    // No need for extra margins since we're in a container now
    setContentsMargins(0, 0, 0, 0);
    
    // Style for better appearance and more compact tabs
    setStyleSheet(
        "QTabWidget::pane { "
        "    border: 1px solid palette(mid); "
        "    background: palette(window); "
        "    top: -1px; "  // Overlap with tab bar slightly
        "} "
        "QTabBar::tab { "
        "    padding: 2px 8px; "  // Compact tabs
        "    margin: 0px; "
        "    margin-right: 2px; "  // Small gap between tabs
        "} "
        "QTabBar::tab:selected { "
        "    background: palette(window); "
        "    border: 1px solid palette(mid); "
        "    border-bottom: 1px solid palette(window); "
        "} "
        "QTabBar::tab:!selected { "
        "    background: palette(button); "
        "    border: 1px solid palette(mid); "
        "    margin-top: 2px; "
        "} "
        "QTabBar::close-button { "
        "    image: url(:/icons/close.png); "
        "    subcontrol-position: right; "
        "    padding: 2px; "
        "}"
    );
}

void DraggableTabWidget::handleTabDetachRequest(int index, const QPoint& globalPos)
{
    if (index < 0 || index >= count()) return;
    
    QWidget* widget = this->widget(index);
    QString title = tabText(index);
    
    // Remove the tab
    removeTab(index);
    
    // Emit our signal
    emit tabDetached(widget, title, globalPos);
}

void DraggableTabWidget::mousePressEvent(QMouseEvent* event)
{
    // Let the tab bar handle it
    QTabWidget::mousePressEvent(event);
}

void DraggableTabWidget::mouseMoveEvent(QMouseEvent* event)
{
    // Let the tab bar handle it
    QTabWidget::mouseMoveEvent(event);
}

void DraggableTabWidget::mouseReleaseEvent(QMouseEvent* event)
{
    // Let the tab bar handle it
    QTabWidget::mouseReleaseEvent(event);
}

void DraggableTabWidget::dragEnterEvent(QDragEnterEvent* event)
{
    // Support dropping tabs back into the widget
    if (event->mimeData()->hasFormat("application/x-cgraph-tab")) {
        event->acceptProposedAction();
    }
}

void DraggableTabWidget::dropEvent(QDropEvent* event)
{
    if (event->mimeData()->hasFormat("application/x-cgraph-tab")) {
        // Handle dropping a tab back into this widget
        // For now, we'll handle this through other mechanisms (context menu, double-click)
        event->acceptProposedAction();
    }
}
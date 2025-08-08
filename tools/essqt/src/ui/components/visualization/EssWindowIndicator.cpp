#include "EssWindowIndicator.h"
#include <QPainter>
#include <QPaintEvent>

EssWindowIndicator::EssWindowIndicator(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(24);
    m_windows.resize(8);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void EssWindowIndicator::setWindowCount(int count) {
    m_windows.resize(count);
    updateGeometry();
    update();
}

void EssWindowIndicator::setWindowStatus(int index, bool active, bool inside) {
    if (index >= 0 && index < m_windows.size()) {
        m_windows[index].active = active;
        m_windows[index].inside = inside;
        update();
    }
}

void EssWindowIndicator::setLabel(const QString& label) {
    m_label = label;
    updateGeometry();
    update();
}

// In EssWindowIndicator.cpp, update the paintEvent:

void EssWindowIndicator::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    
    int x = 5;
    
    // Draw label
    if (!m_label.isEmpty()) {
        painter.setPen(palette().color(QPalette::Text));
        QFont font;
		font.setStyleHint(QFont::SansSerif);
		font.setPointSize(9);
		painter.setFont(font);
        painter.drawText(x, 0, 50, height(), Qt::AlignVCenter, m_label + ":");
        x += 55;
    }
    
    // Draw window indicators
    for (int i = 0; i < m_windows.size(); ++i) {
        QRect rect(x + i * (INDICATOR_SIZE + SPACING), 
                  (height() - INDICATOR_SIZE) / 2,
                  INDICATOR_SIZE, INDICATOR_SIZE);
        
        // Background
        if (m_windows[i].inside) {
            painter.fillRect(rect, QColor(82, 196, 26));  // Green when inside
        } else if (m_windows[i].active) {
            painter.fillRect(rect, QColor(51, 51, 51));   // Dark gray when active
        } else {
            painter.fillRect(rect, QColor(31, 31, 31));   // Darker gray when inactive
        }
        
        // Border - thicker and brighter for active windows
        if (m_windows[i].active) {
            painter.setPen(QPen(QColor(24, 144, 255), 2));  // Bright blue, 2px
        } else {
            painter.setPen(QPen(QColor(102, 102, 102), 1)); // Gray, 1px
        }
        painter.drawRect(rect);

		QFont font;
		font.setStyleHint(QFont::Monospace);
		font.setPointSize(8);

        // Number - bold for active windows
        if (m_windows[i].active) {
			font.setWeight(QFont::Bold);
            painter.setFont(font);
            painter.setPen(m_windows[i].inside ? Qt::white : Qt::white);
        } else {
  			font.setWeight(QFont::Normal);
            painter.setFont(font);
            painter.setPen(QColor(153, 153, 153));  // Dimmed for inactive
        }
        
        painter.drawText(rect, Qt::AlignCenter, QString::number(i));
    }
}

QSize EssWindowIndicator::sizeHint() const {
    int width = 60 + m_windows.size() * (INDICATOR_SIZE + SPACING);
    return QSize(width, 24);
}

QSize EssWindowIndicator::minimumSizeHint() const {
    return sizeHint();
}
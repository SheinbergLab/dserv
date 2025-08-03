#pragma once
#include <QWidget>
#include <QVector>

class EssWindowIndicator : public QWidget {
    Q_OBJECT
    
public:
    explicit EssWindowIndicator(QWidget *parent = nullptr);
    
    void setWindowCount(int count);
    void setWindowStatus(int index, bool active, bool inside);
    void setLabel(const QString& label);
    int windowCount() const { return m_windows.size(); }
    
protected:
    void paintEvent(QPaintEvent *event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    
private:
    struct WindowState {
        bool active = false;
        bool inside = false;
    };
    
    QString m_label;
    QVector<WindowState> m_windows;
    static constexpr int INDICATOR_SIZE = 18;
    static constexpr int SPACING = 4;
};
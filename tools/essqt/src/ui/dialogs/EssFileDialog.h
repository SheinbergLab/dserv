#pragma once

#include <QDialog>
#include <QString>

class QLineEdit;
class QPushButton;
class QLabel;
class QProgressBar;

class EssFileDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EssFileDialog(QWidget *parent = nullptr);
    ~EssFileDialog();

    // Get the final filename chosen by user
    QString selectedFilename() const;
    
    // Static convenience method
    static QString getDatafileName(QWidget *parent = nullptr);

public slots:
    void suggestFilename();
    
private slots:
    void onSuggestClicked();
    void onFilenameChanged();
    void onAccepted();
    void updateButtonStates();

private:
    void setupUi();
    void connectSignals();
    bool validateFilename(const QString &filename) const;
    void showValidationError(const QString &message);
    
    // UI Elements
    QLineEdit *m_filenameEdit;
    QPushButton *m_suggestBtn;
    QPushButton *m_okBtn;
    QPushButton *m_cancelBtn;
    QLabel *m_statusLabel;
    QProgressBar *m_progressBar;
    
    // State
    QString m_selectedFilename;
    bool m_suggesting;
};
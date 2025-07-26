#pragma once

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QFrame>
#include "SystemConfigWidget.h"

class EssControlWidget : public QWidget {
    Q_OBJECT

public:
    explicit EssControlWidget(QWidget* parent = nullptr);

    // Access to sub-components
    SystemConfigWidget* systemConfig() const { return systemConfigWidget; }
    
    // Subject management
    QString currentSubject() const;
    void setSubjectList(const QStringList& subjects);
    void setCurrentSubject(const QString& subject);
    
    // Status display
    void setSystemStatus(const QString& status);
    void setObservationCount(const QString& obsCount);
    void setObservationActive(bool active);

signals:
    // Subject signals
    void subjectChanged(const QString& subject);
    
    // Control button signals
    void startRequested();
    void stopRequested();
    void resetRequested();
    
    // Settings signals
    void saveSettingsRequested();
    void resetSettingsRequested();
    
    // Forward system config signals
    void systemChanged(const QString& system);
    void protocolChanged(const QString& protocol);
    void variantChanged(const QString& variant);
    void reloadSystemRequested();
    void reloadProtocolRequested();
    void reloadVariantRequested();

private slots:
    void onSubjectChanged(int index);
    void forwardSystemSignals();

private:
    void setupUI();
    void createSubjectSection();
    void createControlSection();
    void createStatusSection();
    void createSettingsSection();

    // Sub-components
    SystemConfigWidget* systemConfigWidget;
    
    // Subject section
    QGroupBox* subjectGroup;
    QComboBox* subjectCombo;
    
    // Control section
    QGroupBox* controlGroup;
    QPushButton* goBtn;
    QPushButton* stopBtn;
    QPushButton* resetBtn;
    
    // Status section
    QGroupBox* statusGroup;
    QLabel* systemStatusLabel;
    QLabel* obsCountLabel;
    QLabel* obsIndicator;
    
    // Settings section
    QGroupBox* settingsGroup;
    QPushButton* saveSettingsBtn;
    QPushButton* resetSettingsBtn;
};

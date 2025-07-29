#pragma once

#include <QWidget>
#include <QVariant>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <memory>

class QGroupBox;
class QPushButton;
class QComboBox;
class QLabel;
class QScrollArea;
class QFormLayout;

class EssExperimentControlWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EssExperimentControlWidget(QWidget *parent = nullptr);
    ~EssExperimentControlWidget();

    // Current selections
    QString currentSystem() const;
    QString currentProtocol() const;
    QString currentVariant() const;
    
    // Control state
    bool isRunning() const { return m_isRunning; }
    
    // Reset to disconnected state
    void resetToDisconnectedState();

signals:
    // User-initiated actions
    void experimentStarted();
    void experimentStopped();
    void experimentReset();
    
    void systemChanged(const QString &system);
    void protocolChanged(const QString &protocol);
    void variantChanged(const QString &variant);
    
    void reloadSystemRequested();
    void reloadProtocolRequested();
    void reloadVariantRequested();

    // Settings save/reset
    void saveSettingsRequested();
    void resetSettingsRequested();

private slots:
    // DataProcessor signals
    void onSystemStatusUpdated(const QString &status);
    void onExperimentStateChanged(const QString &state);
    void onGenericDatapointReceived(const QString &name, const QVariant &value, qint64 timestamp);
    
    // UI control slots
    void onStartClicked();
    void onStopClicked();
    void onResetClicked();
    
    void onSystemComboChanged(int index);
    void onProtocolComboChanged(int index);
    void onVariantComboChanged(int index);
    
    void onReloadSystemClicked();
    void onReloadProtocolClicked();
    void onReloadVariantClicked();
    
    void onSaveSettingsClicked();
    void onResetSettingsClicked();

private:
    void setupUi();
    void connectSignals();
    void createControlSection();
    void createStatusSection();
    void createSystemConfigSection();
    void createParameterSection();
    void createVariantOptionsSection();
    void createSettingsSection();
    
    void updateButtonStates();
    void updateStatusDisplay();
    
    // Helper to update combo without triggering signals
    void setComboBoxValue(QComboBox *combo, const QString &value);
    void setComboBoxItems(QComboBox *combo, const QStringList &items, const QString &currentValue);
    
    // Process specific datapoints
    void processSystemDatapoint(const QString &name, const QVariant &value);
    void processEssDatapoint(const QString &name, const QVariant &value);
    
    // Parse Tcl list format
    QStringList parseTclList(const QString &tclList) const;
    
    // Update parameter and variant settings
    void updateParameterSettings(const QJsonObject &params);
    void updateVariantOptions(const QJsonObject &variantInfo);
    void clearParameterSettings();
    void clearVariantOptions();
    
    // UI Elements
    // Control section
    QGroupBox *m_controlGroup;
    QPushButton *m_startBtn;
    QPushButton *m_stopBtn;
    QPushButton *m_resetBtn;
    
    // Status section
    QGroupBox *m_statusGroup;
    QLabel *m_statusLabel;
    QLabel *m_obsCountLabel;
    QLabel *m_obsIndicator;
    
    // System configuration section
    QGroupBox *m_systemConfigGroup;
    QComboBox *m_systemCombo;
    QComboBox *m_protocolCombo;
    QComboBox *m_variantCombo;
    QPushButton *m_reloadSystemBtn;
    QPushButton *m_reloadProtocolBtn;
    QPushButton *m_reloadVariantBtn;
    
    // Parameter section
    QGroupBox *m_parameterGroup;
    QScrollArea *m_parameterScrollArea;
    QWidget *m_parameterContainer;
    QFormLayout *m_parameterLayout;
    
    // Variant options section
    QGroupBox *m_variantOptionsGroup;
    QScrollArea *m_variantOptionsScrollArea;
    QWidget *m_variantOptionsContainer;
    QFormLayout *m_variantOptionsLayout;

    // Settings section
    QGroupBox *m_settingsGroup;
    QPushButton *m_saveSettingsBtn;
    QPushButton *m_resetSettingsBtn;
    
    // State tracking
    bool m_isRunning;
    QString m_currentStatus;
    int m_currentObsId;
    int m_totalObs;
    bool m_observationActive;
    
    // Flag to prevent infinite signal loops
    bool m_blockSignals;
    
    // Track if we need to update after lists change
    QString m_pendingSystem;
    QString m_pendingProtocol;
    QString m_pendingVariant;
};

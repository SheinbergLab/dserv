#pragma once

#include <QWidget>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QStringList>

class SystemConfigWidget : public QGroupBox {
    Q_OBJECT

public:
    explicit SystemConfigWidget(QWidget* parent = nullptr);

    // Accessors for current selections
    QString currentSystem() const;
    QString currentProtocol() const; 
    QString currentVariant() const;

    // Methods to populate dropdowns (equivalent to FLTK's add())
    void setSystemList(const QStringList& systems);
    void setProtocolList(const QStringList& protocols);
    void setVariantList(const QStringList& variants);

    // Methods to set current selection (equivalent to FLTK's value())
    void setCurrentSystem(const QString& system);
    void setCurrentProtocol(const QString& protocol);
    void setCurrentVariant(const QString& variant);

    // Clear methods
    void clearSystems();
    void clearProtocols();
    void clearVariants();

signals:
    // Emitted when user changes selections
    void systemChanged(const QString& system);
    void protocolChanged(const QString& protocol);
    void variantChanged(const QString& variant);

    // Emitted when reload buttons are clicked
    void reloadSystemRequested();
    void reloadProtocolRequested();
    void reloadVariantRequested();

private slots:
    void onSystemChanged(int index);
    void onProtocolChanged(int index);
    void onVariantChanged(int index);

private:
    void setupUI();

    QComboBox* systemCombo;
    QComboBox* protocolCombo;
    QComboBox* variantCombo;
    
    QPushButton* reloadSystemBtn;
    QPushButton* reloadProtocolBtn;
    QPushButton* reloadVariantBtn;
};

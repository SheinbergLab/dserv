// EssApplication.h
#pragma once

#include <QApplication>
#include <memory>

class EssConfig;
class EssCommandInterface;
class EssDataProcessor;
class EssMainWindow;

class EssApplication : public QApplication
{
    Q_OBJECT

public:
    EssApplication(int &argc, char **argv);
    ~EssApplication();

    static EssApplication* instance();
    
    // Version info
    static const QString Version;
    static const QString Organization;
    static const QString ApplicationName;
    
    // Core services - globally accessible
    EssConfig* config() const { return m_config.get(); }
    EssCommandInterface* commandInterface() const { return m_commandInterface.get(); }
    EssDataProcessor* dataProcessor() const { return m_dataProcessor.get(); }
    EssMainWindow* mainWindow() const { return m_mainWindow; } 
    
signals:
    void disconnectCancelled();  // Emitted when user cancels disconnect due to unsaved changes

private slots:
    void handleDisconnectRequest();


private:
    void initializeServices();
    void shutdownServices();
    
    std::unique_ptr<EssConfig> m_config;
    std::unique_ptr<EssCommandInterface> m_commandInterface;
    std::unique_ptr<EssDataProcessor> m_dataProcessor;
    EssMainWindow* m_mainWindow;
};

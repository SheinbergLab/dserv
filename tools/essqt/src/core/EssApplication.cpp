// EssApplication.cpp - Complete implementation with disconnect handling
#include "EssApplication.h"
#include "EssConfig.h"
#include "EssCommandInterface.h"
#include "EssDataProcessor.h"
#include "EssScriptEditorWidget.h"  // Add this include
#include "console/EssOutputConsole.h"  // Add for logging
#include <QDebug>

const QString EssApplication::Version = "0.1.0";
const QString EssApplication::Organization = "ESS";
const QString EssApplication::ApplicationName = "EssQt";

EssApplication::EssApplication(int &argc, char **argv)
    : QApplication(argc, argv)
{
    setOrganizationName(Organization);
    setApplicationName(ApplicationName);
    setApplicationVersion(Version);
    
    // Initialize core services
    initializeServices();
}

EssApplication::~EssApplication()
{
    shutdownServices();
}

void EssApplication::initializeServices()
{
    // Create config first
    m_config = std::make_unique<EssConfig>();
    
    // Create command interface
    m_commandInterface = std::make_unique<EssCommandInterface>();
    
    // Create data processor
    m_dataProcessor = std::make_unique<EssDataProcessor>();
    
    // Connect command interface to data processor - now with dtype parameter
    connect(m_commandInterface.get(), &EssCommandInterface::datapointUpdated,
            m_dataProcessor.get(), &EssDataProcessor::processDatapoint);
    
    // Connect disconnect request to handler
    connect(m_commandInterface.get(), &EssCommandInterface::disconnectRequested,
            this, &EssApplication::handleDisconnectRequest);
    
    qDebug() << "ESS Application services initialized";
}

void EssApplication::shutdownServices()
{
    if (m_commandInterface) {
        m_commandInterface->disconnectFromHost();
    }
    // Shutdown in reverse order
    m_dataProcessor.reset();
    m_commandInterface.reset();
    m_config.reset();
    
    qDebug() << "ESS Application services shut down";
}

void EssApplication::handleDisconnectRequest()
{
    // Find script editor widget
    auto* scriptEditor = findChild<EssScriptEditorWidget*>();
    
    // Check for unsaved changes if script editor exists
    if (scriptEditor && !scriptEditor->confirmDisconnectWithUnsavedChanges()) {
        EssConsoleManager::instance()->logInfo("Disconnect cancelled due to unsaved scripts", "Application");
        emit disconnectCancelled();
        return;
    }
    
    // Proceed with disconnect
    if (m_commandInterface) {
        EssConsoleManager::instance()->logInfo("Proceeding with disconnect", "Application");
        m_commandInterface->disconnectFromHost();
    }
}

EssApplication* EssApplication::instance()
{
    return qobject_cast<EssApplication*>(QApplication::instance());
}
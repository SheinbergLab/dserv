// EssApplication.cpp
#include "EssApplication.h"
#include "EssConfig.h"
#include "EssCommandInterface.h"
#include "EssDataProcessor.h"
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
    
    // Connect command interface to data processor
    connect(m_commandInterface.get(), &EssCommandInterface::datapointUpdated,
            m_dataProcessor.get(), &EssDataProcessor::processDatapoint);
    
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

EssApplication* EssApplication::instance()
{
    return qobject_cast<EssApplication*>(QApplication::instance());
}

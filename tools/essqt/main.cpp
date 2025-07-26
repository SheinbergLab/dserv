#include <QApplication>
#include <QStyleFactory>
#include <QDir>
#include "MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    // Set application properties
    app.setApplicationName("essgui");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("Sheinberg Lab");
    
    // Optional: Set a nice style (you can comment this out if you prefer default)
    #ifdef Q_OS_WIN
    app.setStyle(QStyleFactory::create("Fusion"));
    #endif
    
    // Create and show main window
    MainWindow window;
    window.show();
    
    return app.exec();
}

#include <QApplication>
#include <QLoggingCategory>
#include "core/EssApplication.h"
#include "ui/EssMainWindow.h"

int main(int argc, char *argv[])
{

#ifdef QT_DEBUG
    // Comment out or modify this line to reduce verbosity
    // QLoggingCategory::setFilterRules("*.debug=true");
    
    // Only show warnings and above
    QLoggingCategory::setFilterRules("*.warning=true");
#endif


    EssApplication app(argc, argv);
    
    EssMainWindow window;
    window.show();
    
    return app.exec();
}

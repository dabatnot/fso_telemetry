#include "main_window.h"

#include <QApplication>
#include <QSettings>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("FS2Open"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("fs2open.github.com"));
    QCoreApplication::setApplicationName(QStringLiteral("AV DS — AV Display System"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    simpit::radar::MainWindow window;
    window.show();
    return application.exec();
}

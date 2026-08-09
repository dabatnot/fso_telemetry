#include "main_window.h"

#include <QApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("FS2Open"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("fs2open.github.com"));
    QCoreApplication::setApplicationName(QStringLiteral("FsoSimpitRadar"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0.0"));

    QTranslator qtTranslator;
    const QString locale = QLocale::system().language() == QLocale::English
        ? QStringLiteral("en") : QStringLiteral("fr");
    if (qtTranslator.load(QStringLiteral("qt_%1").arg(locale),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        application.installTranslator(&qtTranslator);
    QTranslator applicationTranslator;
    if (applicationTranslator.load(QStringLiteral(":/i18n/radar_%1.qm").arg(locale)))
        application.installTranslator(&applicationTranslator);

    simpit::radar::MainWindow window;
    window.show();
    return application.exec();
}

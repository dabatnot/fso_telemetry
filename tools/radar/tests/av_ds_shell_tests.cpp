#include "application_settings.h"
#include "av_ds_test_support.h"
#include "display_unit.h"
#include "main_window.h"

#include <QTest>

#include <memory>

using namespace simpit::radar;

class AvDsShellTests final : public QObject {
    Q_OBJECT
private slots:
    void mfdLeftStartsOnTheOnlyRadarPage()
    {
        DisplayUnit unit(DisplayUnitId::MfdLeft, new QWidget);
        QCOMPARE(unit.id(), DisplayUnitId::MfdLeft);
        QCOMPARE(unit.activePage(), PageId::Radar);
        QCOMPARE(unit.pageCatalog().size(), std::size_t{1});
        QCOMPARE(unit.pageCatalog().front(), PageId::Radar);
        QVERIFY(!unit.setActivePage(static_cast<PageId>(99)));
    }

    void mainWindowHasOneMaximizableMfdLeft()
    {
        test::TemporarySettings temporary;
        auto settings = std::make_unique<ApplicationSettings>(temporary.settingsPath());
        MainWindow window(std::move(settings));
        QCOMPARE(window.windowTitle(), QStringLiteral("AV DS — AV Display System"));
        QVERIFY(window.windowFlags().testFlag(Qt::WindowMaximizeButtonHint));
        QCOMPARE(window.displayUnit()->id(), DisplayUnitId::MfdLeft);
        QCOMPARE(window.displayUnit()->activePage(), PageId::Radar);
        QCOMPARE(window.displayUnit()->pageCatalog().size(), std::size_t{1});
    }
};

QTEST_MAIN(AvDsShellTests)
#include "av_ds_shell_tests.moc"

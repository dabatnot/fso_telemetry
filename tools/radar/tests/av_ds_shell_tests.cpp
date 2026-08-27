#include "application_settings.h"
#include "av_ds_message.h"
#include "av_ds_test_support.h"
#include "display_unit.h"
#include "main_window.h"

#include <QTest>

#include <array>
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

    void globalMessagesMapClientStatusAndReachEveryDisplayUnit()
    {
        DisplayUnit left(DisplayUnitId::MfdLeft, new QWidget);
        DisplayUnit testUnit(DisplayUnitId::MfdLeft, new QWidget);
        struct ExpectedMessage {
            ClientStatus status;
            const char* title;
            const char* detail;
            const char* color;
            bool visible;
            bool dimsContent;
        };
        const std::array expected{
            ExpectedMessage{ClientStatus::Disconnected, "SENSOR LINK DISCONNECTED",
                            "PRESS ESCAPE OR CTRL+, TO CONFIGURE", "#FF5C57", true, false},
            ExpectedMessage{ClientStatus::Resolving, "ESTABLISHING SENSOR LINK",
                            "STANDBY", "#66D9E8", true, false},
            ExpectedMessage{ClientStatus::Connecting, "ESTABLISHING SENSOR LINK",
                            "STANDBY", "#66D9E8", true, false},
            ExpectedMessage{ClientStatus::Synchronizing, "BUILDING TACTICAL PICTURE",
                            "STANDBY", "#66D9E8", true, false},
            ExpectedMessage{ClientStatus::Ready, "SENSOR LINK READY",
                            "WAITING FOR MISSION", "#66D9E8", true, false},
            ExpectedMessage{ClientStatus::Live, "", "", "#000000", false, false},
            ExpectedMessage{ClientStatus::Paused, "MISSION PAUSED",
                            "SENSOR LINK MAINTAINED", "#66D9E8", true, false},
            ExpectedMessage{ClientStatus::Stale, "SENSOR FEED LOST",
                            "HOLDING LAST CONTACT PICTURE", "#FFB83D", true, true},
            ExpectedMessage{ClientStatus::Reconnecting, "REACQUIRING SENSOR LINK",
                            "STANDBY", "#66D9E8", true, false},
            ExpectedMessage{ClientStatus::Error, "SENSOR LINK FAILURE",
                            "CHECK TELEMETRY SOURCE", "#FF5C57", true, false},
        };
        for (const auto& item : expected) {
            const AvDsMessage message = messageForClientStatus(item.status);
            QCOMPARE(message.title, QString::fromLatin1(item.title));
            QCOMPARE(message.detail, QString::fromLatin1(item.detail));
            QCOMPARE(message.visible, item.visible);
            QCOMPARE(message.dimsContent, item.dimsContent);
            if (item.visible) QCOMPARE(message.color, QColor(QString::fromLatin1(item.color)));
        }

        const AvDsMessage stale = messageForClientStatus(ClientStatus::Stale);
        const PageId leftPage = left.activePage();
        const PageId testPage = testUnit.activePage();

        left.setMessage(stale);
        testUnit.setMessage(stale);
        QCOMPARE(left.message().title, stale.title);
        QCOMPARE(testUnit.message().title, stale.title);
        QCOMPARE(left.activePage(), leftPage);
        QCOMPARE(testUnit.activePage(), testPage);

        left.resize(320, 320);
        const QImage staleRender = test::renderOffscreen(left, left.size());
        left.setMessage(messageForClientStatus(ClientStatus::Live));
        const QImage liveRender = test::renderOffscreen(left, left.size());
        QVERIFY(staleRender != liveRender);
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

#include "application_settings.h"
#include "av_ds_test_support.h"

#include <QTest>

using namespace simpit::radar;

class ApplicationSettingsTests final : public QObject {
    Q_OBJECT
private slots:
    void emptyConfiguredHostRequiresConfiguration()
    {
        test::TemporarySettings temporary;
        QSettings seeded = temporary.open();
        seeded.setValue(QStringLiteral("connection/configured"), true);
        seeded.setValue(QStringLiteral("connection/host"), QStringLiteral("   "));
        seeded.setValue(QStringLiteral("connection/port"), 42042);
        seeded.sync();

        const ApplicationSettings settings(temporary.settingsPath());
        const auto connection = settings.connection();
        QVERIFY(!connection.configured);
        QCOMPARE(connection.host, QStringLiteral("127.0.0.1"));
        QCOMPARE(connection.port, quint16{42042});
    }

    void readsAndWritesLegacyRadarNamespace()
    {
        test::TemporarySettings temporary;
        QSettings seeded = temporary.open();
        seeded.setValue(QStringLiteral("connection/configured"), true);
        seeded.setValue(QStringLiteral("connection/host"), QStringLiteral("192.0.2.42"));
        seeded.setValue(QStringLiteral("connection/port"), 42043);
        seeded.setValue(QStringLiteral("display/targetCallout"), false);
        seeded.setValue(QStringLiteral("display/targetStrength"), false);
        seeded.setValue(QStringLiteral("display/lead"), false);
        seeded.setValue(QStringLiteral("display/lock"), false);
        seeded.setValue(QStringLiteral("display/subsystems"), false);
        seeded.setValue(QStringLiteral("display/edgeThreats"), false);
        seeded.setValue(QStringLiteral("display/sensorEffects"), false);
        seeded.setValue(QStringLiteral("display/motionVectors"), true);
        seeded.setValue(QStringLiteral("display/trails"), true);
        seeded.setValue(QStringLiteral("window/geometry"), QByteArray("legacy-geometry"));
        seeded.sync();

        ApplicationSettings settings(temporary.settingsPath());
        auto connection = settings.connection();
        QVERIFY(connection.configured);
        QCOMPARE(connection.host, QStringLiteral("192.0.2.42"));
        QCOMPARE(connection.port, quint16{42043});
        const RadarDisplaySettings display = settings.displaySettings();
        QVERIFY(!display.targetCallout);
        QVERIFY(!display.targetStrength);
        QVERIFY(!display.lead);
        QVERIFY(!display.lock);
        QVERIFY(!display.subsystems);
        QVERIFY(!display.edgeThreats);
        QVERIFY(!display.sensorEffects);
        QVERIFY(display.motionVectors);
        QVERIFY(display.trails);
        QCOMPARE(settings.windowGeometry(), QByteArray("legacy-geometry"));

        connection.configured = true;
        connection.host = QStringLiteral("198.51.100.7");
        connection.port = 42044;
        settings.setConnection(connection);
        settings.setDisplaySettings({true, true, true, true, true, true, true, false, false});
        settings.setWindowGeometry(QByteArray("updated-geometry"));
        settings.sync();

        QSettings persisted = temporary.open();
        QCOMPARE(persisted.value(QStringLiteral("connection/host")).toString(),
                 QStringLiteral("198.51.100.7"));
        QCOMPARE(persisted.value(QStringLiteral("connection/port")).toUInt(), 42044U);
        QVERIFY(persisted.value(QStringLiteral("display/targetCallout")).toBool());
        QVERIFY(!persisted.value(QStringLiteral("display/trails")).toBool());
        QCOMPARE(persisted.value(QStringLiteral("window/geometry")).toByteArray(),
                 QByteArray("updated-geometry"));
    }
};

QTEST_MAIN(ApplicationSettingsTests)
#include "application_settings_tests.moc"

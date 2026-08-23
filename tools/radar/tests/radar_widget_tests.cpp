#include "radar_widget.h"
#include "radar_icons.h"
#include "settings_dialog.h"
#include "svg_icon_cache.h"
#include "vfnt_font.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <QImage>
#include <QFile>
#include <QBuffer>
#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <initializer_list>

using namespace simpit::radar;
namespace protocol = telemetry::protocol;

class RadarWidgetTests final : public QObject {
    Q_OBJECT
private:
    static qint64 firstPresentationTime(
        std::uint64_t contactId, DistortedContactPresentation presentation)
    {
        RadarContact contact;
        contact.id = contactId;
        contact.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Distorted);
        for (qint64 interval = 0; interval < 10'000; ++interval) {
            const qint64 milliseconds = interval * 120;
            if (RadarWidget::contactAnimation(684.0, contact, milliseconds).presentation ==
                presentation) return milliseconds;
        }
        return -1;
    }

    static QImage renderDistortedContact(qint64 milliseconds,
                                         std::initializer_list<RadarIconAsset> overlays,
                                         bool includeContact = true)
    {
        RadarWidget widget;
        widget.resize(480, 480);
        widget.setAnimationTimeForTesting(milliseconds);
        auto image = std::make_shared<RadarImage>();
        if (includeContact) {
            RadarContact contact;
            contact.id = 0x1234U;
            contact.scopePosition = QPointF(0.28, -0.18);
            contact.color = QColor(255, 92, 72, 255);
            contact.visibility =
                static_cast<std::uint8_t>(protocol::RadarVisibility::Distorted);
            contact.visual.base = RadarIconAsset::ShipFighter;
            for (const auto overlay : overlays)
                contact.visual.overlays[contact.visual.overlayCount++] = overlay;
            image->contacts.push_back(contact);
        }
        widget.setImage(image);
        widget.setStatus(ClientStatus::Live);
        QImage capture(widget.size(), QImage::Format_ARGB32_Premultiplied);
        capture.fill(Qt::transparent);
        widget.render(&capture);
        return capture;
    }

private slots:
    void squareIsCenteredAndNeverStretched_data()
    {
        QTest::addColumn<QSize>("size");
        QTest::newRow("320") << QSize(320, 320);
        QTest::newRow("720-rectangular") << QSize(900, 720);
        QTest::newRow("1024-portrait") << QSize(1024, 1200);
    }

    void squareIsCenteredAndNeverStretched()
    {
        QFETCH(QSize, size);
        RadarWidget widget;
        widget.resize(size);
        const QRectF radar = widget.radarCircleRect();
        QCOMPARE(radar.width(), radar.height());
        QCOMPARE(radar.center(), QPointF(size.width() / 2.0, size.height() / 2.0));
        QVERIFY(radar.left() >= 0.0);
        QVERIFY(radar.top() >= 0.0);
    }

    void offscreenControlCaptures_data()
    {
        QTest::addColumn<int>("side");
        QTest::newRow("320") << 320;
        QTest::newRow("720") << 720;
        QTest::newRow("1024") << 1024;
    }

    void offscreenControlCaptures()
    {
        QFETCH(int, side);
        RadarWidget widget;
        widget.resize(side, side);
        auto image = std::make_shared<RadarImage>();
        RadarContact contact;
        contact.id = 42;
        contact.scopePosition = QPointF(0.25, -0.2);
        contact.color = QColor(80, 255, 120, 255);
        contact.glyph = ContactGlyph::Square;
        contact.visibility = 2;
        contact.currentTarget = true;
        contact.elevationRadians = 0.2;
        image->contacts.push_back(contact);
        widget.setImage(image);
        widget.setStatus(ClientStatus::Live);
        QImage capture(widget.size(), QImage::Format_ARGB32_Premultiplied);
        capture.fill(Qt::transparent);
        widget.render(&capture);
        QVERIFY(!capture.isNull());
        QVERIFY(qAlpha(capture.pixel(side / 2, side / 2)) > 0);
        QBuffer encoded;
        QVERIFY(encoded.open(QIODevice::WriteOnly));
        QVERIFY(capture.save(&encoded, "PNG"));
        QVERIFY(!encoded.data().isEmpty());
    }

    void enhancedTargetLayersRenderAtAllControlSizes_data()
    {
        QTest::addColumn<int>("side");
        QTest::newRow("320") << 320;
        QTest::newRow("720") << 720;
        QTest::newRow("1024") << 1024;
    }

    void enhancedTargetLayersRenderAtAllControlSizes()
    {
        QFETCH(int, side);
        RadarWidget widget;
        widget.resize(side, side);
        widget.setAnimationTimeForTesting(750);
        auto image = std::make_shared<RadarImage>();
        image->playerEntityId = 1;
        image->currentTargetEntityId = 42;
        image->target.entityId = 42;
        image->target.hasIdentity = true;
        image->target.revealedName = QStringLiteral("GTB Medusa Alpha 2 with an extremely long name");
        image->target.hudTypeLabel = QStringLiteral("Terran bomber");
        image->target.hasHudDistance = true;
        image->target.hudDistance = 809.0;
        image->target.distanceTrend = static_cast<std::uint8_t>(protocol::ValueTrend::Decreasing);
        image->target.hasHudSpeed = true;
        image->target.hudSpeed = 92.0;
        image->target.speedTrend = static_cast<std::uint8_t>(protocol::ValueTrend::Increasing);
        image->target.hasStrength = true;
        image->target.hullRatio = 0.62;
        image->target.hasShields = true;
        image->target.shieldRatio = 0.25;
        image->target.hasHudColor = true;
        image->target.hudColor = QColor(255, 92, 72, 255);
        image->target.targetSubsystemLabel = QStringLiteral("Forward laser turret");
        image->target.hasLead = true;
        image->target.leadScopePosition = QPointF(0.30, -0.16);
        image->sensors.state = static_cast<std::uint8_t>(protocol::SensorState::Degraded);
        image->sensors.hasEmp = true;
        image->sensors.empIntensity = 0.5;
        image->lock.targetEntityId = 42;
        image->lock.hasProgress = true;
        image->lock.progress = 0.5;
        image->lock.hasRemaining = true;
        image->lock.remainingUs = 1'250'000;
        RadarContact contact;
        contact.id = 42;
        contact.currentTarget = true;
        contact.lockTarget = true;
        contact.scopePosition = QPointF(0.36, -0.25);
        contact.predictedScopePosition = QPointF(0.42, -0.22);
        contact.hasPredictedScopePosition = true;
        contact.color = QColor(255, 92, 72, 255);
        contact.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Visible);
        contact.visual.base = RadarIconAsset::ShipBomber;
        image->contacts.push_back(contact);
        RadarContact escort;
        escort.id = 51;
        escort.scopePosition = QPointF(-0.42, -0.10);
        escort.color = QColor(80, 255, 120, 255);
        escort.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Visible);
        escort.visual.base = RadarIconAsset::ShipFighter;
        image->contacts.push_back(escort);
        RadarContact asteroid;
        asteroid.id = 52;
        asteroid.scopePosition = QPointF(-0.18, 0.48);
        asteroid.color = QColor(166, 190, 205, 230);
        asteroid.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::NotVisible);
        asteroid.visual.base = RadarIconAsset::ContactAsteroid;
        image->contacts.push_back(asteroid);
        RadarContact missile;
        missile.id = 53;
        missile.scopePosition = QPointF(0.10, 0.34);
        missile.predictedScopePosition = QPointF(0.06, 0.27);
        missile.hasPredictedScopePosition = true;
        missile.color = QColor(255, 178, 62, 255);
        missile.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Distorted);
        missile.flags = protocol::ContactFlagThreat | protocol::ContactFlagHoming;
        missile.visual.base = RadarIconAsset::WeaponMissile;
        missile.visual.overlays[0] = RadarIconAsset::OverlayHoming;
        missile.visual.overlays[1] = RadarIconAsset::OverlayThreat;
        missile.visual.overlays[2] = RadarIconAsset::OverlayDistorted;
        missile.visual.overlayCount = 3;
        image->contacts.push_back(missile);
        RadarThreat edgeThreat;
        edgeThreat.entityId = 99;
        edgeThreat.scopeDirection = QPointF(-0.72, -0.69);
        edgeThreat.distance = 1'800.0;
        edgeThreat.closingTimeSeconds = 4.2;
        edgeThreat.dangerous = true;
        image->threats.push_back(edgeThreat);
        RadarDisplaySettings settings;
        settings.motionVectors = true;
        settings.trails = true;
        widget.setDisplaySettings(settings);
        QVERIFY(widget.displaySettings() == settings);
        auto previous = std::make_shared<RadarImage>(*image);
        previous->contacts.front().scopePosition = QPointF(0.31, -0.28);
        widget.setImage(previous);
        widget.setImage(image);
        widget.setStatus(ClientStatus::Live);
        QImage capture(widget.size(), QImage::Format_ARGB32_Premultiplied);
        capture.fill(Qt::transparent);
        widget.render(&capture);
        QVERIFY(!capture.isNull());
        QVERIFY(qAlpha(capture.pixel(side / 2, side / 2)) > 0);
        QVERIFY(qAlpha(capture.pixel(4, 4)) > 0);
        const QString screenshotPath = qEnvironmentVariable("RADAR_SCREENSHOT_PATH");
        if (side == 720 && !screenshotPath.isEmpty())
            QVERIFY2(capture.save(screenshotPath), qPrintable(screenshotPath));
    }

    void calloutConnectorHasExactGeometry()
    {
        const QRectF bounds(0.0, 0.0, 720.0, 720.0);
        const CalloutLayout layout = RadarWidget::calculateCalloutLayout(
            QPointF(240.0, 300.0), 18.0, QSizeF(200.0, 82.0), 30.0,
            bounds, true, true);
        QVERIFY(layout.valid);
        QVERIFY(std::abs(layout.diagonal.length() - 24.0) < 0.0001);
        QVERIFY(std::abs(std::abs(layout.diagonal.dx()) -
                         std::abs(layout.diagonal.dy())) < 0.0001);
        QCOMPARE(layout.diagonal.p2(), layout.shoulder.p1());
        QVERIFY(std::abs(layout.shoulder.dy()) < 0.0001);
        QVERIFY(layout.shoulder.length() >= 8.0);
        QVERIFY(bounds.adjusted(4.0, 4.0, -4.0, -4.0).contains(layout.panel));

        const CalloutLayout mirrored = RadarWidget::calculateCalloutLayout(
            QPointF(480.0, 420.0), 18.0, QSizeF(200.0, 82.0), 30.0,
            bounds, false, false);
        QVERIFY(mirrored.valid);
        QVERIFY(std::abs(mirrored.diagonal.length() - 24.0) < 0.0001);
        QVERIFY(mirrored.diagonal.dx() < 0.0);
        QVERIFY(mirrored.diagonal.dy() > 0.0);
        QVERIFY(std::abs(mirrored.shoulder.dy()) < 0.0001);
    }

    void targetPlacementUsesDeadZoneDelayAndChevronHysteresis()
    {
        RadarWidget widget;
        widget.resize(720, 720);
        const auto makeImage = [](double scopeX, double elevation, double azimuth,
                                  std::uint64_t session = 1U,
                                  std::uint64_t targetId = 42U) {
            auto image = std::make_shared<RadarImage>();
            image->sessionId = session;
            image->target.entityId = targetId;
            image->target.revealedName = QStringLiteral("Target");
            RadarContact contact;
            contact.id = targetId;
            contact.currentTarget = true;
            contact.scopePosition = QPointF(scopeX, 0.0);
            contact.elevationRadians = elevation;
            contact.azimuthRadians = azimuth;
            contact.color = QColor(255, 80, 70);
            contact.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Visible);
            contact.visual.base = RadarIconAsset::ShipFighter;
            image->contacts.push_back(contact);
            return image;
        };

        widget.setAnimationTimeForTesting(0);
        widget.setImage(makeImage(-0.10, 0.06, -0.06));
        QVERIFY(widget.targetDisplayState().panelRight);
        QCOMPARE(widget.targetDisplayState().verticalChevron, 1);
        QCOMPARE(widget.targetDisplayState().horizontalChevron, -1);

        widget.setAnimationTimeForTesting(100);
        widget.setImage(makeImage(0.19, 0.04, -0.04));
        QVERIFY(widget.targetDisplayState().panelRight);
        QCOMPARE(widget.targetDisplayState().verticalChevron, 1);
        QCOMPARE(widget.targetDisplayState().horizontalChevron, -1);

        widget.setAnimationTimeForTesting(200);
        widget.setImage(makeImage(0.30, 0.02, -0.02));
        QVERIFY(widget.targetDisplayState().panelRight);
        QCOMPARE(widget.targetDisplayState().verticalChevron, 0);
        QCOMPARE(widget.targetDisplayState().horizontalChevron, 0);
        widget.setAnimationTimeForTesting(799);
        widget.setImage(makeImage(0.30, 0.0, 0.0));
        QVERIFY(widget.targetDisplayState().panelRight);
        widget.setAnimationTimeForTesting(800);
        widget.setImage(makeImage(0.30, 0.0, 0.0));
        QVERIFY(!widget.targetDisplayState().panelRight);

        widget.setAnimationTimeForTesting(900);
        widget.setImage(makeImage(-0.30, -0.06, 0.06, 2U, 77U));
        QVERIFY(widget.targetDisplayState().panelRight);
        QCOMPARE(widget.targetDisplayState().verticalChevron, -1);
        QCOMPARE(widget.targetDisplayState().horizontalChevron, 1);
    }

    void calloutSwitchesImmediatelyWhenRetainedSideCannotFit()
    {
        RadarWidget widget;
        widget.resize(720, 720);
        widget.setAnimationTimeForTesting(0);
        auto image = std::make_shared<RadarImage>();
        image->sessionId = 1;
        image->target.entityId = 42;
        image->target.revealedName = QStringLiteral("Target");
        RadarContact contact;
        contact.id = 42;
        contact.currentTarget = true;
        contact.scopePosition = QPointF(-0.10, 0.0);
        contact.color = QColor(255, 80, 70);
        contact.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Visible);
        contact.visual.base = RadarIconAsset::ShipFighter;
        image->contacts.push_back(contact);
        widget.setImage(image);
        QVERIFY(widget.targetDisplayState().panelRight);

        auto atEdge = std::make_shared<RadarImage>(*image);
        atEdge->contacts.front().scopePosition = QPointF(0.98, 0.0);
        widget.setAnimationTimeForTesting(100);
        widget.setImage(atEdge);
        QVERIFY(widget.targetDisplayState().panelRight);
        QImage capture(widget.size(), QImage::Format_ARGB32_Premultiplied);
        capture.fill(Qt::transparent);
        widget.render(&capture);
        QVERIFY(!widget.targetDisplayState().panelRight);
    }

    void scopeHasNoOrientationLabelsOrPlayerMarker()
    {
        RadarWidget widget;
        widget.resize(720, 720);
        auto image = std::make_shared<RadarImage>();
        image->playerEntityId = 1;
        RadarContact playerContact;
        playerContact.id = 1;
        playerContact.scopePosition = QPointF();
        playerContact.color = QColor(255, 0, 255);
        playerContact.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Visible);
        playerContact.visual.base = RadarIconAsset::ShipFighter;
        image->contacts.push_back(playerContact);
        widget.setImage(image);
        widget.setStatus(ClientStatus::Live);
        QImage capture(widget.size(), QImage::Format_ARGB32_Premultiplied);
        capture.fill(Qt::transparent);
        widget.render(&capture);
        const QColor center = capture.pixelColor(360, 360);
        for (int y = 356; y <= 364; ++y)
            for (int x = 356; x <= 364; ++x)
                QCOMPARE(capture.pixelColor(x, y), center);
        bool sawLabelColor = false;
        bool sawPlayerColor = false;
        for (int y = 0; y < capture.height() && !sawLabelColor; ++y) {
            for (int x = 0; x < capture.width(); ++x) {
                const QColor pixel = capture.pixelColor(x, y);
                if (pixel.red() > 195 && pixel.green() > 235 && pixel.blue() > 225) {
                    sawLabelColor = true;
                    break;
                }
                if (pixel.red() > 240 && pixel.green() < 40 && pixel.blue() > 240)
                    sawPlayerColor = true;
            }
        }
        QVERIFY(!sawLabelColor);
        QVERIFY(!sawPlayerColor);
    }

    void vfntOverlayFontsLoadAndCanBeTinted()
    {
        VfntFont title(QStringLiteral(":/radar/fonts/font02.vf"));
        VfntFont detail(QStringLiteral(":/radar/fonts/font01.vf"));
        QVERIFY(title.isValid());
        QVERIFY(detail.isValid());
        QCOMPARE(title.nativeHeight(), 18);
        QCOMPARE(detail.nativeHeight(), 9);
        QVERIFY(title.textSize(QStringLiteral("SENSOR LINK"), 2).width() > 0);

        QImage capture(QSize(360, 80), QImage::Format_ARGB32_Premultiplied);
        capture.fill(Qt::transparent);
        QPainter painter(&capture);
        const QColor tint(QStringLiteral("#66D9E8"));
        QVERIFY(title.drawText(painter, capture.rect(), Qt::AlignCenter,
                               QStringLiteral("SENSOR LINK"), tint, 2));
        painter.end();
        bool sawTint = false;
        for (int y = 0; y < capture.height() && !sawTint; ++y) {
            for (int x = 0; x < capture.width(); ++x) {
                const QColor pixel = capture.pixelColor(x, y);
                if (pixel.alpha() > 0 && pixel.red() == tint.red() &&
                    pixel.green() == tint.green() && pixel.blue() == tint.blue()) {
                    sawTint = true;
                    break;
                }
            }
        }
        QVERIFY(sawTint);
    }

    void systemOverlaysUseImmersiveCopyAndStateColors()
    {
        const auto connecting = RadarWidget::systemOverlay(ClientStatus::Connecting);
        QVERIFY(connecting.visible);
        QCOMPARE(connecting.title, QStringLiteral("ESTABLISHING SENSOR LINK"));
        QCOMPARE(connecting.detail, QStringLiteral("STANDBY"));
        QCOMPARE(connecting.color, QColor(QStringLiteral("#66D9E8")));

        const auto synchronizing = RadarWidget::systemOverlay(ClientStatus::Synchronizing);
        QCOMPARE(synchronizing.title, QStringLiteral("BUILDING TACTICAL PICTURE"));
        const auto stale = RadarWidget::systemOverlay(ClientStatus::Stale);
        QCOMPARE(stale.title, QStringLiteral("SENSOR FEED LOST"));
        QCOMPARE(stale.detail, QStringLiteral("HOLDING LAST CONTACT PICTURE"));
        QCOMPARE(stale.color, QColor(QStringLiteral("#FFB83D")));
        const auto reconnecting = RadarWidget::systemOverlay(ClientStatus::Reconnecting);
        QCOMPARE(reconnecting.title, QStringLiteral("REACQUIRING SENSOR LINK"));
        const auto failure = RadarWidget::systemOverlay(ClientStatus::Error);
        QCOMPARE(failure.title, QStringLiteral("SENSOR LINK FAILURE"));
        QCOMPARE(failure.detail, QStringLiteral("CHECK TELEMETRY SOURCE"));
        QCOMPARE(failure.color, QColor(QStringLiteral("#FF5C57")));
        QVERIFY(!RadarWidget::systemOverlay(ClientStatus::Live).visible);
    }

    void technicalDetailsAreNotPaintedOnSystemOverlay()
    {
        RadarWidget widget;
        widget.resize(720, 720);
        widget.setAnimationTimeForTesting(500);
        widget.setStatus(ClientStatus::Error, QStringLiteral("UDP CRC snapshot failure"));
        QImage first(widget.size(), QImage::Format_ARGB32_Premultiplied);
        first.fill(Qt::transparent);
        widget.render(&first);

        widget.setStatus(ClientStatus::Error, QStringLiteral("Different internal diagnostic"));
        QImage second(widget.size(), QImage::Format_ARGB32_Premultiplied);
        second.fill(Qt::transparent);
        widget.render(&second);
        QCOMPARE(first, second);

        bool sawFailureColor = false;
        for (int y = 0; y < first.height() && !sawFailureColor; ++y) {
            for (int x = 0; x < first.width(); ++x) {
                const QColor pixel = first.pixelColor(x, y);
                if (pixel.alpha() > 0 && pixel.red() > 180 &&
                    pixel.red() > pixel.green() * 2 &&
                    pixel.red() > pixel.blue() * 2) {
                    sawFailureColor = true;
                    break;
                }
            }
        }
        QVERIFY(sawFailureColor);
    }

    void leadAndTargetChevronsUseFixedColors()
    {
        RadarWidget widget;
        widget.resize(720, 720);
        auto image = std::make_shared<RadarImage>();
        image->sessionId = 1;
        image->target.entityId = 42;
        image->target.revealedName = QStringLiteral("Target");
        image->target.hasLead = true;
        image->target.leadScopePosition = QPointF(0.42, 0.0);
        RadarContact target;
        target.id = 42;
        target.currentTarget = true;
        target.scopePosition = QPointF(0.20, 0.0);
        target.elevationRadians = 0.08;
        target.azimuthRadians = 0.08;
        target.color = QColor(255, 70, 60);
        target.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Visible);
        target.visual.base = RadarIconAsset::ShipFighter;
        image->contacts.push_back(target);
        RadarContact ordinary = target;
        ordinary.id = 51;
        ordinary.currentTarget = false;
        ordinary.scopePosition = QPointF(-0.40, 0.20);
        ordinary.color = QColor(60, 255, 100);
        image->contacts.push_back(ordinary);
        widget.setImage(image);
        widget.setStatus(ClientStatus::Live);
        QImage capture(widget.size(), QImage::Format_ARGB32_Premultiplied);
        capture.fill(Qt::transparent);
        widget.render(&capture);

        bool sawYellow = false;
        bool sawWhiteNearTarget = false;
        const QPointF center = widget.radarCircleRect().center();
        const double radius = widget.radarCircleRect().width() * 0.5;
        const QPointF targetPixel = center + QPointF(0.20 * radius, 0.0);
        const QPointF ordinaryPixel = center + QPointF(-0.40 * radius, 0.20 * radius);
        for (int y = 0; y < capture.height(); ++y) {
            for (int x = 0; x < capture.width(); ++x) {
                const QColor pixel = capture.pixelColor(x, y);
                if (pixel.red() > 245 && pixel.green() >= 205 &&
                    pixel.green() <= 230 && pixel.blue() < 100) sawYellow = true;
                if (std::abs(x - targetPixel.x()) < 32.0 &&
                    std::abs(y - targetPixel.y()) < 32.0 &&
                    pixel.red() > 248 && pixel.green() > 248 && pixel.blue() > 248)
                    sawWhiteNearTarget = true;
            }
        }
        QVERIFY(sawYellow);
        QVERIFY(sawWhiteNearTarget);
        const auto hasWhiteNear = [&capture](const QPointF& point, int radiusPixels) {
            for (int y = qFloor(point.y()) - radiusPixels;
                 y <= qCeil(point.y()) + radiusPixels; ++y) {
                for (int x = qFloor(point.x()) - radiusPixels;
                     x <= qCeil(point.x()) + radiusPixels; ++x) {
                    const QColor pixel = capture.pixelColor(x, y);
                    if (pixel.red() > 248 && pixel.green() > 248 && pixel.blue() > 248)
                        return true;
                }
            }
            return false;
        };
        QVERIFY(hasWhiteNear(targetPixel + QPointF(0.0, -15.0), 2));
        QVERIFY(hasWhiteNear(targetPixel + QPointF(15.0, 0.0), 2));
        QVERIFY(!hasWhiteNear(ordinaryPixel + QPointF(0.0, -15.0), 2));
    }

    void settingsDialogIsEnglishOnly()
    {
        SettingsDialog dialog(QStringLiteral("127.0.0.1"), 42042, RadarDisplaySettings{});
        QCOMPARE(dialog.windowTitle(), QStringLiteral("Radar connection"));
        QStringList visibleText;
        for (const auto* label : dialog.findChildren<QLabel*>()) visibleText << label->text();
        for (const auto* group : dialog.findChildren<QGroupBox*>()) visibleText << group->title();
        for (const auto* box : dialog.findChildren<QCheckBox*>()) visibleText << box->text();
        for (const auto* button : dialog.findChildren<QPushButton*>()) visibleText << button->text();
        const QString joined = visibleText.join(QLatin1Char('\n'));
        QVERIFY(joined.contains(QStringLiteral("Direct connection to FS2Open")));
        QVERIFY(joined.contains(QStringLiteral("Enhanced display")));
        QVERIFY(joined.contains(QStringLiteral("Hull and shields")));
        QVERIFY(joined.contains(QStringLiteral("Connect")));
        QVERIFY(!joined.contains(QStringLiteral("Connexion")));
        QVERIFY(!joined.contains(QStringLiteral("Coque")));
    }

    void enhancedRadarRendersAtDprTwo()
    {
        RadarWidget widget;
        widget.resize(720, 720);
        auto image = std::make_shared<RadarImage>();
        image->sessionId = 1;
        image->target.entityId = 42;
        image->target.revealedName = QStringLiteral("Target");
        image->target.hasLead = true;
        image->target.leadScopePosition = QPointF(0.22, -0.12);
        RadarContact target;
        target.id = 42;
        target.currentTarget = true;
        target.scopePosition = QPointF(0.10, -0.08);
        target.elevationRadians = 0.08;
        target.azimuthRadians = -0.08;
        target.color = QColor(255, 70, 60);
        target.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Visible);
        target.visual.base = RadarIconAsset::ShipFighter;
        image->contacts.push_back(target);
        widget.setImage(image);
        widget.setStatus(ClientStatus::Live);

        QImage capture(QSize(1440, 1440), QImage::Format_ARGB32_Premultiplied);
        capture.setDevicePixelRatio(2.0);
        capture.fill(Qt::transparent);
        widget.render(&capture);
        QCOMPARE(capture.devicePixelRatio(), 2.0);
        QVERIFY(qAlpha(capture.pixel(720, 720)) > 0);
        bool sawYellow = false;
        for (int y = 0; y < capture.height() && !sawYellow; ++y) {
            for (int x = 0; x < capture.width(); ++x) {
                const QColor pixel = capture.pixelColor(x, y);
                if (pixel.red() > 245 && pixel.green() >= 205 &&
                    pixel.green() <= 230 && pixel.blue() < 100) {
                    sawYellow = true;
                    break;
                }
            }
        }
        QVERIFY(sawYellow);
    }

    void allSvgResourcesAreValidAndNonEmpty()
    {
        SvgIconCache cache;
        QCOMPARE(allRadarIconAssets().size(), RadarIconAssetCount);
        for (const RadarIconAsset asset : allRadarIconAssets()) {
            QVERIFY2(cache.resourceIsValid(asset), radarIconResourcePath(asset));
            QFile file(QString::fromLatin1(radarIconResourcePath(asset)));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QByteArray source = file.readAll().toLower();
            QVERIFY(!source.contains("<image"));
            QVERIFY(!source.contains("<script"));
            QVERIFY(!source.contains("href="));
            QVERIFY(!source.contains("url("));
        }
        QVERIFY(allRadarIconAssets().end() !=
                std::find(allRadarIconAssets().begin(), allRadarIconAssets().end(),
                          RadarIconAsset::WeaponMine));
    }

    void adaptiveSizesRespectBounds()
    {
        RadarContact contact;
        QCOMPARE(RadarWidget::contactIconSize(320.0, contact), 12.8);
        QCOMPARE(RadarWidget::contactIconSize(684.0, contact), 16.0);
        contact.flags = protocol::ContactFlagThreat;
        QCOMPARE(RadarWidget::contactIconSize(684.0, contact), 18.0);
        contact.currentTarget = true;
        QCOMPARE(RadarWidget::contactIconSize(684.0, contact), 20.0);
        QCOMPARE(RadarWidget::contactIconSize(4096.0, contact), 28.0);
    }

    void deterministicAnimationStaysWithinBounds()
    {
        RadarContact contact;
        contact.id = 1234;
        contact.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Distorted);
        contact.flags = protocol::ContactFlagThreat;
        const auto first = RadarWidget::contactAnimation(684.0, contact, 250);
        const auto repeat = RadarWidget::contactAnimation(684.0, contact, 250);
        QCOMPARE(first.jitter, repeat.jitter);
        QCOMPARE(first.sizeMultiplier, repeat.sizeMultiplier);
        QCOMPARE(first.presentation, repeat.presentation);
        QVERIFY(std::abs(first.jitter.x()) <= 1.25);
        QVERIFY(std::abs(first.jitter.y()) <= 1.25);
        QVERIFY(first.sizeMultiplier >= 1.0 && first.sizeMultiplier <= 1.12);
    }

    void distortedPresentationIsReadableIrregularAndIndependent()
    {
        RadarContact normal;
        normal.id = 1234;
        normal.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Visible);
        for (qint64 interval = 0; interval < 100; ++interval)
            QCOMPARE(RadarWidget::contactAnimation(684.0, normal, interval * 120).presentation,
                     DistortedContactPresentation::ContactAndOverlay);

        RadarContact first;
        first.id = 1234;
        first.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Distorted);
        RadarContact second = first;
        second.id = 5678;
        int contactAndOverlay = 0;
        int contactOnly = 0;
        int overlayOnly = 0;
        int hidden = 0;
        int independent = 0;
        for (qint64 interval = 0; interval < 10'000; ++interval) {
            const qint64 milliseconds = interval * 120;
            const auto state = RadarWidget::contactAnimation(684.0, first, milliseconds);
            QCOMPARE(state.presentation,
                     RadarWidget::contactAnimation(684.0, first, milliseconds).presentation);
            switch (state.presentation) {
            case DistortedContactPresentation::ContactAndOverlay:
                ++contactAndOverlay;
                break;
            case DistortedContactPresentation::ContactOnly: ++contactOnly; break;
            case DistortedContactPresentation::OverlayOnly: ++overlayOnly; break;
            case DistortedContactPresentation::Hidden: ++hidden; break;
            }
            if (state.presentation !=
                RadarWidget::contactAnimation(684.0, second, milliseconds).presentation)
                ++independent;
        }
        QVERIFY(contactAndOverlay >= 5'000 && contactAndOverlay <= 5'400);
        QVERIFY(contactOnly >= 2'200 && contactOnly <= 2'600);
        QVERIFY(overlayOnly >= 1'400 && overlayOnly <= 1'800);
        QVERIFY(hidden >= 650 && hidden <= 950);
        QVERIFY(independent > 1'000);
    }

    void distortedPresentationControlsMarkerRendering()
    {
        const qint64 fullTime = firstPresentationTime(
            0x1234U, DistortedContactPresentation::ContactAndOverlay);
        const qint64 contactOnlyTime = firstPresentationTime(
            0x1234U, DistortedContactPresentation::ContactOnly);
        const qint64 overlayOnlyTime = firstPresentationTime(
            0x1234U, DistortedContactPresentation::OverlayOnly);
        const qint64 hiddenTime = firstPresentationTime(
            0x1234U, DistortedContactPresentation::Hidden);
        QVERIFY(fullTime >= 0);
        QVERIFY(contactOnlyTime >= 0);
        QVERIFY(overlayOnlyTime >= 0);
        QVERIFY(hiddenTime >= 0);

        const auto full = renderDistortedContact(fullTime,
            {RadarIconAsset::OverlayTagged, RadarIconAsset::OverlayDistorted});
        const auto fullWithoutDistortion = renderDistortedContact(fullTime,
            {RadarIconAsset::OverlayTagged});
        QVERIFY(full != fullWithoutDistortion);

        const auto contactOnly = renderDistortedContact(contactOnlyTime,
            {RadarIconAsset::OverlayTagged, RadarIconAsset::OverlayDistorted});
        const auto taggedOnly = renderDistortedContact(contactOnlyTime,
            {RadarIconAsset::OverlayTagged});
        const auto noOverlay = renderDistortedContact(contactOnlyTime, {});
        QCOMPARE(contactOnly, taggedOnly);
        QVERIFY(contactOnly != noOverlay);

        const auto overlayOnly = renderDistortedContact(overlayOnlyTime,
            {RadarIconAsset::OverlayTagged, RadarIconAsset::OverlayDistorted});
        const auto distortionOnly = renderDistortedContact(overlayOnlyTime,
            {RadarIconAsset::OverlayDistorted});
        const auto overlayOnlyEmpty = renderDistortedContact(overlayOnlyTime, {}, false);
        QCOMPARE(overlayOnly, distortionOnly);
        QVERIFY(overlayOnly != overlayOnlyEmpty);

        const auto hidden = renderDistortedContact(hiddenTime,
            {RadarIconAsset::OverlayTagged, RadarIconAsset::OverlayDistorted});
        const auto empty = renderDistortedContact(hiddenTime, {}, false);
        QCOMPARE(hidden, empty);
    }

    void dprTwoRendersColorizedSvg()
    {
        QImage capture(QSize(144, 144), QImage::Format_ARGB32_Premultiplied);
        capture.setDevicePixelRatio(2.0);
        capture.fill(Qt::transparent);
        QPainter painter(&capture);
        SvgIconCache cache;
        const QColor iff(37, 211, 94, 255);
        QVERIFY(cache.draw(painter, RadarIconAsset::ShipFighter, QPointF(36, 36), 24, iff, 2.0));
        painter.end();
        bool sawColor = false;
        for (int y = 0; y < capture.height() && !sawColor; ++y) {
            for (int x = 0; x < capture.width(); ++x) {
                const QColor pixel = QColor::fromRgba(capture.pixel(x, y));
                if (pixel.alpha() > 0 && pixel.green() > pixel.red() && pixel.green() > pixel.blue()) {
                    sawColor = true;
                    break;
                }
            }
        }
        QVERIFY(sawColor);
    }
};

QTEST_MAIN(RadarWidgetTests)
#include "radar_widget_tests.moc"

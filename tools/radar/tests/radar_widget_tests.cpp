#include "radar_widget.h"
#include "radar_icons.h"
#include "svg_icon_cache.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <QImage>
#include <QFile>
#include <QBuffer>
#include <QPainter>
#include <QTest>

#include <algorithm>
#include <cmath>

using namespace simpit::radar;
namespace protocol = telemetry::protocol;

class RadarWidgetTests final : public QObject {
    Q_OBJECT
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
        QCOMPARE(first.opacity, repeat.opacity);
        QCOMPARE(first.sizeMultiplier, repeat.sizeMultiplier);
        QVERIFY(std::abs(first.jitter.x()) <= 1.25);
        QVERIFY(std::abs(first.jitter.y()) <= 1.25);
        QVERIFY(first.opacity >= 0.70 && first.opacity <= 1.0);
        QVERIFY(first.sizeMultiplier >= 1.0 && first.sizeMultiplier <= 1.12);
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

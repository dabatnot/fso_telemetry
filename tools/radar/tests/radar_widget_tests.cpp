#include "radar_widget.h"

#include <QImage>
#include <QPainter>
#include <QTest>

using namespace simpit::radar;

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
        QVERIFY(capture.save(QStringLiteral("radar-control-%1.png").arg(side)));
    }
};

QTEST_MAIN(RadarWidgetTests)
#include "radar_widget_tests.moc"


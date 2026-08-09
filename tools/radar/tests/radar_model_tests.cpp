#include "radar_image.h"

#include <QTest>

#include <cmath>

using namespace simpit::radar;

class RadarModelTests final : public QObject {
    Q_OBJECT
private slots:
    void projectionUsesDashboardAngularFormula()
    {
        bool defined = false;
        const QPointF right = projectContact(1.0, 0.0, 0.0, 1.0, &defined);
        QVERIFY(defined);
        QCOMPARE(right.x(), 0.5);
        QCOMPARE(right.y(), 0.0);
        const QPointF upper = projectContact(0.0, 1.0, 0.0, 1.0);
        QCOMPARE(upper.x(), 0.0);
        QCOMPARE(upper.y(), -0.5);
    }

    void projectionCentersUndefinedTransverseDirection()
    {
        bool defined = true;
        QCOMPARE(projectContact(0.0, 0.0, 1.0, 1.0, &defined), QPointF());
        QVERIFY(!defined);
        QCOMPARE(projectContact(0.001, 0.001, -1.0, 1.0, &defined), QPointF());
        QVERIFY(!defined);
    }

    void projectionRejectsInvalidInputs()
    {
        QCOMPARE(projectContact(1.0, 0.0, 0.0, 0.0), QPointF());
        QCOMPARE(projectContact(std::nan(""), 0.0, 0.0, 1.0), QPointF());
    }

    void visibilityAlphaMatchesDashboard()
    {
        RadarContact contact;
        contact.visibility = 0; QCOMPARE(contact.alpha(), 0.35);
        contact.visibility = 1; QCOMPARE(contact.alpha(), 1.0);
        contact.visibility = 2; QCOMPARE(contact.alpha(), 0.6);
    }

    void glyphMappingMatchesDashboard()
    {
        QCOMPARE(contactGlyph(1, 0), ContactGlyph::Square);
        QCOMPARE(contactGlyph(5, 0), ContactGlyph::Circle);
        QCOMPARE(contactGlyph(6, 0), ContactGlyph::Circle);
        QCOMPARE(contactGlyph(1, 0x20), ContactGlyph::Diamond);
        QCOMPARE(contactGlyph(2, 0x40), ContactGlyph::Diamond);
        QCOMPARE(contactGlyph(7, 0), ContactGlyph::Triangle);
    }

    void priorityMatchesDashboardOrdering()
    {
        RadarContact ordinary; ordinary.id = 1; ordinary.visibility = 1; ordinary.distance = 10;
        RadarContact locked = ordinary; locked.id = 2; locked.lockTarget = true;
        RadarContact threat = ordinary; threat.id = 3; threat.flags = 0x80;
        RadarContact target = ordinary; target.id = 4; target.currentTarget = true;
        QVERIFY(target.priority() > threat.priority());
        QVERIFY(threat.priority() > locked.priority());
        QVERIFY(locked.priority() > ordinary.priority());
    }
};

QTEST_APPLESS_MAIN(RadarModelTests)
#include "radar_model_tests.moc"


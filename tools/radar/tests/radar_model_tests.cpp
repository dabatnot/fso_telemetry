#include "radar_image.h"
#include "radar_icons.h"
#include "radar_display_settings.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <QTest>

#include <array>
#include <cmath>
#include <utility>

using namespace simpit::radar;
namespace protocol = telemetry::protocol;

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

    void enhancedSettingsDefaultsAreFailSafe()
    {
        const RadarDisplaySettings settings;
        QVERIFY(settings.targetCallout);
        QVERIFY(settings.targetStrength);
        QVERIFY(settings.lead);
        QVERIFY(settings.lock);
        QVERIFY(settings.subsystems);
        QVERIFY(settings.edgeThreats);
        QVERIFY(settings.sensorEffects);
        QVERIFY(!settings.motionVectors);
        QVERIFY(!settings.trails);
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

    void azimuthUsesPlayerLocalDirection()
    {
        QCOMPARE(contactAzimuthRadians(0.0, 0.0, 1.0), 0.0);
        QVERIFY(contactAzimuthRadians(1.0, 0.0, 1.0) > 0.0);
        QVERIFY(contactAzimuthRadians(-1.0, 0.0, 1.0) < 0.0);
        QCOMPARE(contactAzimuthRadians(1.0, 0.0, 0.0), std::acos(-1.0) * 0.5);
        QVERIFY(std::isnan(contactAzimuthRadians(std::nan(""), 0.0, 1.0)));
    }

    void visibilityAlphaMatchesDashboard()
    {
        RadarContact contact;
        contact.flags = protocol::ContactFlagBright;
        contact.visibility = 0; QCOMPARE(contact.alpha(), 0.35);
        contact.visibility = 1; QCOMPARE(contact.alpha(), 1.0);
        contact.visibility = 2; QCOMPARE(contact.alpha(), 0.6);
        contact.flags = 0;
        contact.visibility = 1; QCOMPARE(contact.alpha(), 0.72);
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

    void resolvesAllBuiltInShipFamilies()
    {
        const std::array<RadarIconAsset, 17> expected{
            RadarIconAsset::ShipNavbuoy, RadarIconAsset::ShipSentryGun,
            RadarIconAsset::ShipEscapePod, RadarIconAsset::ShipCargo,
            RadarIconAsset::ShipSupport, RadarIconAsset::ShipFighter,
            RadarIconAsset::ShipBomber, RadarIconAsset::ShipTransport,
            RadarIconAsset::ShipFreighter, RadarIconAsset::ShipAwacs,
            RadarIconAsset::ShipGasMiner, RadarIconAsset::ShipCruiser,
            RadarIconAsset::ShipCorvette, RadarIconAsset::ShipCapital,
            RadarIconAsset::ShipSuperCapital, RadarIconAsset::ShipDrydock,
            RadarIconAsset::ShipKnossosDevice};
        RadarManifestCatalog catalog;
        RadarVisualInput input;
        input.objectType = static_cast<std::uint8_t>(protocol::ObjectType::Ship);
        input.category = static_cast<std::uint8_t>(protocol::RadarCategory::Ship);
        input.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Visible);
        input.hasRevealedClass = true;
        for (std::uint32_t id = 1; id <= expected.size(); ++id) {
            input.revealedClassId = 100 + id;
            catalog.shipRadarIconIds[input.revealedClassId] = id;
            QCOMPARE(resolveRadarVisual(input, &catalog).base, expected[id - 1]);
        }
        catalog.shipRadarIconIds[input.revealedClassId] = 9876;
        QCOMPARE(resolveRadarVisual(input, &catalog).base, RadarIconAsset::ContactShip);
    }

    void neverLeaksAnUnrevealedClass()
    {
        RadarManifestCatalog catalog;
        catalog.shipRadarIconIds[44] =
            static_cast<std::uint32_t>(protocol::BuiltinRadarIconId::SuperCapital);
        RadarVisualInput input;
        input.objectType = static_cast<std::uint8_t>(protocol::ObjectType::Ship);
        input.category = static_cast<std::uint8_t>(protocol::RadarCategory::Ship);
        input.revealedClassId = 44;
        input.hasRevealedClass = false;
        QCOMPARE(resolveRadarVisual(input, &catalog).base, RadarIconAsset::ContactShip);
        input.objectType = static_cast<std::uint8_t>(protocol::ObjectType::Unknown);
        QCOMPARE(resolveRadarVisual(input, &catalog).base, RadarIconAsset::ContactUnknown);
    }

    void resolvesWeaponsCategoriesAndLayerOrder()
    {
        RadarManifestCatalog catalog;
        RadarVisualInput input;
        input.objectType = static_cast<std::uint8_t>(protocol::ObjectType::Weapon);
        input.category = static_cast<std::uint8_t>(protocol::RadarCategory::Weapon);
        input.hasRevealedClass = true;
        input.revealedClassId = 8;
        const std::array<RadarIconAsset, 6> expected{
            RadarIconAsset::ContactWeapon, RadarIconAsset::WeaponPrimary,
            RadarIconAsset::WeaponMissile, RadarIconAsset::WeaponBeam,
            RadarIconAsset::WeaponCountermeasure, RadarIconAsset::WeaponSpecial};
        for (std::uint8_t subtype = 0; subtype < expected.size(); ++subtype) {
            catalog.weapons[8].subtype = static_cast<protocol::WeaponSubtype>(subtype);
            QCOMPARE(resolveRadarVisual(input, &catalog).base, expected[subtype]);
        }
        input.flags = protocol::ContactFlagBomb | protocol::ContactFlagWarp |
                      protocol::ContactFlagTagged | protocol::ContactFlagStealth |
                      protocol::ContactFlagHoming | protocol::ContactFlagThreat;
        input.visibility = static_cast<std::uint8_t>(protocol::RadarVisibility::Distorted);
        const auto visual = resolveRadarVisual(input, &catalog);
        QCOMPARE(visual.base, RadarIconAsset::WeaponBomb);
        QCOMPARE(visual.behind, RadarIconAsset::OverlayWarp);
        QCOMPARE(visual.overlayCount, std::uint8_t{5});
        QCOMPARE(visual.overlays[0], RadarIconAsset::OverlayTagged);
        QCOMPARE(visual.overlays[1], RadarIconAsset::OverlayStealth);
        QCOMPARE(visual.overlays[2], RadarIconAsset::OverlayHoming);
        QCOMPARE(visual.overlays[3], RadarIconAsset::OverlayThreat);
        QCOMPARE(visual.overlays[4], RadarIconAsset::OverlayDistorted);
        for (const auto asset : visual.overlays)
            QVERIFY(asset != RadarIconAsset::WeaponMine);

        input = {};
        const std::array<std::pair<protocol::ObjectType, RadarIconAsset>, 6> categories{{
            {protocol::ObjectType::Waypoint, RadarIconAsset::ContactNavigation},
            {protocol::ObjectType::JumpNode, RadarIconAsset::ContactJumpNode},
            {protocol::ObjectType::Asteroid, RadarIconAsset::ContactAsteroid},
            {protocol::ObjectType::Debris, RadarIconAsset::ContactDebris},
            {protocol::ObjectType::Fireball, RadarIconAsset::ContactFireball},
            {protocol::ObjectType::Other, RadarIconAsset::ContactOther}}};
        input.category = static_cast<std::uint8_t>(protocol::RadarCategory::Other);
        for (const auto& [type, asset] : categories) {
            input.objectType = static_cast<std::uint8_t>(type);
            QCOMPARE(resolveRadarVisual(input, nullptr).base, asset);
        }
    }
};

QTEST_APPLESS_MAIN(RadarModelTests)
#include "radar_model_tests.moc"

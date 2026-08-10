#include "radar_icons.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

namespace simpit::radar {
namespace protocol = telemetry::protocol;
namespace {

RadarIconAsset shipAsset(std::uint32_t id) noexcept
{
    using Id = protocol::BuiltinRadarIconId;
    switch (static_cast<Id>(id)) {
    case Id::Navbuoy: return RadarIconAsset::ShipNavbuoy;
    case Id::SentryGun: return RadarIconAsset::ShipSentryGun;
    case Id::EscapePod: return RadarIconAsset::ShipEscapePod;
    case Id::Cargo: return RadarIconAsset::ShipCargo;
    case Id::Support: return RadarIconAsset::ShipSupport;
    case Id::Fighter: return RadarIconAsset::ShipFighter;
    case Id::Bomber: return RadarIconAsset::ShipBomber;
    case Id::Transport: return RadarIconAsset::ShipTransport;
    case Id::Freighter: return RadarIconAsset::ShipFreighter;
    case Id::Awacs: return RadarIconAsset::ShipAwacs;
    case Id::GasMiner: return RadarIconAsset::ShipGasMiner;
    case Id::Cruiser: return RadarIconAsset::ShipCruiser;
    case Id::Corvette: return RadarIconAsset::ShipCorvette;
    case Id::Capital: return RadarIconAsset::ShipCapital;
    case Id::SuperCapital: return RadarIconAsset::ShipSuperCapital;
    case Id::Drydock: return RadarIconAsset::ShipDrydock;
    case Id::KnossosDevice: return RadarIconAsset::ShipKnossosDevice;
    case Id::Generic: break;
    }
    return RadarIconAsset::ContactShip;
}

RadarIconAsset weaponAsset(protocol::WeaponSubtype subtype) noexcept
{
    switch (subtype) {
    case protocol::WeaponSubtype::Primary: return RadarIconAsset::WeaponPrimary;
    case protocol::WeaponSubtype::Missile: return RadarIconAsset::WeaponMissile;
    case protocol::WeaponSubtype::Beam: return RadarIconAsset::WeaponBeam;
    case protocol::WeaponSubtype::Countermeasure: return RadarIconAsset::WeaponCountermeasure;
    case protocol::WeaponSubtype::Special: return RadarIconAsset::WeaponSpecial;
    case protocol::WeaponSubtype::Unknown: break;
    }
    return RadarIconAsset::ContactWeapon;
}

void appendOverlay(RadarVisualDescriptor& descriptor, RadarIconAsset asset) noexcept
{
    if (descriptor.overlayCount < descriptor.overlays.size())
        descriptor.overlays[descriptor.overlayCount++] = asset;
}

} // namespace

const char* radarIconResourcePath(RadarIconAsset asset) noexcept
{
    static constexpr const char* Paths[] = {
        "", ":/radar/icons/brackets-crosshair.svg", ":/radar/icons/brackets-lock.svg",
        ":/radar/icons/brackets-selected.svg", ":/radar/icons/brackets-selected-compact.svg",
        ":/radar/icons/brackets-selected-large.svg", ":/radar/icons/contact-asteroid.svg",
        ":/radar/icons/contact-debris.svg", ":/radar/icons/contact-fireball.svg",
        ":/radar/icons/contact-jump-node.svg", ":/radar/icons/contact-navigation.svg",
        ":/radar/icons/contact-other.svg", ":/radar/icons/contact-ship.svg",
        ":/radar/icons/contact-unknown.svg", ":/radar/icons/contact-weapon.svg",
        ":/radar/icons/overlay-distorted.svg", ":/radar/icons/overlay-homing.svg",
        ":/radar/icons/overlay-stealth.svg", ":/radar/icons/overlay-tagged.svg",
        ":/radar/icons/overlay-threat.svg", ":/radar/icons/overlay-warp.svg",
        ":/radar/icons/ship-awacs.svg", ":/radar/icons/ship-bomber.svg",
        ":/radar/icons/ship-capital.svg", ":/radar/icons/ship-cargo.svg",
        ":/radar/icons/ship-corvette.svg", ":/radar/icons/ship-cruiser.svg",
        ":/radar/icons/ship-drydock.svg", ":/radar/icons/ship-escape-pod.svg",
        ":/radar/icons/ship-fighter.svg", ":/radar/icons/ship-freighter.svg",
        ":/radar/icons/ship-gas-miner.svg", ":/radar/icons/ship-knossos-device.svg",
        ":/radar/icons/ship-navbuoy.svg", ":/radar/icons/ship-sentry-gun.svg",
        ":/radar/icons/ship-super-capital.svg", ":/radar/icons/ship-support.svg",
        ":/radar/icons/ship-transport.svg", ":/radar/icons/weapon-beam.svg",
        ":/radar/icons/weapon-bomb.svg", ":/radar/icons/weapon-countermeasure.svg",
        ":/radar/icons/weapon-mine.svg", ":/radar/icons/weapon-missile.svg",
        ":/radar/icons/weapon-primary.svg", ":/radar/icons/weapon-special.svg",
    };
    const auto index = static_cast<std::size_t>(asset);
    return index < std::size(Paths) ? Paths[index] : "";
}

const std::array<RadarIconAsset, RadarIconAssetCount>& allRadarIconAssets() noexcept
{
    static constexpr std::array<RadarIconAsset, RadarIconAssetCount> Assets{
        RadarIconAsset::BracketsCrosshair, RadarIconAsset::BracketsLock,
        RadarIconAsset::BracketsSelected, RadarIconAsset::BracketsSelectedCompact,
        RadarIconAsset::BracketsSelectedLarge, RadarIconAsset::ContactAsteroid,
        RadarIconAsset::ContactDebris, RadarIconAsset::ContactFireball,
        RadarIconAsset::ContactJumpNode, RadarIconAsset::ContactNavigation,
        RadarIconAsset::ContactOther, RadarIconAsset::ContactShip,
        RadarIconAsset::ContactUnknown, RadarIconAsset::ContactWeapon,
        RadarIconAsset::OverlayDistorted, RadarIconAsset::OverlayHoming,
        RadarIconAsset::OverlayStealth, RadarIconAsset::OverlayTagged,
        RadarIconAsset::OverlayThreat, RadarIconAsset::OverlayWarp,
        RadarIconAsset::ShipAwacs, RadarIconAsset::ShipBomber,
        RadarIconAsset::ShipCapital, RadarIconAsset::ShipCargo,
        RadarIconAsset::ShipCorvette, RadarIconAsset::ShipCruiser,
        RadarIconAsset::ShipDrydock, RadarIconAsset::ShipEscapePod,
        RadarIconAsset::ShipFighter, RadarIconAsset::ShipFreighter,
        RadarIconAsset::ShipGasMiner, RadarIconAsset::ShipKnossosDevice,
        RadarIconAsset::ShipNavbuoy, RadarIconAsset::ShipSentryGun,
        RadarIconAsset::ShipSuperCapital, RadarIconAsset::ShipSupport,
        RadarIconAsset::ShipTransport, RadarIconAsset::WeaponBeam,
        RadarIconAsset::WeaponBomb, RadarIconAsset::WeaponCountermeasure,
        RadarIconAsset::WeaponMine, RadarIconAsset::WeaponMissile,
        RadarIconAsset::WeaponPrimary, RadarIconAsset::WeaponSpecial,
    };
    return Assets;
}

RadarVisualDescriptor resolveRadarVisual(
    const RadarVisualInput& input,
    const RadarManifestCatalog* catalog) noexcept
{
    RadarVisualDescriptor descriptor;
    const auto objectType = static_cast<protocol::ObjectType>(input.objectType);
    const auto category = static_cast<protocol::RadarCategory>(input.category);
    if (objectType == protocol::ObjectType::Unknown ||
        category == protocol::RadarCategory::Unknown) {
        descriptor.base = RadarIconAsset::ContactUnknown;
    } else if ((input.flags & protocol::ContactFlagBomb) != 0U) {
        descriptor.base = RadarIconAsset::WeaponBomb;
    } else if (objectType == protocol::ObjectType::Ship) {
        descriptor.base = RadarIconAsset::ContactShip;
        if (input.hasRevealedClass && input.revealedClassId != 0 && catalog != nullptr) {
            const auto found = catalog->shipRadarIconIds.find(input.revealedClassId);
            if (found != catalog->shipRadarIconIds.end() &&
                protocol::is_known_builtin_radar_icon_id(found->second)) {
                descriptor.base = shipAsset(found->second);
            }
        }
    } else if (objectType == protocol::ObjectType::Weapon) {
        descriptor.base = RadarIconAsset::ContactWeapon;
        if (input.hasRevealedClass && input.revealedClassId != 0 && catalog != nullptr) {
            const auto found = catalog->weapons.find(input.revealedClassId);
            if (found != catalog->weapons.end()) descriptor.base = weaponAsset(found->second.subtype);
        }
    } else if (objectType == protocol::ObjectType::Waypoint ||
               category == protocol::RadarCategory::Navigation) {
        descriptor.base = RadarIconAsset::ContactNavigation;
    } else if (objectType == protocol::ObjectType::JumpNode ||
               category == protocol::RadarCategory::JumpNode) {
        descriptor.base = RadarIconAsset::ContactJumpNode;
    } else if (objectType == protocol::ObjectType::Asteroid ||
               category == protocol::RadarCategory::Asteroid) {
        descriptor.base = RadarIconAsset::ContactAsteroid;
    } else if (objectType == protocol::ObjectType::Debris ||
               category == protocol::RadarCategory::Debris) {
        descriptor.base = RadarIconAsset::ContactDebris;
    } else if (objectType == protocol::ObjectType::Fireball) {
        descriptor.base = RadarIconAsset::ContactFireball;
    } else {
        descriptor.base = RadarIconAsset::ContactOther;
    }

    if ((input.flags & protocol::ContactFlagWarp) != 0U)
        descriptor.behind = RadarIconAsset::OverlayWarp;
    if ((input.flags & protocol::ContactFlagTagged) != 0U)
        appendOverlay(descriptor, RadarIconAsset::OverlayTagged);
    if ((input.flags & protocol::ContactFlagStealth) != 0U)
        appendOverlay(descriptor, RadarIconAsset::OverlayStealth);
    if ((input.flags & protocol::ContactFlagHoming) != 0U)
        appendOverlay(descriptor, RadarIconAsset::OverlayHoming);
    if ((input.flags & protocol::ContactFlagThreat) != 0U)
        appendOverlay(descriptor, RadarIconAsset::OverlayThreat);
    if (input.visibility == static_cast<std::uint8_t>(protocol::RadarVisibility::Distorted))
        appendOverlay(descriptor, RadarIconAsset::OverlayDistorted);
    return descriptor;
}

} // namespace simpit::radar

#pragma once

#include "radar_manifest.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace simpit::radar {

enum class RadarIconAsset : std::uint8_t {
    None,
    BracketsCrosshair,
    BracketsLock,
    BracketsSelected,
    BracketsSelectedCompact,
    BracketsSelectedLarge,
    ContactAsteroid,
    ContactDebris,
    ContactFireball,
    ContactJumpNode,
    ContactNavigation,
    ContactOther,
    ContactShip,
    ContactUnknown,
    ContactWeapon,
    OverlayDistorted,
    OverlayHoming,
    OverlayStealth,
    OverlayTagged,
    OverlayThreat,
    OverlayWarp,
    ShipAwacs,
    ShipBomber,
    ShipCapital,
    ShipCargo,
    ShipCorvette,
    ShipCruiser,
    ShipDrydock,
    ShipEscapePod,
    ShipFighter,
    ShipFreighter,
    ShipGasMiner,
    ShipKnossosDevice,
    ShipNavbuoy,
    ShipSentryGun,
    ShipSuperCapital,
    ShipSupport,
    ShipTransport,
    WeaponBeam,
    WeaponBomb,
    WeaponCountermeasure,
    WeaponMine,
    WeaponMissile,
    WeaponPrimary,
    WeaponSpecial,
};

constexpr std::size_t RadarIconAssetCount = 44;

struct RadarVisualInput {
    std::uint8_t objectType = 0;
    std::uint8_t category = 0;
    std::uint8_t visibility = 0;
    std::uint32_t flags = 0;
    bool hasRevealedClass = false;
    std::uint32_t revealedClassId = 0;
};

struct RadarVisualDescriptor {
    RadarIconAsset base = RadarIconAsset::ContactUnknown;
    RadarIconAsset behind = RadarIconAsset::None;
    std::array<RadarIconAsset, 5> overlays{};
    std::uint8_t overlayCount = 0;
};

const char* radarIconResourcePath(RadarIconAsset asset) noexcept;
const std::array<RadarIconAsset, RadarIconAssetCount>& allRadarIconAssets() noexcept;
RadarVisualDescriptor resolveRadarVisual(
    const RadarVisualInput& input,
    const RadarManifestCatalog* catalog) noexcept;

} // namespace simpit::radar

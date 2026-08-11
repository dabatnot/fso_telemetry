#include "radar_image.h"

#include "telemetry/protocol/packet_reader.h"
#include "telemetry/protocol/telemetry_protocol_constants.h"
#include "telemetry/protocol/telemetry_replication.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>

namespace simpit::radar {
namespace {

using telemetry::protocol::ByteView;
using telemetry::protocol::PacketReader;
using telemetry::protocol::RecordType;
using telemetry::protocol::StateAtom;

constexpr std::uint64_t CockpitSensorsCoverage = 0x07cbULL;
constexpr double Pi = 3.141592653589793238462643383279502884;

struct FlightPose final {
    std::uint64_t id = 0;
    std::array<double, 3> position{};
    std::array<double, 3> velocity{};
    // Wire order is w, x, y, z.
    std::array<double, 4> orientation{1.0, 0.0, 0.0, 0.0};
    bool valid = false;
};

bool readVec3(PacketReader& reader, std::array<double, 3>& output)
{
    for (double& component : output) {
        float value = 0.0F;
        if (!reader.read_f32(value) || !std::isfinite(value)) {
            return false;
        }
        component = value;
    }
    return true;
}

bool skipUtf8(PacketReader& reader, std::size_t maximum)
{
    std::string_view value;
    return reader.read_utf8(maximum, value);
}

bool readQString(PacketReader& reader, std::size_t maximum, QString& output)
{
    std::string_view value;
    if (!reader.read_utf8(maximum, value)) return false;
    output = QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
    return true;
}

std::array<double, 3> rotateWorldToLocal(const std::array<double, 4>& q,
                                         const std::array<double, 3>& v)
{
    const auto [w, x, y, z] = q;
    const auto [vx, vy, vz] = v;
    const double tx = 2.0 * (y * vz - z * vy);
    const double ty = 2.0 * (z * vx - x * vz);
    const double tz = 2.0 * (x * vy - y * vx);
    return {vx - w * tx + (y * tz - z * ty),
            vy - w * ty + (z * tx - x * tz),
            vz - w * tz + (x * ty - y * tx)};
}

bool decodeSession(const StateAtom& atom,
                   std::uint64_t& player,
                   std::uint64_t& sample,
                   QString& error)
{
    PacketReader reader(ByteView{atom.value.data(), atom.value.size()});
    std::uint64_t presence = 0, producer = 0, capabilities = 0, coverage = 0;
    std::uint64_t derivedEvents = 0, exactEvents = 0;
    std::uint8_t authority = 0, visibility = 0, phase = 0, reserved = 0;
    std::uint32_t generation = 0;
    if (!reader.read_u64(presence) || !reader.read_u64(producer) ||
        !reader.read_u64(sample) || !reader.read_u8(authority) ||
        !reader.read_u8(visibility) || !reader.read_u8(phase) ||
        !reader.read_u8(reserved) || !reader.read_u32(generation) ||
        !reader.read_u64(capabilities) || !reader.read_u64(coverage) ||
        !reader.read_u64(derivedEvents) || !reader.read_u64(exactEvents)) {
        error = QStringLiteral("Truncated SESSION_STATE");
        return false;
    }
    if (visibility != static_cast<std::uint8_t>(telemetry::protocol::VisibilityMode::Cockpit) ||
        coverage != CockpitSensorsCoverage) {
        error = QStringLiteral("Incompatible FSTL profile (CockpitSensors 0x07CB required)");
        return false;
    }
    if ((presence & telemetry::protocol::SessionStatePresenceFlagObservedPlayer) != 0U &&
        !reader.read_u64(player)) {
        error = QStringLiteral("SESSION_STATE has no valid player");
        return false;
    }
    if (!reader.at_end()) {
        error = QStringLiteral("Invalid SESSION_STATE");
        return false;
    }
    return true;
}

bool decodeFlight(const StateAtom& atom, FlightPose& pose)
{
    PacketReader reader(ByteView{atom.value.data(), atom.value.size()});
    std::uint64_t presence = 0, sample = 0;
    if (!reader.read_u64(pose.id) || !reader.read_u64(presence) ||
        !reader.read_u64(sample) || !readVec3(reader, pose.position)) {
        return false;
    }
    for (double& component : pose.orientation) {
        float value = 0.0F;
        if (!reader.read_f32(value) || !std::isfinite(value)) return false;
        component = value;
    }
    if (!readVec3(reader, pose.velocity)) return false;
    const double norm = std::sqrt(pose.orientation[0] * pose.orientation[0] +
                                  pose.orientation[1] * pose.orientation[1] +
                                  pose.orientation[2] * pose.orientation[2] +
                                  pose.orientation[3] * pose.orientation[3]);
    pose.valid = pose.id != 0 && std::abs(norm - 1.0) < 0.01;
    return pose.valid;
}

QPointF projectWorldPoint(const FlightPose& playerPose,
                          const std::array<double, 3>& world,
                          bool* defined = nullptr)
{
    const std::array<double, 3> separation{world[0] - playerPose.position[0],
                                           world[1] - playerPose.position[1],
                                           world[2] - playerPose.position[2]};
    const auto local = rotateWorldToLocal(playerPose.orientation, separation);
    const double distance = std::hypot(local[0], local[1], local[2]);
    return projectContact(local[0], local[1], local[2], distance, defined);
}

bool decodeTarget(const StateAtom& atom, std::uint64_t& observer,
                  const FlightPose& playerPose, RadarTargetInfo& target)
{
    if (atom.record_version != 5U && atom.record_version != 6U) return false;
    PacketReader reader(ByteView{atom.value.data(), atom.value.size()});
    std::uint64_t presence = 0, sample = 0;
    if (!reader.read_u64(observer) || !reader.read_u64(presence) ||
        !reader.read_u64(sample) || !reader.read_u64(target.entityId)) return false;
    target.presence = presence;
    std::uint64_t ignored64 = 0;
    std::uint32_t ignored32 = 0;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagPreviousTarget) != 0U &&
        !reader.read_u64(ignored64)) return false;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagRevealedIdentity) != 0U) {
        if (!reader.read_u8(target.revealedObjectType) ||
            !readQString(reader, 255U, target.revealedName) ||
            !reader.read_u32(target.revealedClassId) || !reader.read_u32(ignored32) ||
            !reader.read_u32(ignored32)) return false;
        target.hasIdentity = true;
    }
    if ((presence & telemetry::protocol::TargetStatePresenceFlagTimeOnTarget) != 0U &&
        !reader.read_u64(ignored64)) return false;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagTargetSubsystem) != 0U &&
        !reader.read_u32(ignored32)) return false;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagLockSubsystem) != 0U &&
        !reader.read_u32(ignored32)) return false;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagLastStealthObservation) != 0U) {
        std::array<double, 3> world{}, velocity{};
        if (!readVec3(reader, world) || !readVec3(reader, velocity)) return false;
        bool defined = false;
        target.stealthScopePosition = projectWorldPoint(playerPose, world, &defined);
        target.hasStealthPosition = defined;
    }
    if ((presence & telemetry::protocol::TargetStatePresenceFlagDistanceTrend) != 0U &&
        !reader.read_u8(target.distanceTrend)) return false;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagSpeedTrend) != 0U &&
        !reader.read_u8(target.speedTrend)) return false;
    bool ignoredBool = false;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagInCone) != 0U &&
        !reader.read_bool8(ignoredBool)) return false;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagLead) != 0U) {
        std::array<double, 3> leadWorld{};
        if (!readVec3(reader, leadWorld) || !reader.read_u32(ignored32)) return false;
        bool defined = false;
        target.leadScopePosition = projectWorldPoint(playerPose, leadWorld, &defined);
        target.hasLead = defined;
    }
    for (const auto flag : {telemetry::protocol::TargetStatePresenceFlagAttacker,
                            telemetry::protocol::TargetStatePresenceFlagDangerousWeapon,
                            telemetry::protocol::TargetStatePresenceFlagNearestLocked}) {
        if ((presence & flag) != 0U && !reader.read_u64(ignored64)) return false;
    }
    float value = 0.0F;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagExactHudDistance) != 0U) {
        if (!reader.read_f32(value)) return false;
        target.hudDistance = value;
        target.hasHudDistance = true;
    }
    if ((presence & telemetry::protocol::TargetStatePresenceFlagExactHudSpeed) != 0U) {
        if (!reader.read_f32(value)) return false;
        target.hudSpeed = value;
        target.hasHudSpeed = true;
    }
    if ((presence & telemetry::protocol::TargetStatePresenceFlagHudTypeLabel) != 0U &&
        !readQString(reader, 255U, target.hudTypeLabel)) return false;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagHudTargetColor) != 0U) {
        std::uint8_t rgba[4]{};
        for (auto& component : rgba) if (!reader.read_u8(component)) return false;
        target.hudColor = QColor(rgba[0], rgba[1], rgba[2], rgba[3]);
        target.hasHudColor = true;
    }
    if ((presence & telemetry::protocol::TargetStatePresenceFlagHudTargetSubsystemLabel) != 0U &&
        !readQString(reader, 255U, target.targetSubsystemLabel)) return false;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagHudLockSubsystemLabel) != 0U &&
        !readQString(reader, 255U, target.lockSubsystemLabel)) return false;
    if ((presence & telemetry::protocol::TargetStatePresenceFlagHudTargetStrength) != 0U) {
        bool hasShields = false;
        if (atom.record_version < 6U || !reader.read_f32(value) ||
            !reader.read_bool8(hasShields)) return false;
        target.hullRatio = value;
        target.hasShields = hasShields;
        if (hasShields) {
            if (!reader.read_f32(value)) return false;
            target.shieldRatio = value;
        }
        target.hasStrength = true;
    }
    return reader.at_end();
}

bool decodeLocks(const StateAtom& atom, std::uint64_t expectedObserver,
                 std::set<std::uint64_t>& targets, RadarLockInfo& currentLock,
                 std::uint64_t currentTarget)
{
    PacketReader reader(ByteView{atom.value.data(), atom.value.size()});
    std::uint64_t observer = 0, presence = 0, sample = 0;
    std::uint16_t count = 0;
    if (!reader.read_u64(observer) || observer != expectedObserver ||
        !reader.read_u64(presence) || !reader.read_u64(sample) ||
        !reader.read_u16(count)) return false;
    for (std::uint16_t index = 0; index < count; ++index) {
        std::uint8_t version = 0;
        std::uint16_t size = 0;
        if (!reader.read_u8(version) || !reader.read_u16(size) || version != 1U) return false;
        PacketReader item;
        if (!reader.subreader(size, item)) return false;
        std::uint16_t itemPresence = 0;
        bool locked = false, inCone = false;
        std::uint64_t target = 0;
        if (!item.read_u16(itemPresence) || !item.read_bool8(locked) ||
            !item.read_bool8(inCone) || !item.read_u64(target)) return false;
        std::uint32_t subsystem = 0;
        if ((itemPresence & telemetry::protocol::LockItemPresenceFlagSubsystem) != 0U &&
            !item.read_u32(subsystem)) return false;
        std::array<double, 3> world{};
        if (!readVec3(item, world)) return false;
        std::uint64_t remaining = 0;
        const bool hasRemaining =
            (itemPresence & telemetry::protocol::LockItemPresenceFlagLockAttempt) != 0U;
        if (hasRemaining && !item.read_u64(remaining)) return false;
        if (!item.at_end()) return false;
        if (target != 0) targets.insert(target);
        if (target == currentTarget && target != 0) {
            currentLock.targetEntityId = target;
            currentLock.locked = locked;
            currentLock.inCone = inCone;
            currentLock.remainingUs = remaining;
            currentLock.hasRemaining = hasRemaining;
        }
    }
    return reader.at_end();
}

bool skipVersionedItems(PacketReader& reader)
{
    std::uint16_t count = 0;
    if (!reader.read_u16(count)) return false;
    for (std::uint16_t index = 0; index < count; ++index) {
        std::uint8_t version = 0;
        std::uint16_t size = 0;
        if (!reader.read_u8(version) || version != 1U ||
            !reader.read_u16(size) || !reader.skip(size)) return false;
    }
    return true;
}

bool decodeSelectedSecondaryWeapon(const StateAtom& atom, std::uint64_t expectedObserver,
                                   std::uint32_t& weaponClassId)
{
    PacketReader reader(ByteView{atom.value.data(), atom.value.size()});
    std::uint64_t observer = 0, presence = 0, sample = 0;
    std::uint16_t primaryCount = 0, secondaryCount = 0, tertiaryCount = 0, reserved = 0;
    std::uint32_t currentPrimary = 0, currentSecondary = 0, currentTertiary = 0, flags = 0;
    if (!reader.read_u64(observer) || observer != expectedObserver ||
        !reader.read_u64(presence) || !reader.read_u64(sample) ||
        !reader.read_u16(primaryCount) || !reader.read_u16(secondaryCount) ||
        !reader.read_u16(tertiaryCount) || !reader.read_u16(reserved) ||
        !reader.read_u32(currentPrimary) || !reader.read_u32(currentSecondary) ||
        !reader.read_u32(currentTertiary) || !reader.read_u32(flags)) return false;
    if ((presence & telemetry::protocol::WeaponStatePresenceFlagPreviousPrimary) != 0U &&
        !reader.skip(4U)) return false;
    if ((presence & telemetry::protocol::WeaponStatePresenceFlagPreviousSecondary) != 0U &&
        !reader.skip(4U)) return false;
    if ((presence & telemetry::protocol::WeaponStatePresenceFlagTargetingLaser) != 0U &&
        !reader.skip(4U)) return false;
    if ((presence & telemetry::protocol::WeaponStatePresenceFlagSwarm) != 0U &&
        !reader.skip(6U)) return false;
    if ((presence & telemetry::protocol::WeaponStatePresenceFlagRemoteDetonation) != 0U &&
        !reader.skip(8U)) return false;
    if ((presence & telemetry::protocol::WeaponStatePresenceFlagPerBurstRotation) != 0U &&
        !reader.skip(4U)) return false;
    if (!skipVersionedItems(reader)) return false;
    std::uint16_t encodedSecondary = 0;
    if (!reader.read_u16(encodedSecondary)) return false;
    for (std::uint16_t index = 0; index < encodedSecondary; ++index) {
        std::uint8_t version = 0;
        std::uint16_t size = 0;
        PacketReader item;
        std::uint16_t itemPresence = 0, bankIndex = 0;
        std::uint32_t bankId = 0, itemWeaponClass = 0;
        if (!reader.read_u8(version) || version != 1U || !reader.read_u16(size) ||
            !reader.subreader(size, item) || !item.read_u16(itemPresence) ||
            !item.read_u16(bankIndex) || !item.read_u32(bankId) ||
            !item.read_u32(itemWeaponClass)) return false;
        if (bankId == currentSecondary) weaponClassId = itemWeaponClass;
    }
    return true;
}

bool decodeRadarState(const StateAtom& atom, std::uint64_t expectedObserver,
                      RadarSensorInfo& sensors)
{
    PacketReader reader(ByteView{atom.value.data(), atom.value.size()});
    std::uint64_t observer = 0, presence = 0, sample = 0, ignored64 = 0;
    std::uint8_t mode = 0;
    float range = 0.0F, current = 0.0F, maximum = 0.0F, ignored = 0.0F;
    if (!reader.read_u64(observer) || observer != expectedObserver ||
        !reader.read_u64(presence) || !reader.read_u64(sample) ||
        !reader.read_u8(mode) || !reader.read_f32(range) ||
        !reader.read_u8(sensors.state) || !reader.read_f32(current) ||
        !reader.read_f32(maximum)) return false;
    sensors.ratio = maximum > 0.0F ? std::clamp<double>(current / maximum, 0.0, 1.0) : 0.0;
    if ((presence & telemetry::protocol::RadarStatePresenceFlagBrightRange) != 0U &&
        !reader.read_f32(ignored)) return false;
    if ((presence & telemetry::protocol::RadarStatePresenceFlagPrimitiveRange) != 0U &&
        !reader.read_f32(ignored)) return false;
    if ((presence & telemetry::protocol::RadarStatePresenceFlagAwacs) != 0U &&
        (!reader.read_f32(ignored) || !reader.read_f32(ignored))) return false;
    if ((presence & telemetry::protocol::RadarStatePresenceFlagEmp) != 0U) {
        if (!reader.read_f32(ignored) || !reader.read_u64(ignored64)) return false;
        sensors.empIntensity = ignored;
        sensors.empRemainingUs = ignored64;
        sensors.hasEmp = ignored > 0.0F || ignored64 > 0U;
    }
    if ((presence & telemetry::protocol::RadarStatePresenceFlagJamming) != 0U &&
        !reader.read_f32(ignored)) return false;
    if ((presence & telemetry::protocol::RadarStatePresenceFlagVisibilityTimes) != 0U &&
        (!reader.read_u64(ignored64) || !reader.read_u64(ignored64))) return false;
    return reader.at_end();
}

bool decodeThreats(const StateAtom& atom, std::uint64_t expectedObserver,
                   const FlightPose& playerPose, std::vector<RadarThreat>& threats)
{
    PacketReader reader(ByteView{atom.value.data(), atom.value.size()});
    std::uint64_t observer = 0, presence = 0, sample = 0, optional = 0;
    std::uint8_t level = 0;
    if (!reader.read_u64(observer) || observer != expectedObserver ||
        !reader.read_u64(presence) || !reader.read_u64(sample) || !reader.read_u8(level)) return false;
    std::uint64_t dangerousWeapon = 0;
    if ((presence & telemetry::protocol::ThreatStatePresenceFlagNearestAttacker) != 0U &&
        !reader.read_u64(optional)) return false;
    if ((presence & telemetry::protocol::ThreatStatePresenceFlagDangerousWeapon) != 0U &&
        !reader.read_u64(dangerousWeapon)) return false;
    if ((presence & telemetry::protocol::ThreatStatePresenceFlagNearestHoming) != 0U &&
        !reader.read_u64(optional)) return false;
    std::uint16_t count = 0;
    if (!reader.read_u16(count)) return false;
    for (std::uint16_t index = 0; index < count; ++index) {
        std::uint8_t version = 0, guidance = 0;
        std::uint16_t size = 0, itemPresence = 0;
        std::uint32_t weaponClass = 0, subsystem = 0;
        std::uint64_t target = 0;
        PacketReader item;
        RadarThreat threat;
        std::array<double, 3> world{}, velocity{};
        if (!reader.read_u8(version) || version != 1U || !reader.read_u16(size) ||
            !reader.subreader(size, item) || !item.read_u16(itemPresence) ||
            !item.read_u8(guidance) || !item.read_u8(threat.visibility) ||
            !item.read_u64(threat.entityId) || !item.read_u32(weaponClass) ||
            !item.read_u64(target)) return false;
        if ((itemPresence & telemetry::protocol::IncomingMissilePresenceFlagHomingSubsystem) != 0U &&
            !item.read_u32(subsystem)) return false;
        if (!readVec3(item, world) || !item.skip(4U * sizeof(float)) ||
            !readVec3(item, velocity) || !item.at_end()) return false;
        bool defined = false;
        threat.scopeDirection = projectWorldPoint(playerPose, world, &defined);
        const std::array<double, 3> separation{world[0] - playerPose.position[0],
                                               world[1] - playerPose.position[1],
                                               world[2] - playerPose.position[2]};
        const std::array<double, 3> relativeVelocity{velocity[0] - playerPose.velocity[0],
                                                     velocity[1] - playerPose.velocity[1],
                                                     velocity[2] - playerPose.velocity[2]};
        threat.distance = std::hypot(separation[0], separation[1], separation[2]);
        const double closingSpeed = threat.distance > 0.0
            ? -(separation[0] * relativeVelocity[0] + separation[1] * relativeVelocity[1] +
                separation[2] * relativeVelocity[2]) / threat.distance : 0.0;
        threat.closingTimeSeconds = closingSpeed > 0.0 ? threat.distance / closingSpeed
                                                       : std::numeric_limits<double>::infinity();
        threat.dangerous = threat.entityId == dangerousWeapon;
        if (defined) threats.push_back(threat);
    }
    return reader.at_end();
}

bool decodeContact(const StateAtom& atom,
                   std::uint64_t expectedObserver,
                   const FlightPose& playerPose,
                   const RadarManifestCatalog* catalog,
                   RadarContact& contact)
{
    if (atom.record_version != 4U) return false;
    PacketReader reader(ByteView{atom.value.data(), atom.value.size()});
    std::uint64_t observer = 0, sample = 0;
    std::array<double, 3> world{}, velocity{}, local{};
    float projectionDistance = 0.0F, radius = 0.0F;
    if (!reader.read_u64(observer) || observer != expectedObserver ||
        !reader.read_u64(contact.id) || contact.id == 0 ||
        !reader.read_u64(contact.presence) || !reader.read_u64(sample) ||
        !reader.read_u8(contact.objectType) || !reader.read_u8(contact.category) ||
        !reader.read_u8(contact.visibility) || !readVec3(reader, world) ||
        !readVec3(reader, velocity) || !readVec3(reader, local) ||
        !reader.read_f32(projectionDistance) || !std::isfinite(projectionDistance) ||
        projectionDistance < 0.0F || !reader.read_f32(radius) ||
        !std::isfinite(radius) || !reader.read_u32(contact.flags)) {
        return false;
    }
    if ((contact.presence & telemetry::protocol::RadarContactsPresenceFlagIconSize) != 0U) {
        if (!reader.read_f32(contact.iconSize) || contact.iconSize < 0.0F) return false;
    }
    if ((contact.presence & telemetry::protocol::RadarContactsPresenceFlagRevealedName) != 0U &&
        !readQString(reader, 255U, contact.revealedName)) return false;
    if ((contact.presence & telemetry::protocol::RadarContactsPresenceFlagRevealedClass) != 0U) {
        if (!reader.read_u32(contact.revealedClassId) || contact.revealedClassId == 0) return false;
        contact.hasRevealedClass = true;
    }
    if ((contact.presence & telemetry::protocol::RadarContactsPresenceFlagRevealedTeamIff) != 0U) {
        std::uint32_t team = 0, iff = 0;
        if (!reader.read_u32(team) || !reader.read_u32(iff)) return false;
    }
    if ((contact.presence & telemetry::protocol::RadarContactsPresenceFlagDetectionTimes) != 0U) {
        std::uint64_t first = 0, last = 0;
        if (!reader.read_u64(first) || !reader.read_u64(last)) return false;
    }
    if ((contact.presence & telemetry::protocol::RadarContactsPresenceFlagConfidence) != 0U) {
        float confidence = 0.0F;
        if (!reader.read_f32(confidence)) return false;
    }
    if ((contact.presence & telemetry::protocol::RadarContactsPresenceFlagHudTypeLabel) != 0U &&
        !readQString(reader, 255U, contact.hudTypeLabel)) return false;
    if ((contact.presence & telemetry::protocol::RadarContactsPresenceFlagRadarVisual) == 0U) return false;
    std::uint8_t red = 0, green = 0, blue = 0, alpha = 0;
    if (!reader.read_u8(red) || !reader.read_u8(green) || !reader.read_u8(blue) ||
        !reader.read_u8(alpha) || !reader.read_u8(contact.blipType) ||
        !reader.at_end() || contact.blipType > 5U || contact.visibility > 2U ||
        contact.category > 7U) return false;

    contact.scopePosition = projectContact(local[0], local[1], local[2], projectionDistance);
    contact.worldPosition = world;
    contact.worldVelocity = velocity;
    contact.color = QColor(red, green, blue, alpha);
    contact.glyph = contactGlyph(contact.category, contact.flags);
    contact.visual = resolveRadarVisual(
        {contact.objectType, contact.category, contact.visibility, contact.flags,
         contact.hasRevealedClass, contact.revealedClassId}, catalog);
    if (playerPose.valid) {
        const std::array<double, 3> separation{world[0] - playerPose.position[0],
                                               world[1] - playerPose.position[1],
                                               world[2] - playerPose.position[2]};
        contact.distance = std::hypot(separation[0], separation[1], separation[2]);
        const auto localSeparation = rotateWorldToLocal(playerPose.orientation, separation);
        contact.elevationRadians = std::atan2(
            localSeparation[1], std::hypot(localSeparation[0], localSeparation[2]));
        contact.azimuthRadians = contactAzimuthRadians(
            localSeparation[0], localSeparation[1], localSeparation[2]);
        const std::array<double, 3> predictedWorld{
            world[0] + (velocity[0] - playerPose.velocity[0]) * 0.75,
            world[1] + (velocity[1] - playerPose.velocity[1]) * 0.75,
            world[2] + (velocity[2] - playerPose.velocity[2]) * 0.75};
        bool predictedDefined = false;
        contact.predictedScopePosition = projectWorldPoint(playerPose, predictedWorld,
                                                            &predictedDefined);
        contact.hasPredictedScopePosition = predictedDefined;
    }
    return std::isfinite(contact.distance) && std::isfinite(contact.elevationRadians) &&
           std::isfinite(contact.azimuthRadians);
}

} // namespace

double RadarContact::alpha() const noexcept
{
    const double visibilityAlpha = visibility == 0U ? 0.35 : visibility == 2U ? 0.6 : 1.0;
    const double intensity = (flags & telemetry::protocol::ContactFlagBright) != 0U ? 1.0 : 0.72;
    return visibilityAlpha * intensity;
}

double RadarContact::priority() const noexcept
{
    return (currentTarget ? 1'000'000.0 : 0.0) +
           ((flags & 0xe0U) != 0U ? 500'000.0 : 0.0) +
           (lockTarget ? 250'000.0 : 0.0) +
           (visibility == 1U ? 100'000.0 : visibility == 2U ? 50'000.0 : 0.0) -
           distance / 1000.0;
}

QPointF projectContact(double localX, double localY, double localZ,
                       double projectionDistance, bool* directionDefined) noexcept
{
    if (directionDefined != nullptr) *directionDefined = false;
    if (!std::isfinite(localX) || !std::isfinite(localY) || !std::isfinite(localZ) ||
        !std::isfinite(projectionDistance) || projectionDistance <= 0.0) {
        return {};
    }
    const double transverse = std::hypot(localX, localY);
    const double cosine = std::clamp(localZ / projectionDistance, -1.0, 1.0);
    const double radial = projectionDistance < localZ ? 0.0 : std::acos(cosine) / Pi;
    if (transverse < 0.01) return {};
    if (directionDefined != nullptr) *directionDefined = true;
    return {localX * radial / transverse, -localY * radial / transverse};
}

double contactAzimuthRadians(double localX, double localY, double localZ) noexcept
{
    return std::atan2(localX, std::hypot(localY, localZ));
}

ContactGlyph contactGlyph(std::uint8_t category, std::uint32_t flags) noexcept
{
    if ((flags & 0x60U) != 0U) return ContactGlyph::Diamond;
    if (category == 1U) return ContactGlyph::Square;
    if (category == 5U || category == 6U) return ContactGlyph::Circle;
    return ContactGlyph::Triangle;
}

std::shared_ptr<const RadarImage> makeRadarImage(
    const telemetry::protocol::StateImage& state, QString* error)
{
    return makeRadarImage(state, nullptr, 0U, error);
}

std::shared_ptr<const RadarImage> makeRadarImage(
    const telemetry::protocol::StateImage& state,
    const RadarManifestCatalog* catalog,
    QString* error)
{
    return makeRadarImage(state, catalog, 0U, error);
}

std::shared_ptr<const RadarImage> makeRadarImage(
    const telemetry::protocol::StateImage& state,
    const RadarManifestCatalog* catalog,
    std::uint64_t sessionId,
    QString* error)
{
    QString localError;
    auto fail = [&](const QString& message) -> std::shared_ptr<const RadarImage> {
        if (error != nullptr) *error = message;
        return {};
    };
    auto image = std::make_shared<RadarImage>();
    image->sessionId = sessionId;
    FlightPose playerPose;
    bool sessionSeen = false;
    for (const StateAtom& atom : state.records()) {
        const auto type = static_cast<RecordType>(atom.key.record_type);
        if (type == RecordType::SessionState) {
            if (sessionSeen || !decodeSession(atom, image->playerEntityId,
                                               image->producerSampleTimeUs, localError)) {
                return fail(localError.isEmpty() ? QStringLiteral("Duplicate SESSION_STATE") : localError);
            }
            sessionSeen = true;
        }
    }
    if (!sessionSeen) return fail(QStringLiteral("Missing SESSION_STATE"));
    if (image->playerEntityId == 0) {
        if (error != nullptr) error->clear();
        return image;
    }

    for (const StateAtom& atom : state.records()) {
        if (atom.key.record_type == static_cast<std::uint16_t>(RecordType::FlightState)) {
            FlightPose candidate;
            if (!decodeFlight(atom, candidate)) return fail(QStringLiteral("Invalid FLIGHT_STATE"));
            if (candidate.id == image->playerEntityId) playerPose = candidate;
        }
    }
    if (!playerPose.valid) return fail(QStringLiteral("Missing player FLIGHT_STATE"));
    image->playerWorldPosition = playerPose.position;
    image->playerWorldVelocity = playerPose.velocity;
    image->playerOrientationLocalToWorld = playerPose.orientation;

    std::set<std::uint64_t> lockTargets;
    bool targetSeen = false;
    for (const StateAtom& atom : state.records()) {
        const auto type = static_cast<RecordType>(atom.key.record_type);
        if (type == RecordType::TargetState) {
            std::uint64_t observer = 0;
            RadarTargetInfo target;
            if (!decodeTarget(atom, observer, playerPose, target))
                return fail(QStringLiteral("Invalid TARGET_STATE v5/v6"));
            if (observer == image->playerEntityId) {
                if (targetSeen) return fail(QStringLiteral("Duplicate TARGET_STATE"));
                targetSeen = true;
                image->target = std::move(target);
                image->currentTargetEntityId = image->target.entityId;
            }
        } else if (type == RecordType::LockState) {
            if (!decodeLocks(atom, image->playerEntityId, lockTargets,
                             image->lock, image->currentTargetEntityId)) {
                return fail(QStringLiteral("Invalid LOCK_STATE"));
            }
        }
    }

    // LOCK_STATE sorts before TARGET_STATE in a canonical state image. Replay
    // the auxiliary records now that the current target is known.
    lockTargets.clear();
    image->lock = RadarLockInfo{};
    std::uint32_t selectedSecondaryWeaponClass = 0;
    for (const StateAtom& atom : state.records()) {
        const auto type = static_cast<RecordType>(atom.key.record_type);
        if (type == RecordType::LockState) {
            if (!decodeLocks(atom, image->playerEntityId, lockTargets,
                             image->lock, image->currentTargetEntityId))
                return fail(QStringLiteral("Invalid LOCK_STATE"));
        } else if (type == RecordType::WeaponState) {
            if (!decodeSelectedSecondaryWeapon(atom, image->playerEntityId,
                                               selectedSecondaryWeaponClass))
                return fail(QStringLiteral("Invalid WEAPON_STATE"));
        } else if (type == RecordType::RadarState) {
            if (!decodeRadarState(atom, image->playerEntityId, image->sensors))
                return fail(QStringLiteral("Invalid RADAR_STATE"));
        } else if (type == RecordType::ThreatState) {
            if (!decodeThreats(atom, image->playerEntityId, playerPose, image->threats))
                return fail(QStringLiteral("Invalid THREAT_STATE"));
        }
    }
    if (image->lock.hasRemaining && selectedSecondaryWeaponClass != 0U && catalog != nullptr) {
        const auto metadata = catalog->weapons.find(selectedSecondaryWeaponClass);
        if (metadata != catalog->weapons.end() && metadata->second.hasLock &&
            metadata->second.nominalLockTimeUs > 0U) {
            image->lock.progress = std::clamp(
                1.0 - static_cast<double>(image->lock.remainingUs) /
                          static_cast<double>(metadata->second.nominalLockTimeUs), 0.0, 1.0);
            image->lock.hasProgress = true;
        }
    }
    if (image->lock.locked) {
        image->lock.progress = 1.0;
        image->lock.hasProgress = true;
    }

    for (const StateAtom& atom : state.records()) {
        if (atom.key.record_type != static_cast<std::uint16_t>(RecordType::RadarContacts)) continue;
        RadarContact contact;
        if (!decodeContact(atom, image->playerEntityId, playerPose, catalog, contact)) {
            return fail(QStringLiteral("Invalid RADAR_CONTACTS v4"));
        }
        contact.currentTarget = contact.id == image->currentTargetEntityId && contact.id != 0;
        contact.lockTarget = lockTargets.contains(contact.id);
        image->contacts.push_back(contact);
    }
    std::stable_sort(image->contacts.begin(), image->contacts.end(),
                     [](const RadarContact& left, const RadarContact& right) {
        if (left.priority() != right.priority()) return left.priority() > right.priority();
        return left.id < right.id;
    });
    if (error != nullptr) error->clear();
    return image;
}

} // namespace simpit::radar

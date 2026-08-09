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
        error = QStringLiteral("SESSION_STATE tronqué");
        return false;
    }
    if (visibility != static_cast<std::uint8_t>(telemetry::protocol::VisibilityMode::Cockpit) ||
        coverage != CockpitSensorsCoverage) {
        error = QStringLiteral("Profil FSTL incompatible (CockpitSensors 0x07CB requis)");
        return false;
    }
    if ((presence & telemetry::protocol::SessionStatePresenceFlagObservedPlayer) != 0U &&
        !reader.read_u64(player)) {
        error = QStringLiteral("SESSION_STATE sans joueur valide");
        return false;
    }
    if (!reader.at_end()) {
        error = QStringLiteral("SESSION_STATE invalide");
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
    const double norm = std::sqrt(pose.orientation[0] * pose.orientation[0] +
                                  pose.orientation[1] * pose.orientation[1] +
                                  pose.orientation[2] * pose.orientation[2] +
                                  pose.orientation[3] * pose.orientation[3]);
    pose.valid = pose.id != 0 && std::abs(norm - 1.0) < 0.01;
    return pose.valid;
}

bool decodeTarget(const StateAtom& atom, std::uint64_t& observer, std::uint64_t& target)
{
    if (atom.record_version != 5U) return false;
    PacketReader reader(ByteView{atom.value.data(), atom.value.size()});
    std::uint64_t presence = 0, sample = 0;
    return reader.read_u64(observer) && reader.read_u64(presence) &&
           reader.read_u64(sample) && reader.read_u64(target);
}

bool decodeLocks(const StateAtom& atom, std::uint64_t expectedObserver,
                 std::set<std::uint64_t>& targets)
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
        if (target != 0) targets.insert(target);
    }
    return reader.at_end();
}

bool decodeContact(const StateAtom& atom,
                   std::uint64_t expectedObserver,
                   const FlightPose& playerPose,
                   RadarContact& contact)
{
    if (atom.record_version != 4U) return false;
    PacketReader reader(ByteView{atom.value.data(), atom.value.size()});
    std::uint64_t observer = 0, presence = 0, sample = 0;
    std::uint8_t objectType = 0;
    std::array<double, 3> world{}, velocity{}, local{};
    float projectionDistance = 0.0F, radius = 0.0F;
    if (!reader.read_u64(observer) || observer != expectedObserver ||
        !reader.read_u64(contact.id) || contact.id == 0 ||
        !reader.read_u64(presence) || !reader.read_u64(sample) ||
        !reader.read_u8(objectType) || !reader.read_u8(contact.category) ||
        !reader.read_u8(contact.visibility) || !readVec3(reader, world) ||
        !readVec3(reader, velocity) || !readVec3(reader, local) ||
        !reader.read_f32(projectionDistance) || !std::isfinite(projectionDistance) ||
        projectionDistance < 0.0F || !reader.read_f32(radius) ||
        !std::isfinite(radius) || !reader.read_u32(contact.flags)) {
        return false;
    }
    if ((presence & telemetry::protocol::RadarContactsPresenceFlagIconSize) != 0U) {
        float icon = 0.0F;
        if (!reader.read_f32(icon)) return false;
    }
    if ((presence & telemetry::protocol::RadarContactsPresenceFlagRevealedName) != 0U &&
        !skipUtf8(reader, 255U)) return false;
    if ((presence & telemetry::protocol::RadarContactsPresenceFlagRevealedClass) != 0U) {
        std::uint32_t value = 0;
        if (!reader.read_u32(value)) return false;
    }
    if ((presence & telemetry::protocol::RadarContactsPresenceFlagRevealedTeamIff) != 0U) {
        std::uint32_t team = 0, iff = 0;
        if (!reader.read_u32(team) || !reader.read_u32(iff)) return false;
    }
    if ((presence & telemetry::protocol::RadarContactsPresenceFlagDetectionTimes) != 0U) {
        std::uint64_t first = 0, last = 0;
        if (!reader.read_u64(first) || !reader.read_u64(last)) return false;
    }
    if ((presence & telemetry::protocol::RadarContactsPresenceFlagConfidence) != 0U) {
        float confidence = 0.0F;
        if (!reader.read_f32(confidence)) return false;
    }
    if ((presence & telemetry::protocol::RadarContactsPresenceFlagHudTypeLabel) != 0U &&
        !skipUtf8(reader, 255U)) return false;
    if ((presence & telemetry::protocol::RadarContactsPresenceFlagRadarVisual) == 0U) return false;
    std::uint8_t red = 0, green = 0, blue = 0, alpha = 0;
    if (!reader.read_u8(red) || !reader.read_u8(green) || !reader.read_u8(blue) ||
        !reader.read_u8(alpha) || !reader.read_u8(contact.blipType) ||
        !reader.at_end() || contact.blipType > 5U || contact.visibility > 2U ||
        contact.category > 7U) return false;

    contact.scopePosition = projectContact(local[0], local[1], local[2], projectionDistance);
    contact.color = QColor(red, green, blue, alpha);
    contact.glyph = contactGlyph(contact.category, contact.flags);
    if (playerPose.valid) {
        const std::array<double, 3> separation{world[0] - playerPose.position[0],
                                               world[1] - playerPose.position[1],
                                               world[2] - playerPose.position[2]};
        contact.distance = std::hypot(separation[0], separation[1], separation[2]);
        const auto localSeparation = rotateWorldToLocal(playerPose.orientation, separation);
        contact.elevationRadians = std::atan2(
            localSeparation[1], std::hypot(localSeparation[0], localSeparation[2]));
    }
    return std::isfinite(contact.distance) && std::isfinite(contact.elevationRadians);
}

} // namespace

double RadarContact::alpha() const noexcept
{
    if (visibility == 0U) return 0.35;
    if (visibility == 2U) return 0.6;
    return 1.0;
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
    QString localError;
    auto fail = [&](const QString& message) -> std::shared_ptr<const RadarImage> {
        if (error != nullptr) *error = message;
        return {};
    };
    auto image = std::make_shared<RadarImage>();
    FlightPose playerPose;
    bool sessionSeen = false;
    for (const StateAtom& atom : state.records()) {
        const auto type = static_cast<RecordType>(atom.key.record_type);
        if (type == RecordType::SessionState) {
            if (sessionSeen || !decodeSession(atom, image->playerEntityId,
                                               image->producerSampleTimeUs, localError)) {
                return fail(localError.isEmpty() ? QStringLiteral("SESSION_STATE dupliqué") : localError);
            }
            sessionSeen = true;
        }
    }
    if (!sessionSeen) return fail(QStringLiteral("SESSION_STATE absent"));
    if (image->playerEntityId == 0) {
        if (error != nullptr) error->clear();
        return image;
    }

    for (const StateAtom& atom : state.records()) {
        if (atom.key.record_type == static_cast<std::uint16_t>(RecordType::FlightState)) {
            FlightPose candidate;
            if (!decodeFlight(atom, candidate)) return fail(QStringLiteral("FLIGHT_STATE invalide"));
            if (candidate.id == image->playerEntityId) playerPose = candidate;
        }
    }
    if (!playerPose.valid) return fail(QStringLiteral("FLIGHT_STATE joueur absent"));

    std::set<std::uint64_t> lockTargets;
    bool targetSeen = false;
    for (const StateAtom& atom : state.records()) {
        const auto type = static_cast<RecordType>(atom.key.record_type);
        if (type == RecordType::TargetState) {
            std::uint64_t observer = 0, target = 0;
            if (!decodeTarget(atom, observer, target)) return fail(QStringLiteral("TARGET_STATE v5 requis"));
            if (observer == image->playerEntityId) {
                if (targetSeen) return fail(QStringLiteral("TARGET_STATE dupliqué"));
                targetSeen = true;
                image->currentTargetEntityId = target;
            }
        } else if (type == RecordType::LockState) {
            if (!decodeLocks(atom, image->playerEntityId, lockTargets)) {
                return fail(QStringLiteral("LOCK_STATE invalide"));
            }
        }
    }

    for (const StateAtom& atom : state.records()) {
        if (atom.key.record_type != static_cast<std::uint16_t>(RecordType::RadarContacts)) continue;
        RadarContact contact;
        if (!decodeContact(atom, image->playerEntityId, playerPose, contact)) {
            return fail(QStringLiteral("RADAR_CONTACTS v4 invalide"));
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

#pragma once

#include "radar_icons.h"

#include <QColor>
#include <QMetaType>
#include <QPointF>
#include <QString>

#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace telemetry::protocol {
class StateImage;
}

namespace simpit::radar {

enum class ContactGlyph : std::uint8_t {
    Square,
    Circle,
    Diamond,
    Triangle,
};

struct RadarContact final {
    std::uint64_t id = 0;
    std::array<double, 3> worldPosition{};
    std::array<double, 3> worldVelocity{};
    QPointF scopePosition;
    QPointF predictedScopePosition;
    QColor color;
    QString revealedName;
    QString hudTypeLabel;
    ContactGlyph glyph = ContactGlyph::Triangle;
    RadarVisualDescriptor visual;
    std::uint8_t visibility = 0;
    std::uint8_t objectType = 0;
    std::uint8_t category = 0;
    std::uint8_t blipType = 0;
    std::uint64_t presence = 0;
    std::uint32_t flags = 0;
    std::uint32_t revealedClassId = 0;
    float iconSize = 0.0F;
    double distance = 0.0;
    double elevationRadians = 0.0;
    double azimuthRadians = 0.0;
    bool currentTarget = false;
    bool lockTarget = false;
    bool inRange = true;
    bool invalid = false;
    bool hasRevealedClass = false;
    bool hasPredictedScopePosition = false;

    double alpha() const noexcept;
    double priority() const noexcept;
};

struct RadarTargetInfo final {
    std::uint64_t entityId = 0;
    std::uint64_t presence = 0;
    std::uint8_t revealedObjectType = 0;
    std::uint8_t distanceTrend = 0;
    std::uint8_t speedTrend = 0;
    std::uint32_t revealedClassId = 0;
    QString revealedName;
    QString hudTypeLabel;
    QString targetSubsystemLabel;
    QString lockSubsystemLabel;
    QColor hudColor{213, 248, 241};
    QPointF leadScopePosition;
    QPointF stealthScopePosition;
    double hudDistance = 0.0;
    double hudSpeed = 0.0;
    double hullRatio = 0.0;
    double shieldRatio = 0.0;
    bool hasIdentity = false;
    bool hasHudDistance = false;
    bool hasHudSpeed = false;
    bool hasHudColor = false;
    bool hasLead = false;
    bool hasStealthPosition = false;
    bool hasStrength = false;
    bool hasShields = false;
};

struct RadarLockInfo final {
    std::uint64_t targetEntityId = 0;
    std::uint64_t remainingUs = 0;
    double progress = 0.0;
    bool locked = false;
    bool inCone = false;
    bool hasRemaining = false;
    bool hasProgress = false;
};

struct RadarThreat final {
    std::uint64_t entityId = 0;
    QPointF scopeDirection;
    double distance = 0.0;
    double closingTimeSeconds = 0.0;
    std::uint8_t visibility = 0;
    bool dangerous = false;
};

struct RadarSensorInfo final {
    std::uint8_t state = 0;
    double ratio = 0.0;
    double empIntensity = 0.0;
    std::uint64_t empRemainingUs = 0;
    bool hasEmp = false;
};

struct RadarImage final {
    std::uint64_t sessionId = 0;
    std::uint64_t producerSampleTimeUs = 0;
    std::uint64_t playerEntityId = 0;
    std::uint64_t currentTargetEntityId = 0;
	bool missionPaused = false;
	float timeCompression = 1.0F;
    std::array<double, 3> playerWorldPosition{};
    std::array<double, 3> playerWorldVelocity{};
    std::array<double, 4> playerOrientationLocalToWorld{1.0, 0.0, 0.0, 0.0};
    RadarTargetInfo target;
    RadarLockInfo lock;
    RadarSensorInfo sensors;
    std::vector<RadarThreat> threats;
    std::vector<RadarContact> contacts;
};

QPointF projectContact(double localX,
                       double localY,
                       double localZ,
                       double projectionDistance,
                       bool* directionDefined = nullptr) noexcept;
double contactAzimuthRadians(double localX, double localY, double localZ) noexcept;
ContactGlyph contactGlyph(std::uint8_t category, std::uint32_t flags) noexcept;

// Converts one already atomically validated FSTL state image. The returned
// object is immutable by ownership and is left null on any radar-data error.
std::shared_ptr<const RadarImage> makeRadarImage(
    const telemetry::protocol::StateImage& state,
    QString* error = nullptr);
std::shared_ptr<const RadarImage> makeRadarImage(
    const telemetry::protocol::StateImage& state,
    const RadarManifestCatalog* catalog,
    QString* error);
std::shared_ptr<const RadarImage> makeRadarImage(
    const telemetry::protocol::StateImage& state,
    const RadarManifestCatalog* catalog,
    std::uint64_t sessionId,
    QString* error);

} // namespace simpit::radar

Q_DECLARE_METATYPE(std::shared_ptr<const simpit::radar::RadarImage>)

#pragma once

#include "radar_icons.h"

#include <QColor>
#include <QMetaType>
#include <QPointF>
#include <QString>

#include <cstdint>
#include <memory>
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
    QPointF scopePosition;
    QColor color;
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
    bool currentTarget = false;
    bool lockTarget = false;
    bool inRange = true;
    bool invalid = false;
    bool hasRevealedClass = false;

    double alpha() const noexcept;
    double priority() const noexcept;
};

struct RadarImage final {
    std::uint64_t producerSampleTimeUs = 0;
    std::uint64_t playerEntityId = 0;
    std::uint64_t currentTargetEntityId = 0;
    std::vector<RadarContact> contacts;
};

QPointF projectContact(double localX,
                       double localY,
                       double localZ,
                       double projectionDistance,
                       bool* directionDefined = nullptr) noexcept;
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

} // namespace simpit::radar

Q_DECLARE_METATYPE(std::shared_ptr<const simpit::radar::RadarImage>)

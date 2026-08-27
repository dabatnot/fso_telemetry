#pragma once

#include "radar_display_settings.h"
#include "radar_image.h"
#include "svg_icon_cache.h"
#include "vfnt_font.h"

#include <QElapsedTimer>
#include <QHash>
#include <QLineF>
#include <QRectF>
#include <QSizeF>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <cstdint>

namespace simpit::radar {

enum class DistortedContactPresentation : std::uint8_t {
    ContactAndOverlay,
    ContactOnly,
    OverlayOnly,
    Hidden,
};

struct ContactAnimationState final {
    QPointF jitter;
    double sizeMultiplier = 1.0;
    DistortedContactPresentation presentation =
        DistortedContactPresentation::ContactAndOverlay;
};

struct CalloutLayout final {
    QRectF panel;
    QLineF diagonal;
    QLineF shoulder;
    bool valid = false;
    bool panelRight = false;
    bool upward = true;
};

struct TargetDisplayState final {
    std::uint64_t sessionId = 0;
    std::uint64_t targetEntityId = 0;
    qint64 oppositeZoneSinceMs = -1;
    bool panelRight = false;
    bool diagonalUpward = true;
    int verticalChevron = 0;
    int horizontalChevron = 0;
};

class RadarWidget final : public QWidget {
    Q_OBJECT
public:
    explicit RadarWidget(QWidget* parent = nullptr);

    QRectF radarCircleRect() const noexcept;
    std::shared_ptr<const RadarImage> image() const noexcept { return m_image; }
    static double contactIconSize(double radarDiameter, const RadarContact& contact) noexcept;
    static ContactAnimationState contactAnimation(
        double radarDiameter, const RadarContact& contact, qint64 milliseconds) noexcept;
    static CalloutLayout calculateCalloutLayout(
        const QPointF& anchor, double bracketExtent, const QSizeF& panelSize,
        double separatorOffset, const QRectF& bounds, bool panelRight,
        bool upward) noexcept;
    void setAnimationTimeForTesting(qint64 milliseconds);
    void setDisplaySettings(const RadarDisplaySettings& settings);
    RadarDisplaySettings displaySettings() const noexcept { return m_display; }
    TargetDisplayState targetDisplayState() const noexcept { return m_targetDisplay; }

public slots:
    void setImage(std::shared_ptr<const RadarImage> image);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void drawContact(QPainter& painter, const RadarContact& contact,
                     const QPointF& position, double radarDiameter) const;
    void drawEnhancedLayers(QPainter& painter, const QRectF& circle,
                            const QPointF& center, double radius);
    void updateTrails(const std::shared_ptr<const RadarImage>& next);
    void updateTargetDisplayState(const std::shared_ptr<const RadarImage>& next);
    qint64 animationMilliseconds() const noexcept;
    void updateAnimationTimer();

    std::shared_ptr<const RadarImage> m_image;
    mutable SvgIconCache m_iconCache;
    QElapsedTimer m_animationClock;
    QTimer m_animationTimer;
    qint64 m_testAnimationMilliseconds = -1;
    RadarDisplaySettings m_display;
    TargetDisplayState m_targetDisplay;
    struct TrailSample { QPointF scopePosition; qint64 milliseconds = 0; };
    QHash<qulonglong, QVector<TrailSample>> m_trails;
};

} // namespace simpit::radar

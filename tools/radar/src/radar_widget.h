#pragma once

#include "radar_client.h"
#include "svg_icon_cache.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

namespace simpit::radar {

struct ContactAnimationState final {
    QPointF jitter;
    double opacity = 1.0;
    double sizeMultiplier = 1.0;
};

class RadarWidget final : public QWidget {
    Q_OBJECT
public:
    explicit RadarWidget(QWidget* parent = nullptr);

    QRectF radarCircleRect() const noexcept;
    std::shared_ptr<const RadarImage> image() const noexcept { return m_image; }
    ClientStatus status() const noexcept { return m_status; }
    static double contactIconSize(double radarDiameter, const RadarContact& contact) noexcept;
    static ContactAnimationState contactAnimation(
        double radarDiameter, const RadarContact& contact, qint64 milliseconds) noexcept;
    void setAnimationTimeForTesting(qint64 milliseconds);

public slots:
    void setImage(std::shared_ptr<const RadarImage> image);
    void setStatus(ClientStatus status, const QString& detail = {});

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void drawContact(QPainter& painter, const RadarContact& contact,
                     const QPointF& position, double radarDiameter) const;
    qint64 animationMilliseconds() const noexcept;
    void updateAnimationTimer();

    std::shared_ptr<const RadarImage> m_image;
    mutable SvgIconCache m_iconCache;
    QElapsedTimer m_animationClock;
    QTimer m_animationTimer;
    qint64 m_testAnimationMilliseconds = -1;
    ClientStatus m_status = ClientStatus::Disconnected;
    QString m_detail;
};

} // namespace simpit::radar

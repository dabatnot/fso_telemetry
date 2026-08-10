#include "radar_widget.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>

#include <algorithm>
#include <cmath>

namespace simpit::radar {
namespace {

constexpr double Pi = 3.141592653589793238462643383279502884;
const QColor GridColor(112, 228, 209, 61);
const QColor BackgroundColor(7, 22, 28, 194);
const QColor LabelColor(213, 248, 241);

} // namespace

RadarWidget::RadarWidget(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setMinimumSize(320, 320);
    m_animationClock.start();
    m_animationTimer.setInterval(50);
    connect(&m_animationTimer, &QTimer::timeout, this, [this]() { update(); });
}

QRectF RadarWidget::radarCircleRect() const noexcept
{
    const double side = std::min(width(), height());
    const double radius = std::max(20.0, side / 2.0 - 18.0);
    return QRectF(width() / 2.0 - radius, height() / 2.0 - radius,
                  radius * 2.0, radius * 2.0);
}

void RadarWidget::setImage(std::shared_ptr<const RadarImage> image)
{
    m_image = std::move(image);
    updateAnimationTimer();
    update();
}

void RadarWidget::setStatus(ClientStatus status, const QString& detail)
{
    m_status = status;
    m_detail = detail;
    update();
}

double RadarWidget::contactIconSize(double radarDiameter, const RadarContact& contact) noexcept
{
    const double scale = std::clamp(radarDiameter / 684.0, 0.80, 1.45);
    const bool emphasized = (contact.flags &
        (telemetry::protocol::ContactFlagBomb | telemetry::protocol::ContactFlagThreat)) != 0U;
    double base = emphasized ? 18.0 : 16.0;
    if (contact.currentTarget) base = std::max(base, 20.0);
    return std::clamp(base * scale, 12.0, 28.0);
}

ContactAnimationState RadarWidget::contactAnimation(
    double radarDiameter, const RadarContact& contact, qint64 milliseconds) noexcept
{
    ContactAnimationState state;
    const double uiScale = std::clamp(radarDiameter / 684.0, 0.80, 1.45);
    const double seconds = static_cast<double>(milliseconds) / 1000.0;
    const std::uint64_t mixed = contact.id * 0x9e3779b97f4a7c15ULL;
    const double phase = static_cast<double>(mixed & 0xffffU) / 65535.0 * 2.0 * Pi;
    if (contact.visibility ==
        static_cast<std::uint8_t>(telemetry::protocol::RadarVisibility::Distorted)) {
        constexpr double amplitude = 1.25;
        state.jitter.setX(std::sin(seconds * 2.0 * Pi * 6.0 + phase) * amplitude);
        state.jitter.setY(std::cos(seconds * 2.0 * Pi * 7.0 + phase * 1.7) * amplitude);
        state.opacity = 0.85 + 0.15 * std::sin(seconds * 2.0 * Pi * 4.0 + phase);
    }
    if ((contact.flags & telemetry::protocol::ContactFlagThreat) != 0U)
        state.sizeMultiplier = 1.06 + 0.06 * std::sin(seconds * 2.0 * Pi * 2.0 + phase);
    return state;
}

void RadarWidget::setAnimationTimeForTesting(qint64 milliseconds)
{
    m_testAnimationMilliseconds = milliseconds;
    updateAnimationTimer();
    update();
}

qint64 RadarWidget::animationMilliseconds() const noexcept
{
    return m_testAnimationMilliseconds >= 0
        ? m_testAnimationMilliseconds : m_animationClock.elapsed();
}

void RadarWidget::updateAnimationTimer()
{
    bool animated = false;
    if (m_image != nullptr) {
        animated = std::any_of(m_image->contacts.begin(), m_image->contacts.end(),
            [](const RadarContact& contact) {
                return contact.visibility ==
                           static_cast<std::uint8_t>(telemetry::protocol::RadarVisibility::Distorted) ||
                       (contact.flags & telemetry::protocol::ContactFlagThreat) != 0U;
            });
    }
    if (animated && m_testAnimationMilliseconds < 0) {
        if (!m_animationTimer.isActive()) m_animationTimer.start();
    } else {
        m_animationTimer.stop();
    }
}

void RadarWidget::drawContact(QPainter& painter, const RadarContact& contact,
                              const QPointF& position, double radarDiameter) const
{
    const double uiScale = std::clamp(radarDiameter / 684.0, 0.80, 1.45);
    double iconSize = contactIconSize(radarDiameter, contact);
    const ContactAnimationState animation = contactAnimation(
        radarDiameter, contact, animationMilliseconds());
    iconSize *= animation.sizeMultiplier;

    painter.save();
    painter.translate(position + animation.jitter);
    painter.setOpacity(contact.alpha() * animation.opacity);
    QPen pen(contact.color);
    pen.setWidthF(contact.currentTarget ? 2.6 : 1.5);
    pen.setCapStyle(Qt::SquareCap);
    pen.setJoinStyle(Qt::MiterJoin);
    if (contact.visibility == 2U) pen.setDashPattern({3.0, 3.0});
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const auto fallbackGlyph = [&]() {
        const double size = iconSize * 0.42;
        QPainterPath path;
        switch (contact.glyph) {
        case ContactGlyph::Diamond:
            path.moveTo(0, -size); path.lineTo(size, 0); path.lineTo(0, size);
            path.lineTo(-size, 0); path.closeSubpath();
            break;
        case ContactGlyph::Square:
            path.addRect(-size, -size, size * 2.0, size * 2.0);
            break;
        case ContactGlyph::Circle:
            path.addEllipse(QPointF{}, size, size);
            break;
        case ContactGlyph::Triangle:
            path.moveTo(0, -size); path.lineTo(size, size);
            path.lineTo(-size, size); path.closeSubpath();
            break;
        }
        painter.drawPath(path);
    };
    const auto fallbackBrackets = [&](double extent) {
        const double arm = std::max(4.0, extent * 0.45);
        QPainterPath brackets;
        brackets.moveTo(-extent, -extent); brackets.lineTo(-extent + arm, -extent);
        brackets.moveTo(-extent, -extent); brackets.lineTo(-extent, -extent + arm);
        brackets.moveTo(extent, -extent); brackets.lineTo(extent - arm, -extent);
        brackets.moveTo(extent, -extent); brackets.lineTo(extent, -extent + arm);
        brackets.moveTo(-extent, extent); brackets.lineTo(-extent + arm, extent);
        brackets.moveTo(-extent, extent); brackets.lineTo(-extent, extent - arm);
        brackets.moveTo(extent, extent); brackets.lineTo(extent - arm, extent);
        brackets.moveTo(extent, extent); brackets.lineTo(extent, extent - arm);
        painter.drawPath(brackets);
    };

    const double dpr = devicePixelRatioF();
    if (contact.visual.behind != RadarIconAsset::None)
        m_iconCache.draw(painter, contact.visual.behind, QPointF{}, iconSize,
                         contact.color, dpr);
    const bool svgBase = m_iconCache.draw(
        painter, contact.visual.base, QPointF{}, iconSize, contact.color, dpr);
    if (!svgBase) fallbackGlyph();
    for (std::uint8_t index = 0; index < contact.visual.overlayCount; ++index)
        m_iconCache.draw(painter, contact.visual.overlays[index], QPointF{}, iconSize,
                         contact.color, dpr);

    if (contact.currentTarget) {
        RadarIconAsset bracket = RadarIconAsset::BracketsSelected;
        if (contact.objectType == static_cast<std::uint8_t>(telemetry::protocol::ObjectType::Weapon) ||
            (contact.flags & telemetry::protocol::ContactFlagBomb) != 0U) {
            bracket = RadarIconAsset::BracketsCrosshair;
        } else if (iconSize < 15.0) {
            bracket = RadarIconAsset::BracketsSelectedCompact;
        } else if (iconSize > 20.0) {
            bracket = RadarIconAsset::BracketsSelectedLarge;
        }
        const double bracketSize = iconSize + 10.0 * uiScale;
        if (!m_iconCache.draw(painter, bracket, QPointF{}, bracketSize, contact.color, dpr))
            fallbackBrackets(bracketSize / 2.0);
        if (contact.lockTarget &&
            !m_iconCache.draw(painter, RadarIconAsset::BracketsLock, QPointF{},
                              bracketSize * 1.18, contact.color, dpr)) {
            fallbackBrackets(bracketSize * 0.59);
        }
    } else if (contact.lockTarget) {
        const double bracketSize = (iconSize + 10.0 * uiScale) * 1.18;
        if (!m_iconCache.draw(painter, RadarIconAsset::BracketsLock, QPointF{},
                              bracketSize, contact.color, dpr)) {
            fallbackBrackets(bracketSize / 2.0);
        }
    }
    if (std::abs(contact.elevationRadians) > 0.04) {
        const double direction = contact.elevationRadians > 0.0 ? -1.0 : 1.0;
        const double edge = std::max(10.0, iconSize / 2.0 + 3.0);
        QPainterPath chevron;
        chevron.moveTo(-3, direction * edge); chevron.lineTo(0, direction * (edge + 3.0));
        chevron.lineTo(3, direction * edge);
        painter.drawPath(chevron);
    }
    if (!contact.inRange) painter.fillRect(QRectF(-1.5, -1.5, 3, 3), contact.color);
    painter.restore();
}

void RadarWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                           QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), QColor(2, 8, 11));

    const QRectF circle = radarCircleRect();
    const QPointF center = circle.center();
    const double radius = circle.width() / 2.0;
    painter.setPen(QPen(GridColor, 1.0));
    painter.setBrush(BackgroundColor);
    painter.drawEllipse(circle);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, radius * 0.5, radius * 0.5);

    painter.save();
    painter.translate(center);
    painter.rotate(45.0);
    const double inner = radius * 0.5;
    painter.drawLine(QPointF(-radius, 0), QPointF(-inner, 0));
    painter.drawLine(QPointF(inner, 0), QPointF(radius, 0));
    painter.drawLine(QPointF(0, -radius), QPointF(0, -inner));
    painter.drawLine(QPointF(0, inner), QPointF(0, radius));
    painter.restore();

    QFont labels = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    labels.setPixelSize(13);
    painter.setFont(labels);
    painter.setPen(LabelColor);
    painter.drawText(QRectF(center.x() - 55, center.y() - radius, 110, 18),
                     Qt::AlignHCenter | Qt::AlignTop, tr("HAUT"));
    painter.drawText(QRectF(center.x() - 55, center.y() + radius - 18, 110, 18),
                     Qt::AlignHCenter | Qt::AlignBottom, tr("BAS"));
    painter.drawText(QRectF(center.x() + radius - 65, center.y() - 15, 62, 18),
                     Qt::AlignLeft | Qt::AlignVCenter, tr("DROITE"));
    painter.drawText(QRectF(center.x() - radius + 3, center.y() - 15, 62, 18),
                     Qt::AlignRight | Qt::AlignVCenter, tr("GAUCHE"));
    painter.drawText(QRectF(center.x() - 55, center.y() + 6, 110, 18),
                     Qt::AlignHCenter | Qt::AlignVCenter, tr("AVANT"));
    QPainterPath player;
    player.moveTo(center.x(), center.y() - 6);
    player.lineTo(center.x() - 5, center.y() + 5);
    player.lineTo(center.x() + 5, center.y() + 5);
    player.closeSubpath();
    painter.fillPath(player, LabelColor);

    if (m_image) {
        for (const RadarContact& contact : m_image->contacts) {
            if (contact.invalid) continue;
            const QPointF position(center.x() + contact.scopePosition.x() * radius,
                                   center.y() + contact.scopePosition.y() * radius);
            drawContact(painter, contact, position, circle.width());
        }
    }

    if (m_status == ClientStatus::Stale || m_status == ClientStatus::Reconnecting ||
        m_status == ClientStatus::Error) {
        painter.fillRect(rect(), QColor(0, 0, 0, m_status == ClientStatus::Stale ? 105 : 145));
        QFont overlay = labels;
        overlay.setPixelSize(std::clamp(static_cast<int>(width() / 18), 18, 48));
        overlay.setBold(true);
        painter.setFont(overlay);
        painter.setPen(m_status == ClientStatus::Error ? QColor(255, 107, 85) : LabelColor);
        const QString title = m_status == ClientStatus::Stale ? tr("STALE")
                              : m_status == ClientStatus::Reconnecting ? tr("RECONNEXION")
                              : tr("ERREUR");
        painter.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignCenter,
                         m_detail.isEmpty() ? title : title + QLatin1Char('\n') + m_detail);
    }
}

} // namespace simpit::radar

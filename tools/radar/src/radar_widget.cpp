#include "radar_widget.h"

#include "telemetry/protocol/telemetry_protocol_constants.h"

#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QLineF>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace simpit::radar {
namespace {

constexpr double Pi = 3.141592653589793238462643383279502884;
const QColor GridColor(112, 228, 209, 61);
const QColor BackgroundColor(7, 22, 28, 194);
const QColor LabelColor(213, 248, 241);
const QColor LeadColor(255, 220, 64);
const QColor ChevronColor(255, 255, 255);
const QColor LinkColor(102, 217, 232);
const QColor StaleColor(255, 184, 61);
const QColor FailureColor(255, 92, 87);
constexpr double CalloutDeadZone = 0.20;
constexpr qint64 CalloutSwitchDelayMs = 600;
constexpr double ChevronActivateRadians = 0.05;
constexpr double ChevronReleaseRadians = 0.03;

int hystereticDirection(double angle, int current) noexcept
{
    if (!std::isfinite(angle)) return 0;
    if (current == 0)
        return std::abs(angle) > ChevronActivateRadians ? (angle > 0.0 ? 1 : -1) : 0;
    if (std::abs(angle) < ChevronReleaseRadians) return 0;
    if ((current > 0 && angle < -ChevronActivateRadians) ||
        (current < 0 && angle > ChevronActivateRadians)) {
        return angle > 0.0 ? 1 : -1;
    }
    return current;
}

QString trendMarker(std::uint8_t trend)
{
    using telemetry::protocol::ValueTrend;
    switch (static_cast<ValueTrend>(trend)) {
    case ValueTrend::Decreasing: return QStringLiteral("↓");
    case ValueTrend::Increasing: return QStringLiteral("↑");
    case ValueTrend::Stable: return QStringLiteral("=");
    default: return {};
    }
}

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
    updateTargetDisplayState(image);
    updateTrails(image);
    m_image = std::move(image);
    updateAnimationTimer();
    update();
}

void RadarWidget::updateTargetDisplayState(const std::shared_ptr<const RadarImage>& next)
{
    const std::uint64_t sessionId = next != nullptr ? next->sessionId : 0U;
    const std::uint64_t targetEntityId = next != nullptr ? next->target.entityId : 0U;
    if (next == nullptr || targetEntityId == 0U) {
        m_targetDisplay = {};
        return;
    }

    const RadarContact* targetContact = nullptr;
    for (const auto& contact : next->contacts) {
        if (contact.id == targetEntityId || contact.currentTarget) {
            targetContact = &contact;
            if (contact.id == targetEntityId) break;
        }
    }
    const bool hasPosition = targetContact != nullptr || next->target.hasStealthPosition;
    const double scopeX = targetContact != nullptr ? targetContact->scopePosition.x()
                                                    : next->target.stealthScopePosition.x();
    const bool newTarget = m_targetDisplay.sessionId != sessionId ||
                           m_targetDisplay.targetEntityId != targetEntityId;
    if (newTarget) {
        m_targetDisplay = {};
        m_targetDisplay.sessionId = sessionId;
        m_targetDisplay.targetEntityId = targetEntityId;
        m_targetDisplay.panelRight = hasPosition && scopeX < 0.0;
        m_targetDisplay.diagonalUpward = true;
    } else if (hasPosition) {
        const bool clearlyOpposite =
            (m_targetDisplay.panelRight && scopeX > CalloutDeadZone) ||
            (!m_targetDisplay.panelRight && scopeX < -CalloutDeadZone);
        const qint64 now = animationMilliseconds();
        if (!clearlyOpposite) {
            m_targetDisplay.oppositeZoneSinceMs = -1;
        } else if (m_targetDisplay.oppositeZoneSinceMs < 0 ||
                   now < m_targetDisplay.oppositeZoneSinceMs) {
            m_targetDisplay.oppositeZoneSinceMs = now;
        } else if (now - m_targetDisplay.oppositeZoneSinceMs >= CalloutSwitchDelayMs) {
            m_targetDisplay.panelRight = !m_targetDisplay.panelRight;
            m_targetDisplay.oppositeZoneSinceMs = -1;
        }
    }

    if (targetContact != nullptr) {
        m_targetDisplay.verticalChevron = hystereticDirection(
            targetContact->elevationRadians, m_targetDisplay.verticalChevron);
        m_targetDisplay.horizontalChevron = hystereticDirection(
            targetContact->azimuthRadians, m_targetDisplay.horizontalChevron);
    }
}

void RadarWidget::setDisplaySettings(const RadarDisplaySettings& settings)
{
    m_display = settings;
    if (!m_display.trails) m_trails.clear();
    updateAnimationTimer();
    update();
}

void RadarWidget::updateTrails(const std::shared_ptr<const RadarImage>& next)
{
    if (next == nullptr || (m_image != nullptr &&
        (m_image->sessionId != next->sessionId ||
         m_image->playerEntityId != next->playerEntityId))) {
        m_trails.clear();
    }
    if (next == nullptr) return;
    QSet<qulonglong> retained;
    const qint64 now = animationMilliseconds();
    for (const auto& contact : next->contacts) {
        if (contact.id == next->playerEntityId) continue;
        const bool tactical = contact.currentTarget ||
            (contact.flags & telemetry::protocol::ContactFlagThreat) != 0U;
        if (!tactical) continue;
        retained.insert(contact.id);
        auto& samples = m_trails[contact.id];
        if (samples.isEmpty() ||
            QLineF(samples.constLast().scopePosition, contact.scopePosition).length() > 0.001) {
            samples.push_back({contact.scopePosition, now});
        }
        while (!samples.isEmpty() && (now - samples.front().milliseconds > 1500 ||
                                      samples.size() > 8)) samples.removeFirst();
    }
    for (auto it = m_trails.begin(); it != m_trails.end();) {
        if (!retained.contains(it.key())) it = m_trails.erase(it);
        else ++it;
    }
}

void RadarWidget::setStatus(ClientStatus status, const QString& detail)
{
    m_status = status;
    m_detail = detail;
    updateAnimationTimer();
    update();
}

SystemOverlayPresentation RadarWidget::systemOverlay(ClientStatus status)
{
    switch (status) {
    case ClientStatus::Resolving:
    case ClientStatus::Connecting:
        return {tr("ESTABLISHING SENSOR LINK"), tr("STANDBY"), LinkColor, true};
    case ClientStatus::Synchronizing:
        return {tr("BUILDING TACTICAL PICTURE"), tr("STANDBY"), LinkColor, true};
	case ClientStatus::Ready:
		return {tr("SENSOR LINK READY"), tr("WAITING FOR MISSION"), LinkColor, true};
	case ClientStatus::Paused:
		return {tr("MISSION PAUSED"), tr("SENSOR LINK MAINTAINED"), LinkColor, true};
    case ClientStatus::Stale:
        return {tr("SENSOR FEED LOST"), tr("HOLDING LAST CONTACT PICTURE"), StaleColor, true};
    case ClientStatus::Reconnecting:
        return {tr("REACQUIRING SENSOR LINK"), tr("STANDBY"), LinkColor, true};
    case ClientStatus::Error:
        return {tr("SENSOR LINK FAILURE"), tr("CHECK TELEMETRY SOURCE"), FailureColor, true};
    default:
        return {};
    }
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

CalloutLayout RadarWidget::calculateCalloutLayout(
    const QPointF& anchor, double bracketExtent, const QSizeF& panelSize,
    double separatorOffset, const QRectF& bounds, bool panelRight,
    bool upward) noexcept
{
    CalloutLayout layout;
    layout.panelRight = panelRight;
    layout.upward = upward;
    if (!std::isfinite(anchor.x()) || !std::isfinite(anchor.y()) ||
        !std::isfinite(bracketExtent) || bracketExtent < 0.0 ||
        panelSize.width() <= 0.0 || panelSize.height() <= 0.0 ||
        separatorOffset < 0.0 || separatorOffset > panelSize.height()) {
        return layout;
    }

    constexpr double diagonalLength = 24.0;
    constexpr double nominalShoulder = 16.0;
    constexpr double minimumShoulder = 8.0;
    constexpr double margin = 4.0;
    const double component = 1.0 / std::sqrt(2.0);
    const QPointF direction(panelRight ? component : -component,
                            upward ? -component : component);
    const QPointF diagonalStart = anchor + direction * bracketExtent;
    const QPointF diagonalEnd = diagonalStart + direction * diagonalLength;
    const double panelY = diagonalEnd.y() - separatorOffset;
    const double minimumY = bounds.top() + margin;
    const double maximumY = bounds.bottom() - margin - panelSize.height();
    if (panelY < minimumY || panelY > maximumY) return layout;

    double panelX = 0.0;
    if (panelRight) {
        const double minimumX = std::max(bounds.left() + margin,
                                         diagonalEnd.x() + minimumShoulder);
        const double maximumX = bounds.right() - margin - panelSize.width();
        if (minimumX > maximumX) return layout;
        panelX = std::clamp(diagonalEnd.x() + nominalShoulder, minimumX, maximumX);
    } else {
        const double minimumX = bounds.left() + margin;
        const double maximumX = std::min(bounds.right() - margin - panelSize.width(),
                                         diagonalEnd.x() - minimumShoulder - panelSize.width());
        if (minimumX > maximumX) return layout;
        panelX = std::clamp(diagonalEnd.x() - nominalShoulder - panelSize.width(),
                            minimumX, maximumX);
    }

    layout.panel = QRectF(QPointF(panelX, panelY), panelSize);
    const QPointF panelEdge(panelRight ? layout.panel.left() : layout.panel.right(),
                            diagonalEnd.y());
    layout.diagonal = QLineF(diagonalStart, diagonalEnd);
    layout.shoulder = QLineF(diagonalEnd, panelEdge);
    layout.valid = true;
    return layout;
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
    bool animated = systemOverlay(m_status).visible;
    if (m_image != nullptr) {
        animated = animated || std::any_of(m_image->contacts.begin(), m_image->contacts.end(),
            [](const RadarContact& contact) {
                return contact.visibility ==
                           static_cast<std::uint8_t>(telemetry::protocol::RadarVisibility::Distorted) ||
                       (contact.flags & telemetry::protocol::ContactFlagThreat) != 0U;
            });
        animated = animated || (m_display.sensorEffects && m_image->sensors.hasEmp);
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
    if (!contact.inRange) painter.fillRect(QRectF(-1.5, -1.5, 3, 3), contact.color);
    if (contact.currentTarget && contact.id == m_targetDisplay.targetEntityId) {
        const double edge = std::max(10.0, (iconSize + 10.0 * uiScale) * 0.5);
        const double inner = std::max(4.0, edge - 4.0);
        painter.setOpacity(1.0);
        QPen chevronPen(ChevronColor, 1.4);
        chevronPen.setCapStyle(Qt::RoundCap);
        chevronPen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(chevronPen);
        painter.setBrush(Qt::NoBrush);
        if (m_targetDisplay.verticalChevron != 0) {
            const double direction = m_targetDisplay.verticalChevron > 0 ? -1.0 : 1.0;
            QPainterPath chevron;
            chevron.moveTo(-3.0, direction * inner);
            chevron.lineTo(0.0, direction * edge);
            chevron.lineTo(3.0, direction * inner);
            painter.drawPath(chevron);
        }
        if (m_targetDisplay.horizontalChevron != 0) {
            const double direction = m_targetDisplay.horizontalChevron > 0 ? 1.0 : -1.0;
            QPainterPath chevron;
            chevron.moveTo(direction * inner, -3.0);
            chevron.lineTo(direction * edge, 0.0);
            chevron.lineTo(direction * inner, 3.0);
            painter.drawPath(chevron);
        }
    }
    painter.restore();
}

void RadarWidget::drawEnhancedLayers(QPainter& painter, const QRectF& circle,
                                     const QPointF& center, double radius)
{
    if (m_image == nullptr) return;
    const auto scopeToPixel = [&](const QPointF& scope) {
        return QPointF(center.x() + scope.x() * radius,
                       center.y() + scope.y() * radius);
    };

    const RadarContact* targetContact = nullptr;
    QSet<qulonglong> contactIds;
    for (const auto& contact : m_image->contacts) {
        if (contact.id == m_image->playerEntityId) continue;
        contactIds.insert(contact.id);
        if (contact.currentTarget) targetContact = &contact;
        const bool tactical = contact.currentTarget ||
            (contact.flags & telemetry::protocol::ContactFlagThreat) != 0U;
        if (!tactical) continue;
        const QPointF position = scopeToPixel(contact.scopePosition);
        if (m_display.trails) {
            const auto samples = m_trails.constFind(contact.id);
            if (samples != m_trails.cend() && samples->size() > 1) {
                QPen trailPen(contact.color);
                trailPen.setWidthF(1.1);
                painter.setPen(trailPen);
                for (qsizetype index = 1; index < samples->size(); ++index) {
                    painter.setOpacity(static_cast<double>(index) / samples->size() * 0.55);
                    painter.drawLine(scopeToPixel(samples->at(index - 1).scopePosition),
                                     scopeToPixel(samples->at(index).scopePosition));
                }
                painter.setOpacity(1.0);
            }
        }
        if (m_display.motionVectors && contact.hasPredictedScopePosition) {
            QPointF vector = scopeToPixel(contact.predictedScopePosition) - position;
            const double length = std::hypot(vector.x(), vector.y());
            if (length > 24.0) vector *= 24.0 / length;
            QPen vectorPen(contact.color, 1.2);
            vectorPen.setCapStyle(Qt::RoundCap);
            painter.setPen(vectorPen);
            painter.drawLine(position, position + vector);
            painter.drawEllipse(position + vector, 1.5, 1.5);
        }
    }

    const auto& target = m_image->target;
    QColor accent = target.hasHudColor ? target.hudColor
                                      : targetContact != nullptr ? targetContact->color : LabelColor;
    if (m_display.lead && target.hasLead && target.entityId != 0U) {
        const QPointF lead = scopeToPixel(target.leadScopePosition);
        painter.setPen(QPen(LeadColor, 1.4));
        painter.drawLine(lead + QPointF(-4, 0), lead + QPointF(4, 0));
        painter.drawLine(lead + QPointF(0, -4), lead + QPointF(0, 4));
        if (targetContact != nullptr)
            painter.drawLine(scopeToPixel(targetContact->scopePosition), lead);
    }

    if (m_display.lock && targetContact != nullptr && m_image->lock.hasProgress) {
        const QPointF position = scopeToPixel(targetContact->scopePosition);
        const double icon = contactIconSize(circle.width(), *targetContact);
        const double extent = (icon + 10.0 * std::clamp(circle.width() / 684.0, 0.80, 1.45)) * 0.72;
        QRectF arcRect(position.x() - extent, position.y() - extent,
                       extent * 2.0, extent * 2.0);
        QPen arcPen(accent, 2.0);
        arcPen.setCapStyle(Qt::RoundCap);
        painter.setPen(arcPen);
        painter.drawArc(arcRect, 90 * 16,
                        static_cast<int>(-360.0 * 16.0 * m_image->lock.progress));
        if (m_image->lock.hasRemaining) {
            painter.setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            painter.drawText(QRectF(position.x() - 36, position.y() + extent + 2, 72, 16),
                             Qt::AlignHCenter | Qt::AlignTop,
                             QString::number(m_image->lock.remainingUs / 1'000'000.0, 'f', 1) +
                                 QStringLiteral(" s"));
        }
    }

    if (m_display.edgeThreats) {
        std::vector<const RadarThreat*> edges;
        for (const auto& threat : m_image->threats)
            if (!contactIds.contains(threat.entityId)) edges.push_back(&threat);
        std::sort(edges.begin(), edges.end(), [](const RadarThreat* left, const RadarThreat* right) {
            if (left->dangerous != right->dangerous) return left->dangerous;
            if (left->closingTimeSeconds != right->closingTimeSeconds)
                return left->closingTimeSeconds < right->closingTimeSeconds;
            return left->distance < right->distance;
        });
        if (edges.size() > 4U) edges.resize(4U);
        painter.setBrush(QColor(255, 126, 72, 220));
        painter.setPen(Qt::NoPen);
        for (const auto* threat : edges) {
            QPointF direction = threat->scopeDirection;
            double length = std::hypot(direction.x(), direction.y());
            if (length < 0.001) direction = QPointF(0, -1), length = 1.0;
            direction /= length;
            const QPointF tip = center + direction * (radius - 5.0);
            const QPointF inward = -direction;
            const QPointF perpendicular(-direction.y(), direction.x());
            QPolygonF triangle;
            triangle << tip << tip + inward * 11.0 + perpendicular * 5.0
                     << tip + inward * 11.0 - perpendicular * 5.0;
            painter.drawPolygon(triangle);
        }
    }

    if (m_display.sensorEffects) {
        QColor rim = m_image->sensors.state ==
                static_cast<std::uint8_t>(telemetry::protocol::SensorState::Online)
            ? QColor(112, 228, 209, 150)
            : m_image->sensors.state ==
                static_cast<std::uint8_t>(telemetry::protocol::SensorState::Degraded)
            ? QColor(255, 191, 73, 190) : QColor(255, 92, 75, 210);
        painter.setPen(QPen(rim, m_image->sensors.state == 0U ? 2.8 : 1.8));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(circle.adjusted(2, 2, -2, -2));
        if (m_image->sensors.hasEmp) {
            const double phase = animationMilliseconds() / 1000.0 * 2.0 * Pi;
            const int alpha = std::clamp(static_cast<int>(35.0 +
                30.0 * m_image->sensors.empIntensity * (0.5 + 0.5 * std::sin(phase * 5.0))), 20, 80);
            painter.setPen(QPen(QColor(190, 225, 255, alpha), 1.0));
            for (int band = 0; band < 3; ++band) {
                const double y = center.y() + std::sin(phase + band * 2.1) * radius * 0.62;
                painter.drawLine(QPointF(circle.left() + 12, y), QPointF(circle.right() - 12, y));
            }
        }
    }

    if (!m_display.targetCallout || target.entityId == 0U) return;
    bool docked = targetContact == nullptr && !target.hasStealthPosition;
    QPointF anchor = targetContact != nullptr ? scopeToPixel(targetContact->scopePosition)
                                              : target.hasStealthPosition
                                                    ? scopeToPixel(target.stealthScopePosition)
                                                    : QPointF(circle.right() - 8, circle.top() + 38);
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(std::clamp(static_cast<int>(circle.width() / 52.0), 10, 15));
    painter.setFont(font);
    QFontMetrics metrics(font);
    const double panelWidth = std::clamp(circle.width() * 0.40, 128.0, 245.0);
    const bool compact = circle.width() < 380.0;
    QString name = target.revealedName;
    if (name.isEmpty() && targetContact != nullptr) name = targetContact->revealedName;
    if (name.isEmpty()) name = tr("TARGET");
    QString klass = target.hudTypeLabel;
    if (klass.isEmpty() && targetContact != nullptr) klass = targetContact->hudTypeLabel;
    QStringList lines;
    lines << metrics.elidedText(name, Qt::ElideRight, static_cast<int>(panelWidth - 12));
    if (!compact && !klass.isEmpty())
        lines << metrics.elidedText(klass, Qt::ElideRight, static_cast<int>(panelWidth - 12));
    const qsizetype titleLineCount = lines.size();
    QStringList strengthValues;
    if (m_display.targetStrength && target.hasStrength)
        strengthValues << tr("HULL %1%").arg(qRound(target.hullRatio * 100.0));
    if (m_display.targetStrength && target.hasStrength && target.hasShields && !compact)
        strengthValues << tr("SHIELD %1%").arg(qRound(target.shieldRatio * 100.0));
    if (!strengthValues.isEmpty())
        lines << strengthValues.join(QStringLiteral(" | "));
    QStringList motionValues;
    if (target.hasHudSpeed)
        motionValues << tr("SPD %1 %2").arg(qRound(target.hudSpeed)).arg(trendMarker(target.speedTrend));
    if (target.hasHudDistance)
        motionValues << tr("DIST %1 %2").arg(qRound(target.hudDistance)).arg(trendMarker(target.distanceTrend));
    if (!motionValues.isEmpty())
        lines << motionValues.join(compact ? QStringLiteral("  ") : QStringLiteral(" | "));
    if (m_display.subsystems && !compact) {
        const QString subsystem = !target.lockSubsystemLabel.isEmpty()
            ? target.lockSubsystemLabel : target.targetSubsystemLabel;
        if (!subsystem.isEmpty())
            lines << metrics.elidedText(tr("SUBSYSTEM: %1").arg(subsystem),
                                       Qt::ElideRight, static_cast<int>(panelWidth - 12));
    }
    lines.removeAll(QString{});
    const double lineHeight = metrics.height() + 1.0;
    const double panelHeight = lines.size() * lineHeight + 8.0;
    const double separatorOffset = 4.0 + titleLineCount * lineHeight;
    CalloutLayout callout;
    if (!docked) {
        double bracketExtent = 0.0;
        if (targetContact != nullptr) {
            const double uiScale = std::clamp(circle.width() / 684.0, 0.80, 1.45);
            const double bracketHalf = (contactIconSize(circle.width(), *targetContact) +
                                        10.0 * uiScale) * 0.5 *
                                       (targetContact->lockTarget ? 1.18 : 1.0);
            bracketExtent = bracketHalf * std::sqrt(2.0);
        }
        const QSizeF panelSize(panelWidth, panelHeight);
        callout = calculateCalloutLayout(anchor, bracketExtent, panelSize, separatorOffset,
                                         QRectF(rect()), m_targetDisplay.panelRight,
                                         m_targetDisplay.diagonalUpward);
        if (!callout.valid)
            callout = calculateCalloutLayout(anchor, bracketExtent, panelSize, separatorOffset,
                                             QRectF(rect()), m_targetDisplay.panelRight,
                                             !m_targetDisplay.diagonalUpward);
        if (!callout.valid)
            callout = calculateCalloutLayout(anchor, bracketExtent, panelSize, separatorOffset,
                                             QRectF(rect()), !m_targetDisplay.panelRight,
                                             m_targetDisplay.diagonalUpward);
        if (!callout.valid)
            callout = calculateCalloutLayout(anchor, bracketExtent, panelSize, separatorOffset,
                                             QRectF(rect()), !m_targetDisplay.panelRight,
                                             !m_targetDisplay.diagonalUpward);
        if (callout.valid) {
            if (m_targetDisplay.panelRight != callout.panelRight)
                m_targetDisplay.oppositeZoneSinceMs = -1;
            m_targetDisplay.panelRight = callout.panelRight;
            m_targetDisplay.diagonalUpward = callout.upward;
        } else {
            docked = true;
        }
    }
    QRectF panel = callout.panel;
    if (docked) {
        const double panelX = std::clamp(circle.right() - panelWidth - 4.0,
                                         static_cast<double>(rect().left() + 4),
                                         static_cast<double>(rect().right()) - panelWidth - 4.0);
        const double panelY = std::clamp(circle.top() + 4.0,
                                         static_cast<double>(rect().top() + 4),
                                         static_cast<double>(rect().bottom()) - panelHeight - 4.0);
        panel = QRectF(panelX, panelY, panelWidth, panelHeight);
    }
    painter.fillRect(panel, QColor(2, 8, 11, 205));
    painter.setPen(QPen(accent, 1.3));
    const double separatorY = panel.top() + separatorOffset;
    if (!docked && callout.valid) {
        painter.drawLine(callout.diagonal);
        painter.drawLine(callout.shoulder);
    }
    painter.drawLine(QPointF(panel.left(), separatorY),
                     QPointF(panel.right(), separatorY));
    for (qsizetype index = 0; index < lines.size(); ++index) {
        painter.setPen(index == 0 ? accent : LabelColor);
        painter.drawText(QRectF(panel.left() + 6, panel.top() + 4 + index * lineHeight,
                                panel.width() - 12, lineHeight),
                         Qt::AlignLeft | Qt::AlignVCenter, lines.at(index));
    }
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

    if (m_image) {
        for (const RadarContact& contact : m_image->contacts) {
            if (contact.invalid || contact.id == m_image->playerEntityId) continue;
            const QPointF position(center.x() + contact.scopePosition.x() * radius,
                                   center.y() + contact.scopePosition.y() * radius);
            drawContact(painter, contact, position, circle.width());
        }
        drawEnhancedLayers(painter, circle, center, radius);
    }

    const SystemOverlayPresentation overlay = systemOverlay(m_status);
    if (overlay.visible) {
        painter.fillRect(rect(), QColor(0, 0, 0, m_status == ClientStatus::Stale ? 105 : 145));
        const QRectF safeBounds = QRectF(rect()).adjusted(20, 20, -20, -20);
        int titleScale = std::clamp(qRound(width() / 360.0), 1, 3);
        while (titleScale > 1 &&
               m_overlayTitleFont.textSize(overlay.title, titleScale).width() > safeBounds.width()) {
            --titleScale;
        }
        const int detailScale = titleScale;
        const QSize titleSize = m_overlayTitleFont.textSize(overlay.title, titleScale);
        const QSize detailSize = m_overlayDetailFont.textSize(overlay.detail, detailScale);
        const double gap = 7.0 * titleScale;
        const double blockHeight = titleSize.height() + gap + detailSize.height();
        const double top = safeBounds.center().y() - blockHeight * 0.5;
        const double pulse = 0.92 + 0.08 *
            (0.5 + 0.5 * std::sin(animationMilliseconds() / 1000.0 * 2.0 * Pi * 0.65));
        QColor titleColor = overlay.color;
        titleColor.setAlphaF(titleColor.alphaF() * pulse);
        QColor detailColor = overlay.color;
        detailColor.setAlphaF(detailColor.alphaF() * 0.70);

        const QRectF titleBounds(safeBounds.left(), top, safeBounds.width(), titleSize.height());
        const QRectF detailBounds(safeBounds.left(), top + titleSize.height() + gap,
                                  safeBounds.width(), detailSize.height());
        const bool titleDrawn = m_overlayTitleFont.drawText(
            painter, titleBounds, Qt::AlignCenter, overlay.title, titleColor, titleScale);
        const bool detailDrawn = m_overlayDetailFont.drawText(
            painter, detailBounds, Qt::AlignCenter, overlay.detail, detailColor, detailScale);
        if (!titleDrawn || !detailDrawn) {
            QFont fallback = labels;
            fallback.setPixelSize(std::clamp(static_cast<int>(width() / 22), 16, 42));
            fallback.setBold(true);
            painter.setFont(fallback);
            painter.setPen(titleColor);
            painter.drawText(safeBounds, Qt::AlignCenter,
                             overlay.title + QLatin1Char('\n') + overlay.detail);
        }
    }
}

} // namespace simpit::radar

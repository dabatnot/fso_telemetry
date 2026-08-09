#include "radar_widget.h"

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
    update();
}

void RadarWidget::setStatus(ClientStatus status, const QString& detail)
{
    m_status = status;
    m_detail = detail;
    update();
}

void RadarWidget::drawContact(QPainter& painter, const RadarContact& contact,
                              const QPointF& position) const
{
    painter.save();
    painter.translate(position);
    painter.setOpacity(contact.alpha());
    QPen pen(contact.color);
    pen.setWidthF(contact.currentTarget ? 2.6 : 1.5);
    pen.setCapStyle(Qt::SquareCap);
    pen.setJoinStyle(Qt::MiterJoin);
    if (contact.visibility == 2U) pen.setDashPattern({3.0, 3.0});
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const bool bomb = (contact.flags & 0x20U) != 0U;
    const bool threat = (contact.flags & 0x80U) != 0U;
    const double size = contact.currentTarget ? 8.0 : (threat || bomb) ? 6.0 : 4.5;
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

    if (contact.currentTarget) {
        QPainterPath brackets;
        brackets.moveTo(-12, -12); brackets.lineTo(-6, -12); brackets.moveTo(-12, -12); brackets.lineTo(-12, -6);
        brackets.moveTo(12, -12); brackets.lineTo(6, -12); brackets.moveTo(12, -12); brackets.lineTo(12, -6);
        brackets.moveTo(-12, 12); brackets.lineTo(-6, 12); brackets.moveTo(-12, 12); brackets.lineTo(-12, 6);
        brackets.moveTo(12, 12); brackets.lineTo(6, 12); brackets.moveTo(12, 12); brackets.lineTo(12, 6);
        painter.drawPath(brackets);
    }
    if (std::abs(contact.elevationRadians) > 0.04) {
        const double direction = contact.elevationRadians > 0.0 ? -1.0 : 1.0;
        QPainterPath chevron;
        chevron.moveTo(-3, direction * 10); chevron.lineTo(0, direction * 13);
        chevron.lineTo(3, direction * 10);
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
            drawContact(painter, contact, position);
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


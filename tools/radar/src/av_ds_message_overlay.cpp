#include "av_ds_message_overlay.h"

#include <QFontDatabase>
#include <QPainter>
#include <QPaintEvent>

#include <algorithm>

namespace simpit::radar {

AvDsMessageOverlay::AvDsMessageOverlay(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
}

void AvDsMessageOverlay::setMessage(const AvDsMessage& message)
{
    m_message = message;
    setVisible(m_message.visible);
    update();
}

void AvDsMessageOverlay::paintEvent(QPaintEvent*)
{
    if (!m_message.visible) return;

    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    painter.fillRect(rect(), QColor(0, 0, 0, m_message.dimsContent ? 105 : 145));
    const QRectF safeBounds = QRectF(rect()).adjusted(20, 20, -20, -20);
    int titleScale = std::clamp(qRound(width() / 360.0), 1, 3);
    while (titleScale > 1 &&
           m_titleFont.textSize(m_message.title, titleScale).width() > safeBounds.width()) {
        --titleScale;
    }
    const QSize titleSize = m_titleFont.textSize(m_message.title, titleScale);
    const QSize detailSize = m_detailFont.textSize(m_message.detail, titleScale);
    const double gap = 7.0 * titleScale;
    const double top = safeBounds.center().y() - (titleSize.height() + gap + detailSize.height()) * 0.5;
    const QRectF titleBounds(safeBounds.left(), top, safeBounds.width(), titleSize.height());
    const QRectF detailBounds(safeBounds.left(), top + titleSize.height() + gap,
                              safeBounds.width(), detailSize.height());
    if (m_titleFont.drawText(painter, titleBounds, Qt::AlignCenter, m_message.title,
                             m_message.color, titleScale) &&
        m_detailFont.drawText(painter, detailBounds, Qt::AlignCenter, m_message.detail,
                              m_message.color, titleScale)) return;

    QFont fallback = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    fallback.setPixelSize(std::clamp(static_cast<int>(width() / 22), 16, 42));
    fallback.setBold(true);
    painter.setFont(fallback);
    painter.setPen(m_message.color);
    painter.drawText(safeBounds, Qt::AlignCenter,
                     m_message.title + QLatin1Char('\n') + m_message.detail);
}

} // namespace simpit::radar

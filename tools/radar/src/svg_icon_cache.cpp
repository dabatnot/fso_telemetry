#include "svg_icon_cache.h"

#include <QColor>
#include <QDebug>
#include <QFile>
#include <QPainter>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>

extern int qInitResources_radar_icons();

namespace {
const int RadarIconResourcesInitialized = qInitResources_radar_icons();
}

namespace simpit::radar {

bool SvgIconCache::resourceIsValid(RadarIconAsset asset) const
{
    const QString path = QString::fromLatin1(radarIconResourcePath(asset));
    if (path.isEmpty() || !QFile::exists(path)) return false;
    QSvgRenderer renderer(path);
    const QRectF box = renderer.viewBoxF();
    return renderer.isValid() && std::abs(box.width() - 32.0) < 0.01 &&
           std::abs(box.height() - 32.0) < 0.01;
}

QImage SvgIconCache::mask(
    RadarIconAsset asset, int physicalSize, double devicePixelRatio)
{
    physicalSize = std::clamp(physicalSize, 1, 256);
    const auto dprKey = static_cast<quint64>(
        std::clamp(qRound64(std::max(1.0, devicePixelRatio) * 1024.0), 1024LL, 65535LL));
    const quint64 key =
        (static_cast<quint64>(static_cast<std::uint8_t>(asset)) << 48U) |
        (static_cast<quint64>(physicalSize) << 32U) | dprKey;
    const auto found = m_masks.constFind(key);
    if (found != m_masks.cend()) return found.value();
    if (m_warned.contains(static_cast<std::uint8_t>(asset))) return {};

    const QString path = QString::fromLatin1(radarIconResourcePath(asset));
    QSvgRenderer renderer(path);
    if (!renderer.isValid()) {
        warnInvalid(asset);
        return {};
    }
    QImage rendered(physicalSize, physicalSize, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, QRectF(0.0, 0.0, physicalSize, physicalSize));
    painter.end();
    bool hasAlpha = false;
    for (int y = 0; y < rendered.height() && !hasAlpha; ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(rendered.constScanLine(y));
        for (int x = 0; x < rendered.width(); ++x) {
            if (qAlpha(line[x]) != 0) {
                hasAlpha = true;
                break;
            }
        }
    }
    if (rendered.isNull() || !hasAlpha) {
        warnInvalid(asset);
        return {};
    }
    m_masks.insert(key, rendered);
    return rendered;
}

bool SvgIconCache::draw(QPainter& painter,
                        RadarIconAsset asset,
                        const QPointF& center,
                        double logicalSize,
                        const QColor& color,
                        double devicePixelRatio)
{
    if (asset == RadarIconAsset::None || !std::isfinite(logicalSize) || logicalSize <= 0.0)
        return false;
    const int physicalSize = std::max(1, qRound(logicalSize * std::max(1.0, devicePixelRatio)));
    QImage tinted = mask(asset, physicalSize, devicePixelRatio);
    if (tinted.isNull()) return false;
    {
        QPainter tint(&tinted);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(tinted.rect(), color);
    }
    const QRectF target(center.x() - logicalSize / 2.0,
                        center.y() - logicalSize / 2.0,
                        logicalSize, logicalSize);
    painter.drawImage(target, tinted);
    return true;
}

void SvgIconCache::warnInvalid(RadarIconAsset asset)
{
    const auto key = static_cast<std::uint8_t>(asset);
    if (m_warned.contains(key)) return;
    m_warned.insert(key);
    qWarning().noquote() << QStringLiteral("Icône radar SVG invalide : %1")
        .arg(QString::fromLatin1(radarIconResourcePath(asset)));
}

void SvgIconCache::clear()
{
    m_masks.clear();
    m_warned.clear();
}

} // namespace simpit::radar

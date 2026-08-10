#pragma once

#include "radar_icons.h"

#include <QHash>
#include <QImage>
#include <QSet>

class QColor;
class QPainter;
class QPointF;

namespace simpit::radar {

class SvgIconCache final {
public:
    bool draw(QPainter& painter,
              RadarIconAsset asset,
              const QPointF& center,
              double logicalSize,
              const QColor& color,
              double devicePixelRatio);
    bool resourceIsValid(RadarIconAsset asset) const;
    void clear();

private:
    QImage mask(RadarIconAsset asset, int physicalSize, double devicePixelRatio);
    void warnInvalid(RadarIconAsset asset);

    QHash<quint64, QImage> m_masks;
    QSet<std::uint8_t> m_warned;
};

} // namespace simpit::radar

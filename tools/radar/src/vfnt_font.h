#pragma once

#include <QHash>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QVector>

class QPainter;

namespace simpit::radar {

// Minimal reader for the legacy FreeSpace VFNT bitmap format.  It deliberately
// supports only the immutable rendering operations needed by the system
// overlays; game text layout and the Target Callout remain Qt-rendered.
class VfntFont final {
public:
    VfntFont() = default;
    explicit VfntFont(const QString& resourcePath);

    bool load(const QString& resourcePath);
    bool isValid() const noexcept { return m_valid; }
    int nativeHeight() const noexcept { return m_height; }
    QSize textSize(const QString& text, int scale = 1) const;
    bool drawText(QPainter& painter, const QRectF& bounds, Qt::Alignment alignment,
                  const QString& text, const QColor& color, int scale = 1) const;

private:
    struct Glyph final {
        int spacing = 0;
        int width = 0;
        int offset = 0;
        qint16 kerningEntry = -1;
    };

    struct KernPair final {
        qint8 first = 0;
        qint8 second = 0;
        qint8 offset = 0;
    };

    int glyphIndex(uchar character) const noexcept;
    int glyphSpacing(int glyph, uchar next) const noexcept;
    QImage textMask(const QString& text) const;

    bool m_valid = false;
    int m_firstAscii = 0;
    int m_defaultWidth = 0;
    int m_height = 0;
    QVector<Glyph> m_glyphs;
    QVector<KernPair> m_kernPairs;
    QByteArray m_pixels;
    mutable QHash<QString, QImage> m_maskCache;
};

} // namespace simpit::radar

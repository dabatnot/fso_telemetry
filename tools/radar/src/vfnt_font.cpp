#include "vfnt_font.h"

#include <QBuffer>
#include <QDataStream>
#include <QFile>
#include <QPainter>
#include <QResource>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

static void ensureRadarFontResources()
{
    Q_INIT_RESOURCE(radar_fonts);
}

namespace simpit::radar {

VfntFont::VfntFont(const QString& resourcePath)
{
    load(resourcePath);
}

bool VfntFont::load(const QString& resourcePath)
{
    ensureRadarFontResources();
    m_valid = false;
    m_firstAscii = 0;
    m_defaultWidth = 0;
    m_height = 0;
    m_glyphs.clear();
    m_kernPairs.clear();
    m_pixels.clear();
    m_maskCache.clear();

    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 40 || bytes.first(4) != QByteArrayLiteral("VFNT")) return false;

    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly) || !buffer.seek(4)) return false;
    QDataStream stream(&buffer);
    stream.setByteOrder(QDataStream::LittleEndian);

    qint32 version = 0;
    qint32 numChars = 0;
    qint32 firstAscii = 0;
    qint32 defaultWidth = 0;
    qint32 height = 0;
    qint32 numKernPairs = 0;
    qint32 kernDataSize = 0;
    qint32 charDataSize = 0;
    qint32 pixelDataSize = 0;
    stream >> version >> numChars >> firstAscii >> defaultWidth >> height
           >> numKernPairs >> kernDataSize >> charDataSize >> pixelDataSize;

    constexpr int MaximumCharacters = 512;
    constexpr int MaximumKernPairs = 8192;
    constexpr int MaximumPixels = 4 * 1024 * 1024;
    if (stream.status() != QDataStream::Ok || version < 0 ||
        numChars <= 0 || numChars > MaximumCharacters ||
        firstAscii < 0 || firstAscii > 255 || defaultWidth <= 0 ||
        height <= 0 || height > 256 ||
        numKernPairs < 0 || numKernPairs > MaximumKernPairs ||
        kernDataSize != numKernPairs * 3 || charDataSize != numChars * 16 ||
        pixelDataSize <= 0 || pixelDataSize > MaximumPixels) {
        return false;
    }

    QVector<KernPair> kernPairs;
    kernPairs.reserve(numKernPairs);
    for (int index = 0; index < numKernPairs; ++index) {
        KernPair pair;
        stream >> pair.first >> pair.second >> pair.offset;
        kernPairs.push_back(pair);
    }

    QVector<Glyph> glyphs;
    glyphs.reserve(numChars);
    for (int index = 0; index < numChars; ++index) {
        Glyph glyph;
        qint16 userData = 0;
        stream >> glyph.spacing >> glyph.width >> glyph.offset
               >> glyph.kerningEntry >> userData;
        if (glyph.spacing < 0 || glyph.width < 0 || glyph.width > 256 ||
            glyph.offset < 0 || glyph.kerningEntry < -1 ||
            glyph.kerningEntry >= numKernPairs) {
            return false;
        }
        const qint64 glyphEnd = static_cast<qint64>(glyph.offset) +
                                static_cast<qint64>(glyph.width) * height;
        if (glyphEnd > pixelDataSize) return false;
        glyphs.push_back(glyph);
    }

    QByteArray pixels(pixelDataSize, Qt::Uninitialized);
    if (stream.readRawData(pixels.data(), pixelDataSize) != pixelDataSize ||
        stream.status() != QDataStream::Ok || !buffer.atEnd()) {
        return false;
    }

    m_firstAscii = firstAscii;
    m_defaultWidth = defaultWidth;
    m_height = height;
    m_glyphs = std::move(glyphs);
    m_kernPairs = std::move(kernPairs);
    m_pixels = std::move(pixels);
    m_valid = true;
    return true;
}

int VfntFont::glyphIndex(uchar character) const noexcept
{
    const int index = static_cast<int>(character) - m_firstAscii;
    return index >= 0 && index < m_glyphs.size() ? index : -1;
}

int VfntFont::glyphSpacing(int glyph, uchar next) const noexcept
{
    if (glyph < 0 || glyph >= m_glyphs.size()) return m_defaultWidth;
    int spacing = m_glyphs.at(glyph).spacing;
    int pairIndex = m_glyphs.at(glyph).kerningEntry;
    const int nextGlyph = glyphIndex(next);
    if (pairIndex < 0 || next == 0 || next == '\n' || nextGlyph < 0) return spacing;
    while (pairIndex < m_kernPairs.size()) {
        const auto& pair = m_kernPairs.at(pairIndex);
        if (static_cast<uchar>(pair.first) != static_cast<uchar>(glyph) ||
            static_cast<uchar>(pair.second) >= static_cast<uchar>(nextGlyph)) {
            if (static_cast<uchar>(pair.first) == static_cast<uchar>(glyph) &&
                static_cast<uchar>(pair.second) == static_cast<uchar>(nextGlyph)) {
                spacing += pair.offset;
            }
            break;
        }
        ++pairIndex;
    }
    return spacing;
}

QSize VfntFont::textSize(const QString& text, int scale) const
{
    if (!m_valid || text.isEmpty()) return {};
    scale = std::max(1, scale);
    const QByteArray latin = text.toLatin1();
    int penX = 0;
    int lineExtent = 0;
    int maximumWidth = 0;
    int lines = 1;
    for (qsizetype index = 0; index < latin.size(); ++index) {
        const uchar character = static_cast<uchar>(latin.at(index));
        if (character == '\n') {
            maximumWidth = std::max(maximumWidth, std::max(penX, lineExtent));
            penX = 0;
            lineExtent = 0;
            ++lines;
            continue;
        }
        const int glyph = glyphIndex(character);
        if (glyph >= 0) lineExtent = std::max(lineExtent, penX + m_glyphs.at(glyph).width);
        const uchar next = index + 1 < latin.size()
            ? static_cast<uchar>(latin.at(index + 1)) : 0U;
        penX += glyphSpacing(glyph, next);
    }
    maximumWidth = std::max(maximumWidth, std::max(penX, lineExtent));
    return QSize(maximumWidth * scale, lines * m_height * scale);
}

QImage VfntFont::textMask(const QString& text) const
{
    const auto cached = m_maskCache.constFind(text);
    if (cached != m_maskCache.cend()) return cached.value();
    const QSize size = textSize(text);
    if (size.isEmpty()) return {};

    QImage mask(size, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    const QByteArray latin = text.toLatin1();
    int x = 0;
    int y = 0;
    for (qsizetype index = 0; index < latin.size(); ++index) {
        const uchar character = static_cast<uchar>(latin.at(index));
        if (character == '\n') {
            x = 0;
            y += m_height;
            continue;
        }
        const int glyphIndexValue = glyphIndex(character);
        const uchar next = index + 1 < latin.size()
            ? static_cast<uchar>(latin.at(index + 1)) : 0U;
        if (glyphIndexValue >= 0) {
            const auto& glyph = m_glyphs.at(glyphIndexValue);
            for (int row = 0; row < m_height; ++row) {
                auto* scanLine = reinterpret_cast<QRgb*>(mask.scanLine(y + row));
                for (int column = 0; column < glyph.width; ++column) {
                    const int pixelIndex = glyph.offset + row * glyph.width + column;
                    const int intensity = std::clamp(
                        static_cast<int>(static_cast<uchar>(m_pixels.at(pixelIndex))), 0, 14);
                    const int alpha = intensity * 17;
                    scanLine[x + column] = qRgba(alpha, alpha, alpha, alpha);
                }
            }
        }
        x += glyphSpacing(glyphIndexValue, next);
    }
    m_maskCache.insert(text, mask);
    return mask;
}

bool VfntFont::drawText(QPainter& painter, const QRectF& bounds, Qt::Alignment alignment,
                        const QString& text, const QColor& color, int scale) const
{
    if (!m_valid || text.isEmpty() || !color.isValid()) return false;
    scale = std::max(1, scale);
    QImage tinted = textMask(text);
    if (tinted.isNull()) return false;
    {
        QPainter tintPainter(&tinted);
        tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tintPainter.fillRect(tinted.rect(), color);
    }

    const QSizeF renderedSize(tinted.width() * scale, tinted.height() * scale);
    double x = bounds.left();
    double y = bounds.top();
    if (alignment.testFlag(Qt::AlignHCenter))
        x = bounds.center().x() - renderedSize.width() * 0.5;
    else if (alignment.testFlag(Qt::AlignRight))
        x = bounds.right() - renderedSize.width();
    if (alignment.testFlag(Qt::AlignVCenter))
        y = bounds.center().y() - renderedSize.height() * 0.5;
    else if (alignment.testFlag(Qt::AlignBottom))
        y = bounds.bottom() - renderedSize.height();

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(QRectF(QPointF(std::round(x), std::round(y)), renderedSize), tinted);
    painter.restore();
    return true;
}

} // namespace simpit::radar

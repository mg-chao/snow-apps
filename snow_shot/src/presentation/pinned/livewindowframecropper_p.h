// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_LIVEWINDOWFRAMECROPPER_P_H
#define SNOW_SHOT_PRESENTATION_LIVEWINDOWFRAMECROPPER_P_H

#include "livewindownative_p.h"

#include <QColorSpace>
#include <QPainter>
#include <array>
#include <cstring>
#include <utility>

namespace snow_shot::presentation::live_window_detail {

// Worker-owned storage. Readers retain immutable images; only exclusively owned
// buffers can be overwritten. Two reusable crops bound idle retained capacity.
class FrameCropper {
  public:
    void clear() {
        m_buffers = {};
    }

    QImage crop(const QImage& image, const QRectF& desktopBounds, const QRectF& crop,
                const QSize& pixels) {
        if (image.isNull() || desktopBounds.isEmpty() || crop.isEmpty() || pixels.isEmpty())
            return {};
        const QRectF available(QPointF(), desktopBounds.size());
        if (crop == available && pixels == image.size()) {
            clear();
            return image;
        }

        QImage result;
        QImage* reusable = nullptr;
        for (auto& buffer : m_buffers) {
            if (buffer.isNull() || buffer.isDetached()) {
                reusable = &buffer;
                if (buffer.size() == pixels)
                    result = std::move(buffer);
                break;
            }
        }
        if (result.isNull())
            result = QImage(pixels, QImage::Format_RGBA8888);
        if (result.isNull())
            return {};
        result.setColorSpace(image.colorSpace());
        const auto mapping = cropMapping(crop, available, QRectF(QPointF(), pixels));
        const qreal scaleX = image.width() / desktopBounds.width();
        const qreal scaleY = image.height() / desktopBounds.height();
        const QRectF source(mapping.source.x() * scaleX, mapping.source.y() * scaleY,
                            mapping.source.width() * scaleX, mapping.source.height() * scaleY);
        const QRect sourcePixels = source.toRect();
        const QRect destinationPixels = mapping.destination.toRect();
        const bool direct =
            image.format() == QImage::Format_RGBA8888 && source == QRectF(sourcePixels) &&
            mapping.destination == QRectF(destinationPixels) &&
            sourcePixels.size() == destinationPixels.size() &&
            image.rect().contains(sourcePixels) && result.rect().contains(destinationPixels);
        if (!direct || destinationPixels != result.rect())
            result.fill(Qt::transparent);
        if (!mapping.source.isEmpty()) {
            if (direct) {
                for (int y = 0; y < sourcePixels.height(); ++y)
                    std::memcpy(result.scanLine(destinationPixels.y() + y) +
                                    qsizetype(destinationPixels.x()) * 4,
                                image.constScanLine(sourcePixels.y() + y) +
                                    qsizetype(sourcePixels.x()) * 4,
                                size_t(sourcePixels.width()) * 4);
            } else {
                QPainter painter(&result);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.drawImage(mapping.destination, image, source);
            }
        }
        if (reusable)
            *reusable = result;
        return result;
    }

  private:
    std::array<QImage, 2> m_buffers;
};

} // namespace snow_shot::presentation::live_window_detail
#endif

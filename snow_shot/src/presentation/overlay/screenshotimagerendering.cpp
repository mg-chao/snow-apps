#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/presentation/screenshotimagerendering.h"

#include <QColorSpace>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QRegion>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {
constexpr qreal kMaximumRasterSourceCoordinate = 32768.0;
constexpr qreal kMaximumRasterSourceScale = 16384.0;
constexpr qreal kMaximumSourceChunkSpan = 2048.0;
constexpr qreal kMinimumSourceSamplingPadding = 1.0;

bool finiteRect(const QRectF& rect) {
    return std::isfinite(rect.left()) && std::isfinite(rect.top()) && std::isfinite(rect.width()) &&
           std::isfinite(rect.height());
}

QImage imageWindow(const QImage& image, const QRect& bounds) {
    if (image.isNull() || bounds.isEmpty() || !image.rect().contains(bounds)) {
        return {};
    }

    // A read-only QImage view keeps the source pixels shared while rebasing the coordinates.
    // Screenshot images are normally 32-bit; copy is retained for indexed and packed formats
    // whose palette or bit offset cannot be represented by the public QImage view constructor.
    const int depth = image.depth();
    if (depth > 0 && depth % 8 == 0 && image.colorTable().isEmpty()) {
        const int bytesPerPixel = depth / 8;
        const uchar* data = image.constScanLine(bounds.top()) +
                            static_cast<qsizetype>(bounds.left()) * bytesPerPixel;
        const qsizetype byteOffset = data - image.constBits();
        const qsizetype availableBytes = image.sizeInBytes() - byteOffset;
        const qsizetype requiredBytes = image.bytesPerLine() * bounds.height();
        const bool scanLinesAligned = reinterpret_cast<quintptr>(data) % alignof(quint32) == 0;
        if (scanLinesAligned && requiredBytes <= availableBytes) {
            QImage view(data, bounds.width(), bounds.height(), image.bytesPerLine(),
                        image.format());
            if (!view.isNull()) {
                view.setColorSpace(image.colorSpace());
                view.setDevicePixelRatio(1.0);
                return view;
            }
        }
    }

    QImage copy = snowCanvasCopyImage(image, bounds);
    if (!copy.isNull()) {
        copy.setDevicePixelRatio(1.0);
    }
    return copy;
}

bool sourceMappingFitsRaster(const QRectF& source, qreal sourcePerTargetX, qreal sourcePerTargetY) {
    return finiteRect(source) && source.left() >= 0.0 && source.top() >= 0.0 &&
           source.right() < kMaximumRasterSourceCoordinate &&
           source.bottom() < kMaximumRasterSourceCoordinate && sourcePerTargetX > 0.0 &&
           sourcePerTargetY > 0.0 && sourcePerTargetX <= kMaximumRasterSourceScale &&
           sourcePerTargetY <= kMaximumRasterSourceScale;
}

void paintScaledSourceWindow(QPainter& painter, const QRectF& targetWindow, const QImage& image,
                             const QRect& sourceBounds) {
    if (targetWindow.isEmpty() || sourceBounds.isEmpty()) {
        return;
    }
    const QImage sourceWindow = imageWindow(image, sourceBounds);
    if (sourceWindow.isNull()) {
        return;
    }

    // Scale in QImage, whose transform path does not use the raster paint engine's 16.16
    // source-coordinate representation. The resulting image is deliberately bounded so the
    // final painter call itself remains inside that representation as well.
    const QRectF deviceWindow = painter.deviceTransform().mapRect(targetWindow);
    constexpr int kMaximumScaledDimension = static_cast<int>(kMaximumRasterSourceCoordinate) - 4;
    const int scaledWidth =
        std::clamp(qCeil(std::abs(deviceWindow.width())), 1, kMaximumScaledDimension);
    const int scaledHeight =
        std::clamp(qCeil(std::abs(deviceWindow.height())), 1, kMaximumScaledDimension);
    const Qt::TransformationMode mode = painter.testRenderHint(QPainter::SmoothPixmapTransform)
                                            ? Qt::SmoothTransformation
                                            : Qt::FastTransformation;
    QImage scaled = snowCanvasScaleImage(sourceWindow, QSize(scaledWidth, scaledHeight),
                                         Qt::IgnoreAspectRatio, mode);
    if (scaled.isNull()) {
        return;
    }
    scaled.setDevicePixelRatio(1.0);
    painter.drawImage(targetWindow, scaled, QRectF(scaled.rect()));
}

} // namespace

void paintExposedScreenshotImage(QPainter& painter, const QRectF& targetRect, const QImage& image,
                                 const QRectF& sourceRect, const QRegion& exposedRegion) {
    if (image.isNull() || !finiteRect(targetRect) || !targetRect.isValid() ||
        targetRect.isEmpty() || !finiteRect(sourceRect) || !sourceRect.isValid() ||
        sourceRect.isEmpty() || exposedRegion.isEmpty()) {
        return;
    }

    const qreal sourcePerTargetX = sourceRect.width() / targetRect.width();
    const qreal sourcePerTargetY = sourceRect.height() / targetRect.height();
    if (!std::isfinite(sourcePerTargetX) || !std::isfinite(sourcePerTargetY) ||
        sourcePerTargetX <= 0.0 || sourcePerTargetY <= 0.0) {
        return;
    }

    const QRectF drawableSource = sourceRect.intersected(QRectF(image.rect()));
    if (!drawableSource.isValid() || drawableSource.isEmpty()) {
        return;
    }
    const QRectF drawableTarget(
        targetRect.left() + (drawableSource.left() - sourceRect.left()) / sourcePerTargetX,
        targetRect.top() + (drawableSource.top() - sourceRect.top()) / sourcePerTargetY,
        drawableSource.width() / sourcePerTargetX, drawableSource.height() / sourcePerTargetY);
    if (!drawableTarget.isValid() || drawableTarget.isEmpty()) {
        return;
    }

    // Keep the ordinary path as a single draw. The explicit clip makes this
    // correct even when a caller supplies a damage region without clipping its painter first.
    if (sourceMappingFitsRaster(sourceRect, sourcePerTargetX, sourcePerTargetY)) {
        painter.save();
        painter.setClipRegion(exposedRegion, Qt::IntersectClip);
        painter.drawImage(targetRect, image, sourceRect);
        painter.restore();
        return;
    }

    const auto sourceForTarget = [&](const QRectF& target) {
        return QRectF(sourceRect.left() + (target.left() - targetRect.left()) * sourcePerTargetX,
                      sourceRect.top() + (target.top() - targetRect.top()) * sourcePerTargetY,
                      target.width() * sourcePerTargetX, target.height() * sourcePerTargetY);
    };
    const auto targetForSource = [&](const QRectF& source) {
        return QRectF(targetRect.left() + (source.left() - sourceRect.left()) / sourcePerTargetX,
                      targetRect.top() + (source.top() - sourceRect.top()) / sourcePerTargetY,
                      source.width() / sourcePerTargetX, source.height() / sourcePerTargetY);
    };
    const bool smoothSampling = painter.testRenderHint(QPainter::SmoothPixmapTransform);
    const qreal samplingPaddingX =
        smoothSampling ? std::max(kMinimumSourceSamplingPadding, sourcePerTargetX) : 0.0;
    const qreal samplingPaddingY =
        smoothSampling ? std::max(kMinimumSourceSamplingPadding, sourcePerTargetY) : 0.0;

    for (const QRect& exposedRectangle : exposedRegion) {
        const QRectF exposedTarget = drawableTarget.intersected(QRectF(exposedRectangle));
        if (!exposedTarget.isValid() || exposedTarget.isEmpty()) {
            continue;
        }

        const QRectF exposedSource = sourceForTarget(exposedTarget).intersected(drawableSource);
        if (!exposedSource.isValid() || exposedSource.isEmpty()) {
            continue;
        }

        // Keep each source chunk bounded in both dimensions. A ratio above the signed 16.16
        // increment limit is handled by QImage::scaled below, so that axis does not need to be
        // split into sub-pixel target cells.
        const int chunkCountX =
            sourcePerTargetX > kMaximumRasterSourceScale
                ? 1
                : qMax(1, qCeil(exposedSource.width() / kMaximumSourceChunkSpan));
        const int chunkCountY =
            sourcePerTargetY > kMaximumRasterSourceScale
                ? 1
                : qMax(1, qCeil(exposedSource.height() / kMaximumSourceChunkSpan));

        painter.save();
        painter.setClipRect(exposedRectangle, Qt::IntersectClip);
        for (int chunkY = 0; chunkY < chunkCountY; ++chunkY) {
            const qreal sourceTop =
                exposedSource.top() + exposedSource.height() * chunkY / chunkCountY;
            const qreal sourceBottom =
                exposedSource.top() + exposedSource.height() * (chunkY + 1) / chunkCountY;
            for (int chunkX = 0; chunkX < chunkCountX; ++chunkX) {
                const qreal sourceLeft =
                    exposedSource.left() + exposedSource.width() * chunkX / chunkCountX;
                const qreal sourceRight =
                    exposedSource.left() + exposedSource.width() * (chunkX + 1) / chunkCountX;
                const QRectF chunkSource(sourceLeft, sourceTop, sourceRight - sourceLeft,
                                         sourceBottom - sourceTop);
                const QRectF chunkTarget = targetForSource(chunkSource).intersected(exposedTarget);
                if (!chunkTarget.isValid() || chunkTarget.isEmpty()) {
                    continue;
                }

                const QRectF sampleSource = chunkSource
                                                .adjusted(-samplingPaddingX, -samplingPaddingY,
                                                          samplingPaddingX, samplingPaddingY)
                                                .intersected(drawableSource);
                const QRect sourceBounds = sampleSource.toAlignedRect().intersected(image.rect());
                if (!sampleSource.isValid() || sampleSource.isEmpty() || sourceBounds.isEmpty()) {
                    continue;
                }
                const QRectF sampleTarget = targetForSource(sampleSource);

                painter.save();
                painter.setClipRect(chunkTarget, Qt::IntersectClip);
                if (sourcePerTargetX > kMaximumRasterSourceScale ||
                    sourcePerTargetY > kMaximumRasterSourceScale) {
                    // This also handles a single target pixel representing tens of thousands of
                    // source pixels; splitting that target pixel cannot make the 16.16 increment
                    // finite, while QImage's scaler can reduce the source safely.
                    const QRectF targetWindow = targetForSource(QRectF(sourceBounds));
                    paintScaledSourceWindow(painter, targetWindow, image, sourceBounds);
                } else if (sourceMappingFitsRaster(sampleSource, sourcePerTargetX,
                                                   sourcePerTargetY)) {
                    painter.drawImage(sampleTarget, image, sampleSource);
                } else {
                    const QImage boundedSource = imageWindow(image, sourceBounds);
                    if (!boundedSource.isNull()) {
                        painter.drawImage(sampleTarget, boundedSource,
                                          sampleSource.translated(-sourceBounds.topLeft()));
                    }
                }
                painter.restore();
            }
        }
        painter.restore();
    }
}

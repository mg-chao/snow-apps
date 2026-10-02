#include "snow_canvas_background_restore.h"

#include "snow_canvas_pen_mask_atlas.h"
#include "snow_canvas_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"

#include <QPainter>
#include <QFont>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace snow_canvas_background_restore {
namespace {

void hashValue(std::uint64_t& hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
}

void hashDouble(std::uint64_t& hash, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hashValue(hash, bits);
}

std::uint64_t contentKey(const snow_canvas_renderer::SceneRenderRequest& request, qreal dpr) {
    const auto& info = *request.displayInfo;
    std::uint64_t hash = 1469598103934665603ULL;
    hashDouble(hash, info.surface_width);
    hashDouble(hash, info.surface_height);
    hashDouble(hash, info.camera_center_x);
    hashDouble(hash, info.camera_center_y);
    hashDouble(hash, info.camera_zoom);
    hashDouble(hash, dpr);
    hashValue(hash, static_cast<std::uint64_t>(request.painter->renderHints()));
    hashValue(hash, static_cast<std::uint64_t>(qHash(request.painter->font().toString())));
    if (request.backgroundContext != nullptr) {
        const auto& transform = request.backgroundContext->canvasToViewTransform;
        hashDouble(hash, transform.m11());
        hashDouble(hash, transform.m12());
        hashDouble(hash, transform.m21());
        hashDouble(hash, transform.m22());
        hashDouble(hash, transform.dx());
        hashDouble(hash, transform.dy());
    }
    hashValue(hash, static_cast<std::uint64_t>(info.clear_color.r) |
                        (static_cast<std::uint64_t>(info.clear_color.g) << 8) |
                        (static_cast<std::uint64_t>(info.clear_color.b) << 16) |
                        (static_cast<std::uint64_t>(info.clear_color.a) << 24));
    hashValue(hash, request.clearBackgroundEnabled ? 1 : 0);
    hashValue(hash, reinterpret_cast<std::uintptr_t>(request.backgroundRenderer));
    if (request.backgroundRenderer != nullptr) {
        hashValue(hash, request.backgroundRenderer->originalBackgroundRevision());
    }
    if (request.backgroundImage != nullptr) {
        hashValue(hash, static_cast<std::uint64_t>(request.backgroundImage->cacheKey()));
    }
    return hash;
}

int tileCoordinate(int value) {
    return value >= 0 ? value / snow_canvas_pen_mask::kTileSize
                      : -((-value + snow_canvas_pen_mask::kTileSize - 1) /
                          snow_canvas_pen_mask::kTileSize);
}

void restoreRow(const QRgb* original, QRgb* destination, const uchar* coverage, int count,
                snow_canvas_renderer::FilterRenderDiagnostics& diagnostics) {
    int x = 0;
    while (x < count) {
        const int alpha = coverage != nullptr ? coverage[x] : 255;
        if (alpha == 0) {
            ++x;
            continue;
        }
        if (alpha == 255) {
            const int begin = x++;
            while (x < count && (coverage == nullptr || coverage[x] == 255)) {
                ++x;
            }
            const auto bytes = static_cast<std::size_t>(x - begin) * sizeof(QRgb);
            std::memcpy(destination + begin, original + begin, bytes);
            diagnostics.copiedBytes += bytes;
            diagnostics.restoredPixelCount += static_cast<std::size_t>(x - begin);
            continue;
        }
        const QRgb from = destination[x];
        const QRgb to = original[x];
        const auto blend = [alpha](int a, int b) {
            return (a * (255 - alpha) + b * alpha + 127) / 255;
        };
        destination[x] = qRgba(blend(qRed(from), qRed(to)), blend(qGreen(from), qGreen(to)),
                               blend(qBlue(from), qBlue(to)), blend(qAlpha(from), qAlpha(to)));
        ++diagnostics.restoredPixelCount;
        ++diagnostics.restorationBlendPixelCount;
        ++x;
    }
}

QRect physicalCoverage(const QRectF& logical, const QRect& physicalBounds, qreal dpr) {
    const int left = static_cast<int>(std::floor(logical.left() * dpr));
    const int top = static_cast<int>(std::floor(logical.top() * dpr));
    const int right = static_cast<int>(std::ceil(logical.right() * dpr));
    const int bottom = static_cast<int>(std::ceil(logical.bottom() * dpr));
    return QRect(left, top, right - left, bottom - top).intersected(physicalBounds);
}

} // namespace

bool hasCoverage(const SnowCanvasSceneItem& item,
                 const snow_canvas_renderer::FilterFrameInfo& geometry,
                 const SceneDisplayInfo& info, const QRect& physicalBounds, qreal dpr,
                 snow_canvas_pen_mask::PenMaskAtlas& atlas,
                 const snow_canvas_filter_render::ExecutionOptions& execution) {
    const QRect pixels = physicalCoverage(geometry.logicalBounds, physicalBounds, dpr);
    if (item.is_free_draw == 0 || pixels.isEmpty()) {
        return !pixels.isEmpty();
    }
    for (int tileY = tileCoordinate(pixels.top()); tileY <= tileCoordinate(pixels.bottom());
         ++tileY) {
        for (int tileX = tileCoordinate(pixels.left()); tileX <= tileCoordinate(pixels.right());
             ++tileX) {
            const auto tile = atlas.tile(item, tileX, tileY, info, dpr, execution);
            if (!tile || !tile->occupied) {
                continue;
            }
            for (const auto& span : tile->spans) {
                const int y = tile->physicalOrigin.y() + span.y;
                if (y >= pixels.top() && y <= pixels.bottom() &&
                    tile->physicalOrigin.x() + span.endX > pixels.left() &&
                    tile->physicalOrigin.x() + span.beginX <= pixels.right()) {
                    return true;
                }
            }
        }
    }
    return false;
}

const QImage*
originalBackground(const snow_canvas_renderer::SceneRenderRequest& request,
                   const QRect& physicalBounds, qreal dpr,
                   snow_canvas_filter_render::RenderWorkspace& workspace,
                   std::shared_ptr<const snow_canvas_filter_tile_cache::Entry>& retained,
                   snow_canvas_renderer::FilterRenderDiagnostics& diagnostics) {
    const auto& info = *request.displayInfo;
    if (request.backgroundRenderer == nullptr && request.backgroundImage != nullptr &&
        request.backgroundImage->format() == QImage::Format_ARGB32_Premultiplied &&
        (!request.clearBackgroundEnabled || info.clear_color.a == 0) &&
        physicalBounds.topLeft() == QPoint() &&
        request.backgroundImage->size() == physicalBounds.size() &&
        physicalBounds.size() ==
            QSize(qCeil(info.surface_width * dpr), qCeil(info.surface_height * dpr))) {
        return request.backgroundImage;
    }
    snow_canvas_filter_tile_cache::Key key;
    key.canvasNamespace = request.cacheNamespace;
    key.tile = request.filterTileCoordinate;
    key.sourceRect = physicalBounds;
    key.logicalSize = QSize(qCeil(info.surface_width), qCeil(info.surface_height));
    const double dprDouble = dpr;
    std::memcpy(&key.devicePixelRatioBits, &dprDouble, sizeof(dprDouble));
    key.contentKey = contentKey(request, dpr);
    key.sourceKind = snow_canvas_filter_tile_cache::SourceKind::Pristine;
    const bool cacheEnabled =
        (request.enableFilterTileCache || request.enableOriginalBackgroundCache) &&
        key.canvasNamespace != nullptr;
    if (cacheEnabled) {
        retained = snow_canvas_filter_tile_cache::find(key);
        if (retained) {
            ++diagnostics.pristineTileHits;
            return &retained->image;
        }
        ++diagnostics.pristineTileMisses;
    }
    QImage& image = workspace.originalBackgroundScratch(physicalBounds.size(), dpr);
    if (image.isNull()) {
        return nullptr;
    }
    image.fill(request.clearBackgroundEnabled ? snow_canvas_renderer::toQColor(info.clear_color)
                                              : QColor(Qt::transparent));
    QPainter painter(&image);
    painter.setFont(request.painter->font());
    painter.setRenderHints(request.painter->renderHints());
    painter.translate(-physicalBounds.left() / dpr, -physicalBounds.top() / dpr);
    if (request.backgroundImage != nullptr && !request.backgroundImage->isNull()) {
        painter.drawImage(QRectF(0, 0, info.surface_width, info.surface_height),
                          *request.backgroundImage);
    }
    if (request.backgroundRenderer != nullptr && request.backgroundContext != nullptr) {
        auto context = *request.backgroundContext;
        context.exposedRegion =
            QRegion(QRectF(physicalBounds.x() / dpr, physicalBounds.y() / dpr,
                           physicalBounds.width() / dpr, physicalBounds.height() / dpr)
                        .toAlignedRect());
        context.devicePixelRatio = dpr;
        painter.save();
        request.backgroundRenderer->renderOriginalBackground(painter, context);
        painter.restore();
    }
    painter.end();
    if (cacheEnabled) {
        snow_canvas_filter_tile_cache::store(key, image, physicalBounds);
    }
    return &image;
}

bool apply(const QImage& original, QImage& destination, const SnowCanvasSceneItem& item,
           const snow_canvas_renderer::FilterFrameInfo& geometry, const SceneDisplayInfo& info,
           const QRect& physicalBounds, qreal dpr, snow_canvas_pen_mask::PenMaskAtlas& atlas,
           snow_canvas_filter_render::RenderWorkspace& workspace,
           const snow_canvas_filter_render::ExecutionOptions& execution,
           snow_canvas_renderer::FilterRenderDiagnostics& diagnostics) {
    const QRect pixels = physicalCoverage(geometry.logicalBounds, physicalBounds, dpr);
    if (pixels.isEmpty() || original.size() != destination.size()) {
        return false;
    }
    if (item.is_free_draw != 0) {
        for (int tileY = tileCoordinate(pixels.top()); tileY <= tileCoordinate(pixels.bottom());
             ++tileY) {
            for (int tileX = tileCoordinate(pixels.left()); tileX <= tileCoordinate(pixels.right());
                 ++tileX) {
                const auto tile = atlas.tile(item, tileX, tileY, info, dpr, execution);
                if (!tile || !tile->occupied) {
                    continue;
                }
                for (const auto& span : tile->spans) {
                    const int y = tile->physicalOrigin.y() + span.y;
                    const int begin =
                        std::max(pixels.left(), tile->physicalOrigin.x() + span.beginX);
                    const int end =
                        std::min(pixels.right() + 1, tile->physicalOrigin.x() + span.endX);
                    if (y < pixels.top() || y > pixels.bottom() || begin >= end) {
                        continue;
                    }
                    const int localY = y - physicalBounds.top();
                    const int localX = begin - physicalBounds.left();
                    restoreRow(reinterpret_cast<const QRgb*>(original.constScanLine(localY)) +
                                   localX,
                               reinterpret_cast<QRgb*>(destination.scanLine(localY)) + localX,
                               tile->alpha.constScanLine(span.y) + begin - tile->physicalOrigin.x(),
                               end - begin, diagnostics);
                }
            }
        }
    } else {
        QImage* mask = nullptr;
        if (!geometry.devicePixelAlignedRect) {
            mask = &workspace.alphaScratch(pixels.size(), dpr);
            if (mask->isNull()) {
                return false;
            }
            mask->fill(0);
            QPainter painter(mask);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.translate(-pixels.left() / dpr, -pixels.top() / dpr);
            painter.fillPath(geometry.clipPath, Qt::white);
            painter.end();
            diagnostics.maskPixelCount +=
                static_cast<std::size_t>(pixels.width()) * pixels.height();
        }
        for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
            const int localY = y - physicalBounds.top();
            const int localX = pixels.left() - physicalBounds.left();
            restoreRow(reinterpret_cast<const QRgb*>(original.constScanLine(localY)) + localX,
                       reinterpret_cast<QRgb*>(destination.scanLine(localY)) + localX,
                       mask != nullptr ? mask->constScanLine(y - pixels.top()) : nullptr,
                       pixels.width(), diagnostics);
        }
    }
    ++diagnostics.restorationDispatchCount;
    return true;
}

} // namespace snow_canvas_background_restore

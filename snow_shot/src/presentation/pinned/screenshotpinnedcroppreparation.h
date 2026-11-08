// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/image/screenshotanimationsource.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "../services/screenshotpinnedrenderbytes_p.h"

#include <QtMath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace screenshot_pinned_crop_preparation {
struct Plan {
    QImage image;
    std::shared_ptr<const ScreenshotAnimationSource> animation;
    QTransform orientation;
    ScreenshotResultStyle style;
    QRect pixels;
    QRect composedCrop;
    QSize orientedSize;
    QSize outputSize;
    bool bake = false;
    qint64 workingBytes = 0;
};

struct Prepared {
    QImage original;
    QImage transformed;
    QTransform transform;
    std::shared_ptr<const ScreenshotAnimationSource> animation;
    bool bakedStyle = false;
};

inline std::optional<QSize> transformedSize(QSize size, const QTransform& transform) {
    for (qreal component :
         {transform.m11(), transform.m12(), transform.m13(), transform.m21(), transform.m22(),
          transform.m23(), transform.m31(), transform.m32(), transform.m33()})
        if (!std::isfinite(component))
            return std::nullopt;
    const QRectF bounds = transform.mapRect(QRectF(QPointF(), size));
    const qreal left = std::floor(bounds.left());
    const qreal top = std::floor(bounds.top());
    const qreal right = std::ceil(bounds.right());
    const qreal bottom = std::ceil(bounds.bottom());
    constexpr qreal maximum = std::numeric_limits<int>::max();
    // Validate before trueMatrix performs integer conversion and negates its origin.
    if (size.isEmpty() || !std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
        !std::isfinite(bottom) || left < -maximum || top < -maximum || right > maximum ||
        bottom > maximum || right - left <= 0 || bottom - top <= 0 || right - left > maximum ||
        bottom - top > maximum)
        return std::nullopt;
    return QSize(int(right - left), int(bottom - top));
}

// The plan is shared by admission and execution. Account conservatively for concurrently
// retained output, normalization, masks, shadow scratch and animation preview copies.
inline std::optional<Plan> makePlan(QImage image,
                                    std::shared_ptr<const ScreenshotAnimationSource> animation,
                                    QTransform orientation, ScreenshotResultStyle style,
                                    QRectF background, QRect pixels, bool bake) {
    if (image.isNull() || pixels.isEmpty() || !image.rect().contains(pixels) ||
        !std::isfinite(background.width()) || background.width() <= 0)
        return std::nullopt;
    const auto oriented = transformedSize(image.size(), orientation);
    const auto cropped = transformedSize(pixels.size(), orientation);
    if (!oriented || !cropped)
        return std::nullopt;
    Plan plan;
    plan.image = std::move(image);
    plan.animation = std::move(animation);
    plan.orientation = orientation;
    plan.pixels = pixels;
    plan.orientedSize = *oriented;
    plan.outputSize = *cropped;
    plan.bake = bake;
    plan.workingBytes = 4096;
    const auto addRaster = [&](QSize size, int copies, int depth = 32) {
        const auto bytes = screenshot_pinned_render::rasterBytes(size, depth);
        if (!bytes)
            return false;
        for (int index = 0; index < copies; ++index)
            if (!screenshot_pinned_render::addBytes(plan.workingBytes, *bytes))
                return false;
        return true;
    };
    if (bake) {
        plan.style = ScreenshotResultCompositor::scaledStyle(style, qreal(oriented->width()) /
                                                                        background.width());
        const auto layout = ScreenshotResultCompositor::layoutForContent(*oriented, plan.style);
        const QRectF mapped =
            QImage::trueMatrix(orientation, plan.image.width(), plan.image.height())
                .mapRect(QRectF(pixels));
        if (!std::isfinite(mapped.x()) || !std::isfinite(mapped.y()) ||
            !std::isfinite(mapped.right()) || !std::isfinite(mapped.bottom()) ||
            mapped.left() < std::numeric_limits<int>::min() ||
            mapped.top() < std::numeric_limits<int>::min() ||
            mapped.right() > std::numeric_limits<int>::max() ||
            mapped.bottom() > std::numeric_limits<int>::max() || layout.outputRect.isEmpty())
            return std::nullopt;
        plan.composedCrop = mapped.toAlignedRect().translated(-layout.outputRect.topLeft());
        if (!QRect(QPoint(), layout.outputRect.size()).contains(plan.composedCrop))
            return std::nullopt;
        plan.outputSize = plan.composedCrop.size();
        if (!addRaster(*oriented, 1, std::max(32, plan.image.depth())) ||
            !addRaster(layout.outputRect.size(), 8))
            return std::nullopt;
    }
    if (!addRaster(bake ? plan.outputSize : pixels.size(), 1, std::max(32, plan.image.depth())) ||
        !addRaster(plan.outputSize, 1, std::max(32, plan.image.depth())) ||
        !addRaster(plan.outputSize, 1))
        return std::nullopt;
    if (plan.animation) {
        if (plan.animation->firstFrame.isNull() || plan.animation->firstDisplayFrame.isNull() ||
            !plan.animation->firstFrame.rect().contains(pixels) ||
            !plan.animation->firstDisplayFrame.rect().contains(pixels) ||
            !addRaster(pixels.size(), 1, std::max(32, plan.animation->firstFrame.depth())) ||
            !addRaster(pixels.size(), 1, std::max(32, plan.animation->firstDisplayFrame.depth())))
            return std::nullopt;
    }
    return plan;
}

using StageHook = std::function<void(int, const ScreenshotExportCancellation&)>;

inline Prepared prepare(const Plan& plan, const ScreenshotExportCancellation& token,
                        const StageHook& stage = {}) {
    const auto cancelled = [&](int index) {
        if (stage)
            stage(index, token);
        return token.isCancellationRequested();
    };
    Prepared result;
    result.bakedStyle = plan.bake;
    if (cancelled(0))
        return {};
    if (plan.bake) {
        const auto oriented =
            snowCanvasTransformImage(plan.image, plan.orientation, Qt::SmoothTransformation);
        if (oriented.isNull() || oriented.size() != plan.orientedSize || cancelled(1))
            return {};
        const auto composed = ScreenshotResultCompositor::compose(oriented, plan.style);
        if (composed.isNull() || cancelled(2))
            return {};
        result.original = snowCanvasCopyImage(composed, plan.composedCrop);
    } else {
        if (cancelled(2))
            return {};
        result.original = snowCanvasCopyImage(plan.image, plan.pixels);
        result.transform =
            QImage::trueMatrix(plan.orientation, plan.pixels.width(), plan.pixels.height());
    }
    if (result.original.isNull() || cancelled(3))
        return {};
    const auto transformed =
        snowCanvasTransformImage(result.original, result.transform, Qt::SmoothTransformation);
    if (transformed.isNull() || transformed.size() != plan.outputSize || cancelled(4))
        return {};
    result.transformed = ScreenshotResultCompositor::normalizeImage(transformed);
    if (result.transformed.isNull() || cancelled(5))
        return {};
    if (plan.animation) {
        auto next = std::make_shared<ScreenshotAnimationSource>(*plan.animation);
        next->sourceCropRect = plan.pixels.translated(next->sourceCropRect.topLeft());
        next->canvasSize = plan.pixels.size();
        next->firstFrame = snowCanvasCopyImage(next->firstFrame, plan.pixels);
        if (next->firstFrame.isNull() || cancelled(6))
            return {};
        next->firstDisplayFrame =
            plan.animation->firstFrame.constBits() == plan.animation->firstDisplayFrame.constBits()
                ? next->firstFrame
                : snowCanvasCopyImage(next->firstDisplayFrame, plan.pixels);
        if (next->firstDisplayFrame.isNull())
            return {};
        result.animation = std::move(next);
    }
    return cancelled(7) ? Prepared{} : result;
}
} // namespace screenshot_pinned_crop_preparation

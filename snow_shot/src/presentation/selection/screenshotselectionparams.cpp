#include "snow_shot/presentation/screenshotselectionparams.h"

#include "snow_shot/presentation/screenshotselectionlimits.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

#include <algorithm>

namespace {
using snow_shot::presentation::kScreenshotSelectionCornerRadiusMax;
using snow_shot::presentation::kScreenshotSelectionShadowWidthMax;

QRect validBoundsOrFallback(const QRect& bounds) {
    return bounds.isValid() && !bounds.isEmpty() ? bounds : QRect(0, 0, 1, 1);
}
} // namespace

ScreenshotResultStyle ScreenshotSelectionParams::resultStyle() const {
    ScreenshotResultStyle style{radius, shadowWidth, shadowColor};
    style.borderType = borderType;
    style.borderWidth = borderWidth;
    style.borderColor = borderColor;
    style.backgroundOpacityPercent = backgroundOpacityPercent;
    if (region)
        style.region = region->translated(-selection.topLeft());
    return style;
}

ScreenshotSelectionParams pinnedStyleToContentSelection(const ScreenshotResultStyle& style,
                                                        const QRectF& contentBounds) {
    ScreenshotSelectionParams selection;
    selection.selection = contentBounds.toAlignedRect();
    selection.radius = style.cornerRadius;
    selection.shadowWidth = style.shadowWidth;
    selection.shadowColor = style.shadowColor;
    selection.borderType = style.borderType;
    selection.borderWidth = style.borderWidth;
    selection.borderColor = style.borderColor;
    selection.backgroundOpacityPercent = style.backgroundOpacityPercent;
    const bool integralBounds = QRectF(selection.selection) == contentBounds;
    if (integralBounds && style.regionScale == 1.0) {
        if (style.region)
            selection.region = style.region->translated(selection.selection.topLeft());
    } else if (style.region || !integralBounds) {
        // QRegion transforms round fractional edges. Capture the compositor's
        // exact vector outline instead, and avoid applying its corners twice.
        selection.region = ScreenshotRegionGeometry::fromPath(
            screenshotSelectionPath(contentBounds, style.region, style.cornerRadius,
                                    style.regionScale),
            ScreenshotRegionType::Curve);
        selection.radius = 0;
    }
    return selection;
}

ScreenshotSelectionParams clampScreenshotSelectionParams(const ScreenshotSelectionParams& params,
                                                         const QRect& bounds) {
    const QRect actualBounds = validBoundsOrFallback(bounds);
    const QRect normalized = params.selection.normalized();
    const int width = std::clamp(normalized.width(), 1, actualBounds.width());
    const int height = std::clamp(normalized.height(), 1, actualBounds.height());
    const int maxLeft = actualBounds.left() + actualBounds.width() - width;
    const int maxTop = actualBounds.top() + actualBounds.height() - height;

    ScreenshotSelectionParams result = params;
    result.selection =
        QRect(std::clamp(normalized.left(), actualBounds.left(), maxLeft),
              std::clamp(normalized.top(), actualBounds.top(), maxTop), width, height);
    result.layoutCompaction = result.layoutCompaction.normalized();
    result.backgroundOpacityPercent = std::clamp(result.backgroundOpacityPercent, 0, 100);
    result.radius = std::clamp(result.radius, 0, kScreenshotSelectionCornerRadiusMax);
    result.shadowWidth = std::clamp(result.shadowWidth, 0, kScreenshotSelectionShadowWidthMax);
    result.borderWidth = std::clamp(result.borderWidth, 0, kScreenshotSelectionShadowWidthMax);
    if (!result.borderColor.isValid())
        result.borderColor = QColor(Qt::white);
    result.borderColor.setAlpha(255);
    if (!result.shadowColor.isValid()) {
        result.shadowColor = QColor(0x33, 0x33, 0x33);
    }
    if (result.region) {
        result.region = result.region->intersected(QRegion(actualBounds));
        result.selection = result.region->boundingRect();
    }
    return result;
}

std::optional<ScreenshotSelectionParams>
clampScreenshotSelectionParamsForRestore(const ScreenshotSelectionParams& params,
                                         const QRect& bounds) {
    if (!bounds.isValid() || bounds.isEmpty() ||
        (!params.region && params.selection.normalized().isEmpty())) {
        return std::nullopt;
    }
    auto clamped = clampScreenshotSelectionParams(params, bounds);
    if (clamped.selection.isEmpty()) {
        return std::nullopt;
    }
    return clamped;
}

QVector<ScreenshotSelectionPreset>
sanitizeScreenshotSelectionPresets(const QVector<ScreenshotSelectionPreset>& presets,
                                   const QRect& bounds) {
    QVector<ScreenshotSelectionPreset> sanitized;
    sanitized.reserve(presets.size());
    for (ScreenshotSelectionPreset preset : presets) {
        preset.name = preset.name.trimmed();
        if (preset.name.isEmpty()) {
            continue;
        }
        preset.params.region.reset();
        preset.params = clampScreenshotSelectionParams(preset.params, bounds);
        sanitized.push_back(preset);
    }
    return sanitized;
}

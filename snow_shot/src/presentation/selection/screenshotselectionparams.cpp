#include "snow_shot/presentation/screenshotselectionparams.h"

#include "snow_shot/presentation/screenshotselectionlimits.h"

#include <algorithm>

namespace {
using snow_shot::presentation::kScreenshotSelectionCornerRadiusMax;
using snow_shot::presentation::kScreenshotSelectionShadowWidthMax;

QRect validBoundsOrFallback(const QRect& bounds) {
    return bounds.isValid() && !bounds.isEmpty() ? bounds : QRect(0, 0, 1, 1);
}
} // namespace

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
    result.radius = std::clamp(result.radius, 0, kScreenshotSelectionCornerRadiusMax);
    result.shadowWidth = std::clamp(result.shadowWidth, 0, kScreenshotSelectionShadowWidthMax);
    if (!result.shadowColor.isValid()) {
        result.shadowColor = QColor(0x33, 0x33, 0x33);
    }
    if (result.region) {
        result.region = result.region->intersected(QRegion(actualBounds));
        result.selection = result.region->boundingRect();
    }
    return result;
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

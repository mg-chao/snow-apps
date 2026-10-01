#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONASPECTRATIO_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONASPECTRATIO_H

#include <QString>
#include <QRectF>
#include <QtGlobal>

#include <cmath>
#include <limits>

enum class ScreenshotSelectionAspectRatioPreset {
    Free = 0,
    Square,
    Landscape3x2,
    Landscape4x3,
    Landscape16x9,
    Portrait2x3,
    Portrait3x4,
    Portrait9x16,
};

[[nodiscard]] inline QString
screenshotSelectionAspectRatioPresetId(ScreenshotSelectionAspectRatioPreset preset) {
    switch (preset) {
    case ScreenshotSelectionAspectRatioPreset::Square:
        return QStringLiteral("1:1");
    case ScreenshotSelectionAspectRatioPreset::Landscape3x2:
        return QStringLiteral("3:2");
    case ScreenshotSelectionAspectRatioPreset::Landscape4x3:
        return QStringLiteral("4:3");
    case ScreenshotSelectionAspectRatioPreset::Landscape16x9:
        return QStringLiteral("16:9");
    case ScreenshotSelectionAspectRatioPreset::Portrait2x3:
        return QStringLiteral("2:3");
    case ScreenshotSelectionAspectRatioPreset::Portrait3x4:
        return QStringLiteral("3:4");
    case ScreenshotSelectionAspectRatioPreset::Portrait9x16:
        return QStringLiteral("9:16");
    case ScreenshotSelectionAspectRatioPreset::Free:
    default:
        return QStringLiteral("free");
    }
}

[[nodiscard]] inline ScreenshotSelectionAspectRatioPreset
screenshotSelectionAspectRatioPresetFromId(const QString& id) {
    using Preset = ScreenshotSelectionAspectRatioPreset;
    if (id == QStringLiteral("1:1"))
        return Preset::Square;
    if (id == QStringLiteral("3:2"))
        return Preset::Landscape3x2;
    if (id == QStringLiteral("4:3"))
        return Preset::Landscape4x3;
    if (id == QStringLiteral("16:9"))
        return Preset::Landscape16x9;
    if (id == QStringLiteral("2:3"))
        return Preset::Portrait2x3;
    if (id == QStringLiteral("3:4"))
        return Preset::Portrait3x4;
    if (id == QStringLiteral("9:16"))
        return Preset::Portrait9x16;
    return Preset::Free;
}

// Drag geometry stores ratios as height / width, including portrait presets.
[[nodiscard]] inline qreal
screenshotSelectionAspectRatioHeightOverWidth(ScreenshotSelectionAspectRatioPreset preset) {
    switch (preset) {
    case ScreenshotSelectionAspectRatioPreset::Square:
        return 1.0;
    case ScreenshotSelectionAspectRatioPreset::Landscape3x2:
        return 2.0 / 3.0;
    case ScreenshotSelectionAspectRatioPreset::Landscape4x3:
        return 3.0 / 4.0;
    case ScreenshotSelectionAspectRatioPreset::Landscape16x9:
        return 9.0 / 16.0;
    case ScreenshotSelectionAspectRatioPreset::Portrait2x3:
        return 3.0 / 2.0;
    case ScreenshotSelectionAspectRatioPreset::Portrait3x4:
        return 4.0 / 3.0;
    case ScreenshotSelectionAspectRatioPreset::Portrait9x16:
        return 16.0 / 9.0;
    case ScreenshotSelectionAspectRatioPreset::Free:
    default:
        return 0.0;
    }
}

// Compare conventional width / height ratios. Strictly improving the distance
// makes ties deterministic in the same order as the toolbar's preset list.
[[nodiscard]] inline ScreenshotSelectionAspectRatioPreset
screenshotSelectionClosestAspectRatioPreset(const QRectF& selection) {
    if (!selection.isValid() || selection.width() <= 0.0 || selection.height() <= 0.0 ||
        !std::isfinite(selection.width()) || !std::isfinite(selection.height())) {
        return ScreenshotSelectionAspectRatioPreset::Free;
    }
    const qreal ratio = selection.width() / selection.height();
    if (!std::isfinite(ratio)) {
        return ScreenshotSelectionAspectRatioPreset::Free;
    }
    constexpr ScreenshotSelectionAspectRatioPreset presets[] = {
        ScreenshotSelectionAspectRatioPreset::Square,
        ScreenshotSelectionAspectRatioPreset::Landscape3x2,
        ScreenshotSelectionAspectRatioPreset::Landscape4x3,
        ScreenshotSelectionAspectRatioPreset::Landscape16x9,
        ScreenshotSelectionAspectRatioPreset::Portrait2x3,
        ScreenshotSelectionAspectRatioPreset::Portrait3x4,
        ScreenshotSelectionAspectRatioPreset::Portrait9x16,
    };
    auto closest = ScreenshotSelectionAspectRatioPreset::Free;
    qreal distance = std::numeric_limits<qreal>::infinity();
    for (const auto preset : presets) {
        const qreal candidate = 1.0 / screenshotSelectionAspectRatioHeightOverWidth(preset);
        const qreal candidateDistance = std::abs(ratio - candidate);
        if (candidateDistance < distance) {
            closest = preset;
            distance = candidateDistance;
        }
    }
    return closest;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONASPECTRATIO_H

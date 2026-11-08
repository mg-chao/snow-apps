#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPARAMS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPARAMS_H

#include "snow_shot/image/screenshotlayoutcompaction.h"

#include "snow_shot/image/screenshotselectionbordertype.h"

#include <QColor>
#include <QRect>
#include "snow_shot/image/screenshotregiongeometry.h"
#include <optional>
#include <QString>
#include <QVector>

struct ScreenshotResultStyle;

struct ScreenshotSelectionParams {
    QRect selection;
    int radius = 0;
    int shadowWidth = 0;
    QColor shadowColor = QColor(0x33, 0x33, 0x33);
    bool lockAspectRatio = false;
    bool lockDragAspectRatio = false;
    // Absent for legacy rectangles and named presets; present geometry is authoritative.
    std::optional<ScreenshotRegionGeometry> region;
    ScreenshotLayoutCompaction layoutCompaction = {};
    int backgroundOpacityPercent = 0;
    ScreenshotSelectionBorderType borderType = ScreenshotSelectionBorderType::Shadow;
    int borderWidth = 0;
    QColor borderColor = QColor(Qt::white);

    // Selection geometry is absolute; result geometry is relative to the crop origin.
    [[nodiscard]] ScreenshotResultStyle resultStyle() const;

    [[nodiscard]] int effectWidth() const {
        return borderType == ScreenshotSelectionBorderType::Border ? borderWidth : shadowWidth;
    }
    [[nodiscard]] QColor effectColor() const {
        return borderType == ScreenshotSelectionBorderType::Border ? borderColor : shadowColor;
    }

    bool operator==(const ScreenshotSelectionParams& other) const {
        return selection == other.selection && radius == other.radius &&
               shadowWidth == other.shadowWidth && shadowColor == other.shadowColor &&
               lockAspectRatio == other.lockAspectRatio &&
               lockDragAspectRatio == other.lockDragAspectRatio && region == other.region &&
               layoutCompaction == other.layoutCompaction &&
               backgroundOpacityPercent == other.backgroundOpacityPercent &&
               borderType == other.borderType && borderWidth == other.borderWidth &&
               borderColor == other.borderColor;
    }

    bool operator!=(const ScreenshotSelectionParams& other) const {
        return !(*this == other);
    }
};

struct ScreenshotSelectionPreset {
    QString name;
    ScreenshotSelectionParams params;

    bool operator==(const ScreenshotSelectionPreset& other) const {
        return name == other.name && params == other.params;
    }

    bool operator!=(const ScreenshotSelectionPreset& other) const {
        return !(*this == other);
    }
};

[[nodiscard]] ScreenshotSelectionParams
clampScreenshotSelectionParams(const ScreenshotSelectionParams& params, const QRect& bounds);

// Project a retained pin's content-relative style into absolute document coordinates.
// Fractional mappings capture the rendered outline, including rounded corners.
[[nodiscard]] ScreenshotSelectionParams
pinnedStyleToContentSelection(const ScreenshotResultStyle& style, const QRectF& contentBounds);

// Restore commands must validate available geometry before replacing the current selection.
[[nodiscard]] std::optional<ScreenshotSelectionParams>
clampScreenshotSelectionParamsForRestore(const ScreenshotSelectionParams& params,
                                         const QRect& bounds);

[[nodiscard]] QVector<ScreenshotSelectionPreset>
sanitizeScreenshotSelectionPresets(const QVector<ScreenshotSelectionPreset>& presets,
                                   const QRect& bounds);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPARAMS_H

#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTUIPREFERENCES_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTUIPREFERENCES_H

#include <QColor>
#include <QString>
#include <QtGlobal>

#include <algorithm>

enum class ScreenshotColorPickerDisplayMode {
    HideOutsideSelection,
    AlwaysShow,
    AlwaysHide,
};

struct ScreenshotUiPreferences {
    bool selectionTransitionAnimationEnabled = true;
    ScreenshotColorPickerDisplayMode colorPickerDisplayMode =
        ScreenshotColorPickerDisplayMode::HideOutsideSelection;
    QColor selectionMaskColor = QColor(0, 0, 0, 128);
    qreal shortcutHintOpacity = 1.0;
    QColor cursorGuideLineColor = QColor(0, 0, 0, 0);
    QColor monitorCenterGuideLineColor = QColor(0, 0, 0, 0);
    QColor colorPickerCenterGuideLineColor = QColor(0, 0, 0, 0);

    [[nodiscard]] ScreenshotUiPreferences normalized() const {
        ScreenshotUiPreferences result = *this;
        if (!result.selectionMaskColor.isValid()) {
            result.selectionMaskColor = QColor(0, 0, 0, 128);
        }
        result.shortcutHintOpacity = std::clamp<qreal>(result.shortcutHintOpacity, 0.0, 1.0);
        if (!result.cursorGuideLineColor.isValid()) {
            result.cursorGuideLineColor = QColor(0, 0, 0, 0);
        }
        if (!result.monitorCenterGuideLineColor.isValid()) {
            result.monitorCenterGuideLineColor = QColor(0, 0, 0, 0);
        }
        if (!result.colorPickerCenterGuideLineColor.isValid()) {
            result.colorPickerCenterGuideLineColor = QColor(0, 0, 0, 0);
        }
        return result;
    }
};

[[nodiscard]] inline ScreenshotColorPickerDisplayMode
screenshotColorPickerDisplayModeFromString(const QString& value) {
    if (value == QStringLiteral("always_show")) {
        return ScreenshotColorPickerDisplayMode::AlwaysShow;
    }
    if (value == QStringLiteral("always_hide")) {
        return ScreenshotColorPickerDisplayMode::AlwaysHide;
    }
    return ScreenshotColorPickerDisplayMode::HideOutsideSelection;
}

struct ScreenshotColorPickerVisibilityState {
    bool intelligentSelecting = false;
    bool manualSelecting = false;
    bool movingSelection = false;
    bool dragging = false;
    bool selectionDrag = false;
    bool hasSelection = false;
    bool pointInsideSelection = false;
};

[[nodiscard]] inline qreal screenshotColorPickerOpacity(
    ScreenshotColorPickerDisplayMode mode, const ScreenshotColorPickerVisibilityState& state) {
    if (mode == ScreenshotColorPickerDisplayMode::AlwaysHide) {
        return 0.0;
    }
    if (state.selectionDrag) {
        return 0.83;
    }
    if (state.manualSelecting && state.dragging) {
        return 0.5;
    }
    if (!state.hasSelection) {
        return state.intelligentSelecting || state.manualSelecting ? 1.0 : 0.0;
    }
    if (mode == ScreenshotColorPickerDisplayMode::HideOutsideSelection &&
        !state.pointInsideSelection) {
        return 0.0;
    }
    if (state.manualSelecting || state.movingSelection) {
        return state.dragging ? 0.5 : 1.0;
    }
    return 1.0;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTUIPREFERENCES_H

#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSETTINGSSTORE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSETTINGSSTORE_H

#include "snow_shot/presentation/screenshotselectionparams.h"
#include "snow_shot/presentation/screenshotselectionaspectratio.h"

#include <QVector>

class ScreenshotSelectionSettingsStore final {
  public:
    explicit ScreenshotSelectionSettingsStore();

    ScreenshotRegionType regionType() const;
    void setRegionType(ScreenshotRegionType type);
    [[nodiscard]] int cornerRadius() const;
    [[nodiscard]] int shadowWidth() const;
    [[nodiscard]] QColor shadowColor() const;
    [[nodiscard]] ScreenshotSelectionBorderType borderType() const;
    [[nodiscard]] int borderWidth() const;
    [[nodiscard]] QColor borderColor() const;
    void setSelectionBorder(ScreenshotSelectionBorderType type, int width, const QColor& color);
    void setSelectionEffects(int cornerRadius, int shadowWidth, const QColor& shadowColor);
    void setSelectionEffects(const ScreenshotSelectionParams& params);
    [[nodiscard]] ScreenshotSelectionAspectRatioPreset aspectRatioPreset() const;
    [[nodiscard]] bool aspectRatioLocked() const;
    void setAspectRatioPreference(ScreenshotSelectionAspectRatioPreset preset, bool locked);
    void setAspectRatioLocked(bool locked);

    [[nodiscard]] QVector<ScreenshotSelectionPreset> presets() const;
    void setPresets(const QVector<ScreenshotSelectionPreset>& presets);

    [[nodiscard]] ScreenshotLayoutCompaction layoutCompaction() const;
    void setLayoutCompaction(ScreenshotLayoutCompaction options);

    [[nodiscard]] int backgroundOpacityPercent() const;
    void setBackgroundOpacityPercent(int percent);

    void clear();
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSETTINGSSTORE_H

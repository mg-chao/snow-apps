#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSETTINGSSTORE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSETTINGSSTORE_H

#include "snow_shot/presentation/screenshotselectionexportworkflowports.h"
#include "snow_shot/presentation/screenshotselectionparams.h"
#include "snow_shot/presentation/screenshotintelligentselectionmodel.h"

#include <QVector>

class ScreenshotSelectionSettingsStore final : public ScreenshotSelectionParamsStorePort {
  public:
    explicit ScreenshotSelectionSettingsStore();

    [[nodiscard]] bool hasPreviousSelectionParams() const;
    [[nodiscard]] ScreenshotSelectionParams previousSelectionParams() const;
    void setPreviousSelectionParams(const ScreenshotSelectionParams& params) override;

    ScreenshotRegionType regionType() const;
    void setRegionType(ScreenshotRegionType type);
    [[nodiscard]] int cornerRadius() const;
    [[nodiscard]] int shadowWidth() const;
    void setSelectionEffects(int cornerRadius, int shadowWidth);
    [[nodiscard]] bool aspectRatioLocked() const;
    void setAspectRatioLocked(bool locked);

    [[nodiscard]] ScreenshotIntelligentSelectionTarget selectionTarget() const;
    void setSelectionTarget(ScreenshotIntelligentSelectionTarget target);

    [[nodiscard]] QVector<ScreenshotSelectionPreset> presets() const;
    void setPresets(const QVector<ScreenshotSelectionPreset>& presets);

    void clear();
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSETTINGSSTORE_H

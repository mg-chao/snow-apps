#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTWORKFLOW_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTWORKFLOW_H

#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotselectionexportworkflowports.h"
#include "snow_shot/presentation/screenshotselectionparams.h"
#include "snow_shot/presentation/screenshottypes.h"

#include <QRect>

#include <functional>

class QObject;
class ScreenshotSelectionModel;
struct ScreenshotCaptureState;

struct ScreenshotSelectionExportWorkflowContext {
    ScreenshotCaptureState& captureState;
    const ScreenshotGeometryMapper& geometry;
    ScreenshotSelectionModel& selection;
    ScreenshotSelectionImageComposerPort& imageComposer;
    ScreenshotSelectionExportDestinationPort& destination;
    ScreenshotSelectionParamsStorePort& selectionSettings;
    QObject& callbackContext;
};

class ScreenshotSelectionExportWorkflow final {
  public:
    explicit ScreenshotSelectionExportWorkflow(ScreenshotSelectionExportWorkflowContext context);

    using Completion = std::function<void(bool)>;
    using CopyCompletion = std::function<void(bool, QImage)>;
    using ResultValidator = std::function<bool()>;

    [[nodiscard]] bool copySelectionToClipboard(ResultValidator validator,
                                                CopyCompletion completion);
    [[nodiscard]] bool pinSelectionToScreen(ResultValidator validator,
                                            Completion completion);

  private:
    [[nodiscard]] ScreenshotSelectionParams currentSelectionParams() const;
    [[nodiscard]] QRect selectionBounds() const;
    void persistCurrentSelectionParams();
    void persistSelectionParams(const ScreenshotSelectionParams& params);

    ScreenshotSelectionExportWorkflowContext m_context;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONEXPORTWORKFLOW_H

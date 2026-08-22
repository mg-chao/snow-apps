#include "snow_shot/presentation/screenshotselectionexportworkflow.h"

#include "snow_shot/presentation/screenshotcapturestate.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include <memory>

namespace {
ScreenshotResultStyle resultStyle(const ScreenshotSelectionModel& selection) {
    return ScreenshotResultStyle{selection.cornerRadius(), selection.shadowWidth(),
                                 selection.shadowColor()};
}
} // namespace

ScreenshotSelectionExportWorkflow::ScreenshotSelectionExportWorkflow(
    ScreenshotSelectionExportWorkflowContext context)
    : m_context(context) {}

bool ScreenshotSelectionExportWorkflow::copySelectionToClipboard(ResultValidator validator,
                                                                 CopyCompletion completion) {
    const QRect selection = m_context.selection.pixelSelection();
    if (selection.width() < 1 || selection.height() < 1) {
        return false;
    }

    return m_context.imageComposer.requestSelectionClipboard(
        selection, resultStyle(m_context.selection), &m_context.callbackContext,
        [this, validator = std::move(validator),
         completion = std::move(completion)](ScreenshotSelectionClipboardResult result) mutable {
            if (validator && !validator()) {
                if (completion) {
                    completion(false, {});
                }
                return;
            }
            if (!result.isValid()) {
                if (completion) {
                    completion(false, {});
                }
                return;
            }
            QImage resultImage = std::move(result.image);
            auto terminal = std::make_shared<bool>(false);
            auto publishCompletion = std::make_shared<std::function<void(bool)>>();
            *publishCompletion = [this, terminal, validator = std::move(validator),
                                  completion = std::move(completion),
                                  resultImage = std::move(resultImage)](bool success) mutable {
                if (*terminal) {
                    return;
                }
                *terminal = true;
                if (validator && !validator()) {
                    success = false;
                }
                if (success) {
                    persistCurrentSelectionParams();
                }
                if (completion) {
                    completion(success, success ? std::move(resultImage) : QImage{});
                }
            };
            const bool scheduled = m_context.destination.publishClipboard(
                &m_context.callbackContext, std::move(result.payload),
                [publishCompletion](bool success) { (*publishCompletion)(success); });
            if (!scheduled) {
                (*publishCompletion)(false);
            }
        });
}

bool ScreenshotSelectionExportWorkflow::pinSelectionToScreen(ResultValidator validator,
                                                             Completion completion) {
    SNOW_SHOT_PIN_PERF_SCOPE("workflow.pin_selection");
    const QRect selection = m_context.selection.pixelSelection();
    if (selection.width() < 1 || selection.height() < 1) {
        return false;
    }

    std::optional<ScreenshotPinnedSelectionRequest> request =
        m_context.imageComposer.preparePinnedSelection(selection, resultStyle(m_context.selection));
    if (!request.has_value()) {
        return false;
    }

    return m_context.imageComposer.schedulePinnedSelection(
        std::move(*request), &m_context.callbackContext,
        [this, validator = std::move(validator),
         completion = std::move(completion)](ScreenshotPinnedSelectionRequest pinRequest) mutable {
            if (validator && !validator()) {
                if (completion) {
                    completion(false);
                }
                return;
            }
            const bool success =
                pinRequest.isValid() && m_context.destination.presentPinnedSelection(pinRequest);
            SNOW_SHOT_PIN_PERF_MILESTONE("workflow.destination_complete");
            if (success) {
                persistCurrentSelectionParams();
            }
            if (completion) {
                completion(success);
            }
        });
}

ScreenshotSelectionParams ScreenshotSelectionExportWorkflow::currentSelectionParams() const {
    return m_context.selection.params(selectionBounds());
}

QRect ScreenshotSelectionExportWorkflow::selectionBounds() const {
    const QRectF bounds = m_context.geometry.canvasBounds();
    if (bounds.isNull() || bounds.isEmpty()) {
        return {};
    }
    return ScreenshotHalfOpenRect::fromRectF(bounds).toAlignedQRect();
}

void ScreenshotSelectionExportWorkflow::persistCurrentSelectionParams() {
    persistSelectionParams(currentSelectionParams());
}

void ScreenshotSelectionExportWorkflow::persistSelectionParams(
    const ScreenshotSelectionParams& params) {
    const QRect bounds = selectionBounds();
    if (bounds.isEmpty()) {
        m_context.selectionSettings.setPreviousSelectionParams(params);
        return;
    }
    m_context.selectionSettings.setPreviousSelectionParams(
        clampScreenshotSelectionParams(params, bounds));
}

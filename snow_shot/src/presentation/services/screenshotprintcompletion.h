#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPRINTCOMPLETION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPRINTCOMPLETION_H

#include "snow_shot/presentation/screenshotprintservice.h"

#include <utility>

// Keep native outcomes separate from the capture's clipboard/file export workflow.
// Finish always releases interaction; only a current capture may close or refocus.
inline ScreenshotPrintService::Completion screenshotPrintCompletion(
    std::shared_ptr<bool> completed, std::function<bool()> finishCurrentCapture,
    std::function<void()> submitted, std::function<void(ScreenshotPrintService::Result)> restore) {
    return [completed = std::move(completed), finish = std::move(finishCurrentCapture),
            submitted = std::move(submitted),
            restore = std::move(restore)](ScreenshotPrintService::Result result) {
        if (std::exchange(*completed, true) || !finish())
            return;
        if (result.status == ScreenshotPrintService::Status::Submitted)
            submitted();
        else
            restore(std::move(result));
    };
}

#endif

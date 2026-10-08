#ifndef SNOW_SHOT_WINDOWS_NATIVEPRINTLIFECYCLE_H
#define SNOW_SHOT_WINDOWS_NATIVEPRINTLIFECYCLE_H

#include "snow_shot/presentation/screenshotprintservice.h"

#include <mutex>
#include <optional>

// PrintTask creation starts preview, not submission. Share the submission boundary
// between the UI callbacks and MakeDocument so recovery cannot race a late print.
class ScreenshotWindowsPrintLifecycle final {
  public:
    bool beginDocument() {
        std::lock_guard lock(m_mutex);
        if (m_finished)
            return false;
        m_documentStarted = true;
        return true;
    }

    std::optional<ScreenshotPrintService::Result> finish(ScreenshotPrintService::Result result) {
        std::lock_guard lock(m_mutex);
        if (m_finished)
            return std::nullopt;
        m_finished = true;
        using Status = ScreenshotPrintService::Status;
        if (result.status == Status::Failed || result.status == Status::Unavailable)
            result.status = m_documentStarted ? Status::Failed : Status::Unavailable;
        return result;
    }

    bool finished() const {
        std::lock_guard lock(m_mutex);
        return m_finished;
    }

  private:
    mutable std::mutex m_mutex;
    bool m_documentStarted = false;
    bool m_finished = false;
};

#endif

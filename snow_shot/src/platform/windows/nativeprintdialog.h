#ifndef SNOW_SHOT_WINDOWS_NATIVEPRINTDIALOG_H
#define SNOW_SHOT_WINDOWS_NATIVEPRINTDIALOG_H

#include "snow_shot/presentation/screenshotprintservice.h"

#include <qt_windows.h>
#include <objbase.h>

// Replace only wizard activation for deterministic tests without UI or printer jobs.
struct ScreenshotWindowsPrintDialogApi {
    decltype(&CoCreateInstance) create = &CoCreateInstance;
};

[[nodiscard]] ScreenshotPrintService::Backend
screenshotLegacyWindowsPrintBackend(ScreenshotWindowsPrintDialogApi api = {});

#endif

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/presentation/screenshotcontroller.h"
#include <QImage>
#ifdef Q_OS_MACOS
#include "snow_shot/platform/macos/screenshotsessionfocus.h"
#endif

class ScreenshotOverlayWindow;
class SnowCanvasRuntime;
class ScreenshotToolbarCommandSink;
struct ScreenshotSelectionParams;

// Implemented only in test builds, where the controller's private implementation is complete.
class ScreenshotControllerTestAccess {
  public:
    static void prepareSelection(ScreenshotController& controller, QImage image,
                                 const ScreenshotSelectionParams& selection);
    static SnowCanvasRuntime& runtime(ScreenshotController& controller);
    static ScreenshotOverlayWindow* presentSelection(ScreenshotController& controller);
    static void confirmCaptureSelection(ScreenshotController& controller,
                                        ScreenshotController::CaptureAction action);
    [[nodiscard]] static bool exportActive(const ScreenshotController& controller);
    [[nodiscard]] static bool clipboardFormulaPending(const ScreenshotController& controller);
    [[nodiscard]] static quint64 clipboardPinGeneration(const ScreenshotController& controller);
#ifdef Q_OS_MACOS
    static void
    installSessionFocus(ScreenshotController& controller,
                        snow_shot::platform::macos::ScreenshotSessionFocus::Capture capture);
    static ScreenshotToolbarCommandSink& commands(ScreenshotController& controller);
#endif
};

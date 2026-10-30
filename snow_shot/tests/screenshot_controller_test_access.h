// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>

class ScreenshotController;
struct ScreenshotSelectionParams;

// Implemented only in test builds, where the controller's private implementation is complete.
class ScreenshotControllerTestAccess {
  public:
    static void prepareSelection(ScreenshotController& controller, QImage image,
                                 const ScreenshotSelectionParams& selection);
    [[nodiscard]] static bool exportActive(const ScreenshotController& controller);
};

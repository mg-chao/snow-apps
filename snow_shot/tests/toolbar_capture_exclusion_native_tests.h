#pragma once

#include <functional>

class ScreenshotFloatingToolPaletteWindow;
class QWidget;
int verifyToolbarCaptureExclusionAffinity(QWidget& window, const char* name,
                                          bool showWindow = true);
int verifyToolbarCaptureExclusion(ScreenshotFloatingToolPaletteWindow& toolbar, const char* name);
int verifyFollowAreaCaptureExclusion(QWidget& area, const std::function<void()>& move);

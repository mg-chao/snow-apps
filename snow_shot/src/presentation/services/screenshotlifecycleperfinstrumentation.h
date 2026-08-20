#ifndef SNOW_SHOT_SCREENSHOTLIFECYCLEPERFINSTRUMENTATION_H
#define SNOW_SHOT_SCREENSHOTLIFECYCLEPERFINSTRUMENTATION_H

#include <QString>

namespace snow_shot::presentation::screenshot_lifecycle_perf {
void configureTrace(const QString& path);
void appReady();
void beginCapture();
void mark(const QString& event);
[[nodiscard]] bool captureActive();
void capturePresented();
void captureReleased();
} // namespace snow_shot::presentation::screenshot_lifecycle_perf

#endif

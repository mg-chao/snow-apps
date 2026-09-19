#ifndef SNOW_SHOT_PLATFORM_WINDOWCAPTUREEXCLUSION_H
#define SNOW_SHOT_PLATFORM_WINDOWCAPTUREEXCLUSION_H

#include <QWidget>

#include <cstdint>
#include <optional>

namespace snow_shot::platform {

[[nodiscard]] bool setWindowExcludedFromCapture(QWidget* window, bool excluded);
[[nodiscard]] std::optional<std::uint32_t> captureWindowId(QWidget* window);

} // namespace snow_shot::platform

#endif // SNOW_SHOT_PLATFORM_WINDOWCAPTUREEXCLUSION_H

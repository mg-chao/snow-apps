#ifndef SNOW_SHOT_PRESENTATION_CAPTURE_SCREENSHOTCAPTUREPOLICY_H
#define SNOW_SHOT_PRESENTATION_CAPTURE_SCREENSHOTCAPTUREPOLICY_H

#include <cstdint>

namespace snow_shot::presentation::capture {

enum class ScreenshotApiMode : std::uint8_t { Auto, Dxgi, Wgc, Gdi };

[[nodiscard]] ScreenshotApiMode screenshotApiModeFromValue(const char* value) noexcept;
// Auto uses the cached desktop HDR decision, shared by normal and current-monitor screenshots.
[[nodiscard]] std::uint8_t nativeBackendForNormalScreenshot(ScreenshotApiMode mode) noexcept;
[[nodiscard]] ScreenshotApiMode resolveAutoScreenshotApiMode() noexcept;

} // namespace snow_shot::presentation::capture

#endif

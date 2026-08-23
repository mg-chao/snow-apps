#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTWINDOWCAPTURE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTWINDOWCAPTURE_H

#include "snow_shot/presentation/screenshottypes.h"

#include <QString>
#include <QtGlobal>

#include <memory>
#include <optional>

// Owns one native window capture session. Returned images retain the Rust
// frame through a QImage cleanup lease.
class ScreenshotWindowCapture final {
  public:
    explicit ScreenshotWindowCapture(quintptr nativeWindowHandle);
    ~ScreenshotWindowCapture();

    ScreenshotWindowCapture(const ScreenshotWindowCapture&) = delete;
    ScreenshotWindowCapture& operator=(const ScreenshotWindowCapture&) = delete;
    ScreenshotWindowCapture(ScreenshotWindowCapture&&) noexcept;
    ScreenshotWindowCapture& operator=(ScreenshotWindowCapture&&) noexcept;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool prepare();
    [[nodiscard]] std::optional<ScreenshotWindowCaptureFrame> capture();
    [[nodiscard]] QString errorMessage() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTWINDOWCAPTURE_H

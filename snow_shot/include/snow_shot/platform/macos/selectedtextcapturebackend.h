#ifndef SNOW_SHOT_PLATFORM_MACOS_SELECTEDTEXTCAPTUREBACKEND_H
#define SNOW_SHOT_PLATFORM_MACOS_SELECTEDTEXTCAPTUREBACKEND_H

#include "snow_shot/presentation/selectedtexttranslationcontroller.h"

#include <functional>

namespace snow_shot::platform::macos {

// The foreground PID is captured before the host activates a window. Only the AX read
// runs in a worker; injected accessors allow tests without reading another application.
struct SelectedTextAccess {
    std::function<bool()> accessibilityTrusted;
    std::function<qint64()> foregroundProcessId;
    std::function<presentation::SelectedTextCaptureResult(qint64)> readSelectedText;
};

[[nodiscard]] std::unique_ptr<presentation::SelectedTextCaptureBackend>
createSelectedTextCaptureBackend();
[[nodiscard]] std::unique_ptr<presentation::SelectedTextCaptureBackend>
createSelectedTextCaptureBackend(SelectedTextAccess access);

} // namespace snow_shot::platform::macos

#endif

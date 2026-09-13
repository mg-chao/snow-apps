#pragma once

#include "snow_shot/platform/windows/autostartregistration.h"

namespace snow_shot::platform {
using AutoStartRegistrationSnapshot = windows::AutoStartRegistrationSnapshot;

#ifdef Q_OS_MACOS
class AutoStartRegistration final {
  public:
    [[nodiscard]] static bool isSupported();
    [[nodiscard]] static AutoStartRegistrationSnapshot snapshot();
    [[nodiscard]] static bool matchesExpectedCommand();
    [[nodiscard]] static bool setEnabled(bool enabled, QString* error = nullptr);
    [[nodiscard]] static bool restore(const AutoStartRegistrationSnapshot& previous,
                                      QString* error = nullptr);
};
#else
using AutoStartRegistration = windows::AutoStartRegistration;
#endif
} // namespace snow_shot::platform

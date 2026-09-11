#pragma once

#include "snow_shot/presentation/globalmousemanager.h"

namespace snow_shot::platform::macos {
enum class GlobalMouseError : quint32 {
    AccessibilityPermission = 1,
    EventTapUnavailable,
    EventTapDisabled
};
[[nodiscard]] std::unique_ptr<presentation::GlobalMouseBackend> createGlobalMouseBackend();
} // namespace snow_shot::platform::macos

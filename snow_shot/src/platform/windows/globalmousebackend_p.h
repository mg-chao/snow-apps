#ifndef SNOW_SHOT_PLATFORM_WINDOWS_GLOBALMOUSEBACKEND_P_H
#define SNOW_SHOT_PLATFORM_WINDOWS_GLOBALMOUSEBACKEND_P_H

#include "snow_shot/presentation/globalmousemanager.h"

#include <qt_windows.h>

namespace snow_shot::presentation::detail {
struct GlobalMouseNativeApi {
    decltype(&SetWindowsHookExW) installHook = SetWindowsHookExW;
    decltype(&UnhookWindowsHookEx) removeHook = UnhookWindowsHookEx;
    decltype(&GetAsyncKeyState) keyState = GetAsyncKeyState;
    decltype(&SendInput) sendInput = SendInput;
    decltype(&CallNextHookEx) nextHook = CallNextHookEx;
    std::function<bool()> supported;
    decltype(&GetCursorPos) cursorPosition = GetCursorPos;
};

[[nodiscard]] std::unique_ptr<GlobalMouseBackend>
createGlobalMouseBackend(GlobalMouseNativeApi api);
} // namespace snow_shot::presentation::detail
#endif

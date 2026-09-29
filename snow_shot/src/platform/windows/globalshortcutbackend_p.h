#pragma once
#include "../../presentation/services/globalshortcutbackend_p.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <functional>
namespace snow_shot::presentation {
struct WindowsHotKeyInputApi {
    decltype(&RegisterHotKey) registerHotKey = RegisterHotKey;
    decltype(&UnregisterHotKey) unregisterHotKey = UnregisterHotKey;
    std::function<SHORT(int)> keyState = GetAsyncKeyState;
    std::function<bool()> available;
};
[[nodiscard]] std::unique_ptr<GlobalShortcutBackend>
createWindowsGlobalShortcutBackend(WindowsHotKeyInputApi api);
} // namespace snow_shot::presentation

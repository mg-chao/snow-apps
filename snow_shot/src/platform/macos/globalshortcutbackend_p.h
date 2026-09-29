#ifndef SNOW_SHOT_PLATFORM_MACOS_GLOBALSHORTCUTBACKEND_P_H
#define SNOW_SHOT_PLATFORM_MACOS_GLOBALSHORTCUTBACKEND_P_H

#include "../../presentation/services/globalshortcutbackend_p.h"

#include <Carbon/Carbon.h>
#include <ApplicationServices/ApplicationServices.h>
#include <functional>

namespace snow_shot::presentation {

// Keep system reservation lookup and registration together so tests can model
// disabled symbolic hotkeys without changing the user's Keyboard settings.
struct MacOSHotKeyApi {
    decltype(&CopySymbolicHotKeys) copySymbolicHotKeys = &CopySymbolicHotKeys;
    decltype(&RegisterEventHotKey) registerEventHotKey = &RegisterEventHotKey;
    decltype(&UnregisterEventHotKey) unregisterEventHotKey = &UnregisterEventHotKey;
    std::function<bool(CGKeyCode)> keyDown = [](CGKeyCode key) {
        return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, key);
    };
    std::function<bool()> inputAvailable = [] {
        CFDictionaryRef session = CGSessionCopyCurrentDictionary();
        if (!session)
            return false;
        const bool active = CFDictionaryGetValue(session, kCGSessionOnConsoleKey) == kCFBooleanTrue;
        CFRelease(session);
        return active;
    };
};

[[nodiscard]] std::unique_ptr<GlobalShortcutBackend>
createMacOSGlobalShortcutBackend(MacOSHotKeyApi api);

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PLATFORM_MACOS_GLOBALSHORTCUTBACKEND_P_H

#ifndef SNOW_SHOT_PLATFORM_MACOS_GLOBALSHORTCUTBACKEND_H
#define SNOW_SHOT_PLATFORM_MACOS_GLOBALSHORTCUTBACKEND_H

#include "snow_shot/presentation/globalshortcutmanager.h"

namespace snow_shot::platform::macos {
struct NativeShortcut {
    quint32 keyCode = 0;
    quint32 modifiers = 0;
    bool valid = false;
};

[[nodiscard]] NativeShortcut nativeShortcut(const QString& portableShortcut);
[[nodiscard]] std::unique_ptr<presentation::GlobalShortcutBackend> createGlobalShortcutBackend();
} // namespace snow_shot::platform::macos

#endif

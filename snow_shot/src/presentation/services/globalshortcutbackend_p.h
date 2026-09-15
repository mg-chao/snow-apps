#ifndef SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTBACKEND_P_H
#define SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTBACKEND_P_H

#include "snow_shot/presentation/globalshortcutmanager.h"

#include <memory>

namespace snow_shot::presentation {

[[nodiscard]] std::unique_ptr<GlobalShortcutBackend> createPlatformGlobalShortcutBackend();

#ifdef Q_OS_WIN
[[nodiscard]] std::unique_ptr<GlobalShortcutBackend> createWindowsGlobalShortcutBackend();
#endif
#ifdef Q_OS_MACOS
[[nodiscard]] std::unique_ptr<GlobalShortcutBackend> createMacOSGlobalShortcutBackend();
#endif

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_GLOBALSHORTCUTBACKEND_P_H

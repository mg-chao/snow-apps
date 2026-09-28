#ifndef SNOW_SHOT_PLATFORM_MACOS_WINDOWCURSORCOORDINATOR_H
#define SNOW_SHOT_PLATFORM_MACOS_WINDOWCURSORCOORDINATOR_H

class QWidget;

namespace snow_shot::platform::macos {
// Register a window family for its QWidget lifetime without creating a native surface.
// Current transient descendants are included; repeated registration is harmless.
void configureWindowCursorUpdates(QWidget* window);
} // namespace snow_shot::platform::macos

#endif

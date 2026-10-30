#ifndef SNOW_SHOT_PLATFORM_WINDOWS_SHELLCONTEXTMENUMODE_H
#define SNOW_SHOT_PLATFORM_WINDOWS_SHELLCONTEXTMENUMODE_H

namespace snow_shot::platform::windows {

// Keep this Qt-independent: the packaged Explorer command also consumes this mode.
enum class ShellContextMenuMode { Disabled, ImageAndDocumentFiles, ImageFiles };

} // namespace snow_shot::platform::windows

#endif

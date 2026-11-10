#ifndef SNOW_SHOT_PLATFORM_FILEMANAGERBACKEND_P_H
#define SNOW_SHOT_PLATFORM_FILEMANAGERBACKEND_P_H

#include <QString>

namespace snow_shot::platform::detail {

[[nodiscard]] bool isSystemFileManager(const QString& directory);
[[nodiscard]] bool selectInSystemFileManager(const QString& path);

} // namespace snow_shot::platform::detail

#endif

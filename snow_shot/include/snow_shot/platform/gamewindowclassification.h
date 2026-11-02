#ifndef SNOW_SHOT_PLATFORM_GAMEWINDOWCLASSIFICATION_H
#define SNOW_SHOT_PLATFORM_GAMEWINDOWCLASSIFICATION_H

#include <QJsonObject>
#include <QStringList>
#include <functional>

namespace snow_shot::platform {

// The lookup returns only full executable paths registered under this GameConfigStore index.
using GameConfigStoreLookup = std::function<QStringList(const QString& parentKey)>;

[[nodiscard]] QString gameConfigStoreParentKey(const QString& executablePath);
[[nodiscard]] bool isWindowsGameWindow(const QString& executablePath, const QString& windowClass,
                                       const GameConfigStoreLookup& registeredPaths);
[[nodiscard]] bool isMacOSGameApplication(const QJsonObject& bundleMetadata);

} // namespace snow_shot::platform

#endif // SNOW_SHOT_PLATFORM_GAMEWINDOWCLASSIFICATION_H

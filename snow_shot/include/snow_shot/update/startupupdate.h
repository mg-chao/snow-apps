#pragma once

#include "snow_shot/update/updateservice.h"

#include <chrono>
#include <functional>

namespace snow_shot::update {

[[nodiscard]] UpdateService::Options defaultUpdateServiceOptions();

// Internal updater relaunches skip the startup gate once, including after rollback.
[[nodiscard]] bool isStartupUpdateRelaunch(const QStringList& arguments);
[[nodiscard]] QStringList normalStartupArguments(QStringList arguments);
[[nodiscard]] QStringList startupUpdateRelaunchArguments(QStringList arguments);

enum class StartupUpdateResult { ContinueStartup, ExitForUpdate };

// Keep Qt and single-instance IPC responsive while the updater prepares its handoff.
// Only a committed handoff ends startup; errors and cancellation continue normally.
[[nodiscard]] StartupUpdateResult
runStartupUpdate(UpdateService& service, const std::function<bool()>& flush,
                 const std::function<QStringList()>& relaunchArguments,
                 std::chrono::milliseconds timeout = std::chrono::seconds(210));

} // namespace snow_shot::update
